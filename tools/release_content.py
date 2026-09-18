#!/usr/bin/env python3
"""Validate classified release inputs, staged blobs and exact package contents.

The inventory is an explicit list of engineering assets. Content classification
also requires human review: deterministic checks cannot establish a document's
purpose or the correctness of its claims.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import zipfile
import stat

ROOT = Path(__file__).resolve().parent.parent
INVENTORY = "tools/release_assets.json"
SOURCE_RECORD = "release-source.json"
PRIVATE_NAMES = {
    ".agent-tasks",
    ".repo",
    ".worktrees",
    "_tmp",
    "_study",
    "CLAUDE.md",
    "AGENTS.md",
    "SKILL.md",
}
PRIVATE_PATH = re.compile(
    rb"(?:/home/|/Users/)[A-Za-z0-9_.-]+/[^\s\x00\"'<>]*|[A-Za-z]:\\Users\\[^\s\x00\"']+"
)
PRIVATE_REFERENCE = re.compile(
    rb"(?:\b(?:doc-hygiene-report|drift-risk-map|performance-audit|performance-opportunities|throughput-study)(?:-[a-z0-9]+){0,3}-\d{4}-\d{2}-\d{2}|(?:_tmp|_study|\.agent-tasks|\.repo)/|https?://[^\s/]+/(?:Workspace/)?AI-Memory/|(?:file|vscode)://[^\s\"'<>])",
    re.IGNORECASE,
)
SECRET = re.compile(
    rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|\b(?:ghp_|github_pat_)[A-Za-z0-9_]{30,}"
)
MAX_MEMBERS = 5000
MAX_BYTES = 512 * 1024 * 1024


class ContentError(ValueError):
    """An input or package violates the release contract."""


def safe_path(name: str) -> bool:
    path = PurePosixPath(name)
    return (
        bool(name)
        and not path.is_absolute()
        and ".." not in path.parts
        and "\\" not in name
        and not re.match(r"^[A-Za-z]:", name)
    )


def load_inventory(data: bytes) -> dict:
    inventory = json.loads(data)
    if (
        not isinstance(inventory, dict)
        or inventory.get("schema") != "cadapt.release-assets.v1"
    ):
        raise ContentError("INVENTORY: unsupported schema")
    if not isinstance(inventory.get("source"), dict) or not isinstance(
        inventory.get("binary"), list
    ):
        raise ContentError("INVENTORY: source must be a mapping and binary a list")
    if not all(
        isinstance(name, str) and safe_path(name) for name in inventory["binary"]
    ):
        raise ContentError("INVENTORY: binary members must be safe relative paths")
    for name, entry in inventory["source"].items():
        if not isinstance(entry, dict):
            raise ContentError(f"INVENTORY {name}: expected an asset classification")
        if not safe_path(name) or entry.get("classification") not in {
            "production",
            "documentation",
        }:
            raise ContentError(
                f"INVENTORY {name}: expected a relative engineering asset"
            )
        use = entry.get("product_use")
        if use is not None:
            consumer = (
                inventory["source"].get(use.get("consumer"))
                if isinstance(use, dict) and isinstance(use.get("consumer"), str)
                else None
            )
            if (
                not isinstance(use, dict)
                or set(use) != {"consumer", "reason"}
                or entry["classification"] != "production"
                or not isinstance(use["reason"], str)
                or not use["reason"].strip()
                or use["consumer"] == name
                or not isinstance(consumer, dict)
                or consumer.get("classification") != "production"
            ):
                raise ContentError(
                    f"PRODUCT_USE {name}: declare a distinct production consumer and reviewed rationale"
                )
        if set(PurePosixPath(name).parts) & PRIVATE_NAMES and use is None:
            raise ContentError(
                f"ARTIFACT {name}: relocate working material or document its reviewed product use"
            )
        if entry.get("content") == "fixture" and not name.startswith(
            ("adpat/c/tests/vectors/", "static/corpus/")
        ):
            raise ContentError(
                f"FIXTURE {name}: fixture exceptions are limited to the test corpus"
            )
        for exception in entry.get("exceptions", []):
            if (
                set(exception) != {"rule", "line_sha256", "reason"}
                or exception["rule"] not in {"HOST_PATH", "PRIVATE_REFERENCE", "SECRET"}
                or not re.fullmatch(r"[0-9a-f]{64}", exception["line_sha256"])
                or not exception["reason"].strip()
            ):
                raise ContentError(
                    f"EXCEPTION {name}: use a rule, exact line digest and reason"
                )
        if entry.get("content") == "fixture" and not re.fullmatch(
            r"[0-9a-f]{64}", entry.get("sha256", "")
        ):
            raise ContentError(f"FIXTURE {name}: declare the reviewed fixture digest")
    return inventory


def content_errors(name: str, data: bytes, entry: dict) -> list[str]:
    errors = []
    if entry.get("content") == "fixture":
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            errors.append(
                f"FIXTURE {name}: bytes differ from the reviewed fixture; inspect and update its digest"
            )
        return errors
    for rule, pattern in (
        ("HOST_PATH", PRIVATE_PATH),
        ("PRIVATE_REFERENCE", PRIVATE_REFERENCE),
        ("SECRET", SECRET),
    ):
        for match in pattern.finditer(data):
            line = data[: match.start()].count(b"\n") + 1
            # Exact reviewed negative-test lines are scoped by their digest.
            exceptions = entry.get("exceptions", [])
            line_hash = hashlib.sha256(data.splitlines()[line - 1]).hexdigest()
            if any(
                e.get("rule") == rule
                and e.get("line_sha256") == line_hash
                and e.get("reason")
                for e in exceptions
            ):
                continue
            errors.append(
                f"{rule} {name}:{line}: prohibited content (redacted); remove the reference or use a reviewed test exception"
            )
    if entry.get("classification") == "documentation" and re.search(
        rb"<(?:URL|name)>", data
    ):
        errors.append(
            f"PLACEHOLDER {name}: replace submission placeholders with complete supported guidance"
        )
    return errors


def check_files(
    files: dict[str, bytes], inventory: dict, *, source_record: bool = False
) -> list[str]:
    expected = inventory["source"]
    actual = set(files) - ({SOURCE_RECORD} if source_record else set())
    errors = [
        f"UNCLASSIFIED {p}: classify the asset explicitly or relocate it"
        for p in sorted(actual - set(expected))
    ]
    errors += [
        f"MISSING {p}: include the required release input"
        for p in sorted(set(expected) - actual)
    ]
    for name in sorted(actual & set(expected)):
        errors.extend(content_errors(name, files[name], expected[name]))
    if source_record and SOURCE_RECORD in files:
        record = json.loads(files[SOURCE_RECORD])
        hashes = {
            p: hashlib.sha256(b).hexdigest()
            for p, b in files.items()
            if p != SOURCE_RECORD
        }
        if (
            not isinstance(record, dict)
            or record.get("schema") != "cadapt.release-source.v1"
            or (
                record.get("commit") is not None
                and not re.fullmatch(r"[0-9a-f]{40}", str(record["commit"]))
            )
            or record.get("files") != hashes
        ):
            errors.append(
                f"SOURCE_HASH {SOURCE_RECORD}: source payload differs from its recorded inventory"
            )
    return errors


def context_errors(root: Path, inventory: dict | None = None) -> list[str]:
    """Inspect ignored state without rejecting explicitly reviewed product assets."""
    assets = (inventory or {}).get("source", {})
    errors = []
    for name in sorted(PRIVATE_NAMES):
        path = root / name
        if not path.exists() and not path.is_symlink():
            continue
        members = (
            list(path.rglob("*")) if path.is_dir() and not path.is_symlink() else [path]
        )
        files = [
            member for member in members if not member.is_dir() or member.is_symlink()
        ]
        if not files or any(
            member.is_symlink()
            or not member.is_file()
            or not assets.get(member.relative_to(root).as_posix(), {}).get(
                "product_use"
            )
            for member in files
        ):
            errors.append(
                f"WORKSPACE {name}: relocate working material outside the source checkout"
            )
    return errors


def repository_files(root: Path, *, staged: bool = False) -> dict[str, bytes]:
    cmd = (
        ["git", "ls-files", "-z", "--stage"]
        if staged
        else ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    )
    result = subprocess.run(cmd, cwd=root, capture_output=True)
    if result.returncode:
        if staged:
            raise ContentError("INDEX: staging requires a Git checkout")
        inventory = load_inventory((root / INVENTORY).read_bytes())
        return {
            p: (root / p).read_bytes()
            for p in inventory["source"]
            if (root / p).is_file()
        }
    files = {}
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        if staged:
            metadata, path = raw.decode().split("\t", 1)
            mode, oid, stage = metadata.split()
            if stage != "0" or mode not in ("100644", "100755"):
                raise ContentError(
                    f"INDEX {path}: unresolved stage or unsupported file type"
                )
            files[path] = subprocess.check_output(
                ["git", "cat-file", "blob", oid], cwd=root
            )
        else:
            path = raw.decode()
            target = root / path
            if target.is_symlink():
                raise ContentError(
                    f"SYMLINK {path}: release inputs must be regular files"
                )
            if target.is_file():
                files[path] = target.read_bytes()
    return files


def export(root: Path, destination: Path, revision: str | None = None) -> None:
    if destination.exists() and any(destination.iterdir()):
        raise ContentError("EXPORT: destination must be empty")
    if revision:
        commit = subprocess.check_output(
            ["git", "rev-parse", revision + "^{commit}"], cwd=root, text=True
        ).strip()
        raw = subprocess.check_output(
            ["git", "show", f"{commit}:{INVENTORY}"], cwd=root
        )
        inventory = load_inventory(raw)
        archive = subprocess.check_output(
            ["git", "archive", commit, *inventory["source"]], cwd=root
        )
        with tarfile.open(fileobj=io.BytesIO(archive)) as tf:
            files = {
                m.name: tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()
            }
            executable = {m.name for m in tf.getmembers() if m.mode & 0o111}
    else:
        commit = None
        inventory = load_inventory((root / INVENTORY).read_bytes())
        files = repository_files(root)
        executable = {p for p in files if (root / p).stat().st_mode & 0o111}
    errors = check_files(files, inventory)
    if errors:
        raise ContentError("\n".join(errors))
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755 if name in executable else 0o644)
    record = {
        "schema": "cadapt.release-source.v1",
        "commit": commit,
        "files": {p: hashlib.sha256(b).hexdigest() for p, b in sorted(files.items())},
    }
    (destination / SOURCE_RECORD).write_text(json.dumps(record, indent=2) + "\n")


def zip_files(path: Path) -> dict[str, bytes]:
    """Read a flat LTCB ZIP without extracting paths or trusting link metadata."""
    files = {}
    total = 0
    with zipfile.ZipFile(path) as archive:
        for member in archive.infolist():
            total += member.file_size
            if len(files) >= MAX_MEMBERS or total > MAX_BYTES:
                raise ContentError("ARCHIVE: member or byte limit exceeded")
            mode = member.external_attr >> 16
            if (
                not safe_path(member.filename)
                or member.filename in files
                or stat.S_ISLNK(mode)
                or member.is_dir()
            ):
                raise ContentError(
                    f"ARCHIVE {member.filename}: unsafe or duplicate ZIP member"
                )
            files[member.filename] = archive.read(member)
    return files


def archive_files(path: Path) -> dict[str, bytes]:
    files = {}
    seen = set()
    total = 0
    with tarfile.open(path) as tf:
        for member in tf:
            total += member.size
            if len(seen) >= MAX_MEMBERS or total > MAX_BYTES:
                raise ContentError("ARCHIVE: member or byte limit exceeded")
            if not safe_path(member.name) or member.name in seen:
                raise ContentError(f"ARCHIVE {member.name}: unsafe or duplicate member")
            seen.add(member.name)
            if member.isdir():
                continue
            if not member.isfile():
                raise ContentError(
                    f"ARCHIVE {member.name}: links and special files are not permitted"
                )
            files[member.name] = tf.extractfile(member).read()
    roots = {PurePosixPath(p).parts[0] for p in files}
    if len(roots) != 1:
        raise ContentError("ARCHIVE: expected one package root")
    if any(len(PurePosixPath(p).parts) < 2 for p in files):
        raise ContentError("ARCHIVE: file outside package root")
    return {p.split("/", 1)[1]: data for p, data in files.items()}


def payload_record(package: Path, revision: str) -> dict:
    return {
        "schema": "cadapt.installed-payload.v1",
        "commit": revision,
        "stage": "installed-after-strip",
        "files": {
            p.relative_to(package).as_posix(): {
                "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                "bytes": p.stat().st_size,
            }
            for p in sorted(package.rglob("*"))
            if p.is_file() and p.name != "payload-manifest.json"
        },
    }


def check_archive(path: Path, inventory: dict, kind: str) -> list[str]:
    if kind == "ltcb":
        files = zip_files(path)
        expected = {"cadapt", "README-ltcb.txt", "LICENSE"}
        errors = [
            f"LTCB_PAYLOAD {name}: unexpected ZIP member"
            for name in sorted(set(files) - expected)
        ]
        errors += [
            f"LTCB_PAYLOAD {name}: required ZIP member absent"
            for name in sorted(expected - set(files))
        ]
        for name, data in files.items():
            errors.extend(content_errors(name, data, {"classification": "production"}))
        return errors
    files = archive_files(path)
    if kind == "source":
        if SOURCE_RECORD not in files:
            return [f"MISSING {SOURCE_RECORD}: package requires source identity"]
        embedded = load_inventory(files.get(INVENTORY, b"{}"))
        if embedded != inventory:
            return [
                "INVENTORY: source archive inventory differs from the selected revision"
            ]
        return check_files(files, embedded, source_record=True)
    expected = set(inventory["binary"])
    errors = [
        f"PAYLOAD {p}: unexpected installed file" for p in sorted(set(files) - expected)
    ]
    errors += [
        f"PAYLOAD {p}: missing installed file" for p in sorted(expected - set(files))
    ]
    record = json.loads(files.get("payload-manifest.json", b"{}"))
    hashes = {
        p: {"sha256": hashlib.sha256(b).hexdigest(), "bytes": len(b)}
        for p, b in files.items()
        if p != "payload-manifest.json"
    }
    if (
        not isinstance(record, dict)
        or record.get("schema") != "cadapt.installed-payload.v1"
        or record.get("files") != hashes
        or not re.fullmatch(r"[0-9a-f]{40}", str(record.get("commit", "")))
        or record.get("stage") != "installed-after-strip"
    ):
        errors.append(
            "PAYLOAD_HASH payload-manifest.json: installed bytes or revision do not match"
        )
    if "build-manifest.json" in expected:
        build = json.loads(files.get("build-manifest.json", b"{}"))
        if (
            not isinstance(build, dict)
            or not isinstance(record, dict)
            or build.get("commit") != record.get("commit")
            or build.get("stage") != "build-before-install-and-strip"
        ):
            errors.append(
                "PROVENANCE build-manifest.json: build and installed revisions must match"
            )
    for name, data in files.items():
        errors.extend(content_errors(name, data, {"classification": "production"}))
    return errors


def assemble(artifacts: Path, destination: Path, inventory: dict) -> None:
    """Map platform build artifacts to verified, collision-free download names."""
    if destination.exists() and any(destination.iterdir()):
        raise ContentError("PUBLISH: destination must be empty")
    directories = sorted(artifacts.iterdir())
    if not directories:
        raise ContentError("PUBLISH: no platform artifacts")
    published = {}
    identity = None
    source_identity = None
    platforms = set()

    def digest(data):
        return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}

    for directory in directories:
        if directory.is_symlink() or not directory.is_dir():
            raise ContentError("PUBLISH: platform inputs must be regular directories")
        if any(p.is_symlink() or not p.is_file() for p in directory.iterdir()):
            raise ContentError("PUBLISH: platform artifacts must be regular files")
        manifest = json.loads((directory / "release-manifest.json").read_bytes())
        if (
            not isinstance(manifest, dict)
            or manifest.get("schema") != "cadapt.release-manifest.v1"
        ):
            raise ContentError("PUBLISH: unsupported release manifest")
        version, platform, revision = (
            manifest.get(k, "") for k in ("version", "platform", "commit")
        )
        if (
            not all(isinstance(v, str) for v in (version, platform, revision))
            or not re.fullmatch(r"[0-9][A-Za-z0-9.+-]*", version)
            or not re.fullmatch(r"[A-Za-z0-9_-]+", platform)
            or not re.fullmatch(r"[0-9a-f]{40}", revision)
        ):
            raise ContentError("PUBLISH: invalid version, platform or commit")
        if identity is not None and identity != (version, revision):
            raise ContentError(
                "PUBLISH: all platforms must describe the same version and commit"
            )
        if platform in platforms:
            raise ContentError("PUBLISH: duplicate platform")
        identity = (version, revision)
        platforms.add(platform)
        source = f"cadapt-{version}-src.tar.gz"
        binary = f"cadapt-{version}-{platform}.tar.gz"
        expected = {
            source,
            binary,
            "sbom.spdx.json",
            "build-manifest.json",
            "NOTICE",
            "SHA256SUMS",
        }
        entries = manifest.get("artifacts")
        if (
            not isinstance(entries, dict)
            or set(entries) != expected
            or {p.name for p in directory.iterdir()}
            != expected | {"release-manifest.json"}
        ):
            raise ContentError("PUBLISH: missing or unexpected platform artifact")
        for name in expected:
            if digest((directory / name).read_bytes()) != entries[name]:
                raise ContentError(
                    f"PUBLISH_HASH {name}: artifact differs from its build manifest"
                )
        for name, kind in ((source, "source"), (binary, "binary")):
            errors = check_archive(directory / name, inventory, kind)
            if errors:
                raise ContentError("\n".join(errors))
        source_record = json.loads(archive_files(directory / source)[SOURCE_RECORD])
        installed = archive_files(directory / binary)
        payload = json.loads(installed["payload-manifest.json"])
        for sidecar in ("build-manifest.json", "sbom.spdx.json", "NOTICE"):
            if (
                sidecar in installed
                and installed[sidecar] != (directory / sidecar).read_bytes()
            ):
                raise ContentError(
                    f"PUBLISH {sidecar}: sidecar differs from installed package"
                )
        build = json.loads((directory / "build-manifest.json").read_bytes())
        if (
            source_record.get("commit") != revision
            or payload.get("commit") != revision
            or not isinstance(build, dict)
            or build.get("commit") != revision
        ):
            raise ContentError(
                "PUBLISH: source, build and installed revision must agree"
            )
        if source_identity is not None and source_record != source_identity:
            raise ContentError("PUBLISH: platform source payloads differ")
        source_identity = source_record
        mapped = {}
        for name in sorted(expected - {"SHA256SUMS"}):
            data = (directory / name).read_bytes()
            if not name.endswith(".tar.gz"):
                errors = content_errors(name, data, {"classification": "production"})
                if errors:
                    raise ContentError("\n".join(errors))
            target = {
                "sbom.spdx.json": f"sbom-{platform}.spdx.json",
                "build-manifest.json": f"build-manifest-{platform}.json",
            }.get(name, name)
            if target in published and published[target] != data and name != source:
                raise ContentError(f"PUBLISH {target}: conflicting shared artifact")
            # Source trees were fully verified above. Tar metadata may differ;
            # publish the first archive and point every manifest at its bytes.
            published.setdefault(target, data)
            mapped[target] = digest(published[target])
        manifest["artifacts"] = mapped
        manifest["stage"] = "published-assets"
        data = (json.dumps(manifest, indent=2) + "\n").encode()
        errors = content_errors(
            "release-manifest.json", data, {"classification": "production"}
        )
        if errors:
            raise ContentError("\n".join(errors))
        published[f"release-manifest-{platform}.json"] = data
    published["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(published.items())
    ).encode()
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in published.items():
        (destination / name).write_bytes(data)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("tree")
    commands.add_parser("staged")
    exp = commands.add_parser("export")
    exp.add_argument("--destination", type=Path, required=True)
    exp.add_argument("--rev")
    arc = commands.add_parser("archive")
    arc.add_argument("path", type=Path)
    arc.add_argument("--kind", choices=["source", "binary", "ltcb"], required=True)
    publication = commands.add_parser("assemble")
    publication.add_argument("artifacts", type=Path)
    publication.add_argument("--destination", type=Path, required=True)
    payload = commands.add_parser("payload")
    payload.add_argument("directory", type=Path)
    payload.add_argument("--revision", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            export(args.root, args.destination, args.rev)
            errors = []
        elif args.command == "assemble":
            assemble(
                args.artifacts,
                args.destination,
                load_inventory((args.root / INVENTORY).read_bytes()),
            )
            errors = []
        elif args.command == "payload":
            (args.directory / "payload-manifest.json").write_text(
                json.dumps(payload_record(args.directory, args.revision), indent=2)
                + "\n"
            )
            errors = []
        elif args.command == "archive":
            errors = check_archive(
                args.path,
                load_inventory((args.root / INVENTORY).read_bytes()),
                args.kind,
            )
        else:
            files = repository_files(args.root, staged=args.command == "staged")
            inventory = load_inventory(files.get(INVENTORY, b"{}"))
            errors = check_files(files, inventory)
            if args.command == "tree":
                errors.extend(context_errors(args.root, inventory))
        if errors:
            print("\n".join(errors))
            return 1
    except (
        ContentError,
        OSError,
        ValueError,
        KeyError,
        subprocess.SubprocessError,
        tarfile.TarError,
        zipfile.BadZipFile,
    ) as exc:
        print(f"RELEASE_CONTENT: {exc}")
        return 1
    print("release content: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
