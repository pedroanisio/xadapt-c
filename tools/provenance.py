#!/usr/bin/env python3
"""What the codec is made of and how it was built: the SBOM, the build
manifest, and the check that a build reproduces (DLV-2).

    python3 tools/provenance.py sbom [--output sbom.spdx.json]
    python3 tools/provenance.py manifest [--build adpat/c/build] [--output build-manifest.json]
    python3 tools/provenance.py reproduce [--manifest build-manifest.json] [--build build-reproduce]

`sbom` writes an SPDX 2.3 document for the codec as the tree holds it: the
package, its licence, every source file by SHA-256 and SHA-1, and what it
depends on at run time (the platform's C library, optionally POSIX
threads), at build time (a C99 compiler) and under test (Python 3, the
corpus fixtures with their own licences). It reads the version from
cadapt.c and the corpus manifest for the rows, so `make sbom` after a
change keeps it true; the test compares the committed file with a fresh
one.

`manifest` records a build already made: the sources' hashes, the
compiler's version line and the flags the Makefile uses, the host, and
the hash and size of every output. `reproduce` builds the same sources
again in a scratch directory with the same Makefile and compares the
bytes with the manifest's: identical bytes are the proof that the build
is reproducible on that toolchain; a differing toolchain is reported
rather than blamed on the bytes. Standard library only.

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14,
with Claude Opus 5 (Claude Code).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODEC = ROOT / "adpat" / "c"
SOURCES = sorted(path.name for path in CODEC.iterdir() if path.suffix in (".c", ".h"))
OUTPUTS = ["cadapt", "libadapt.a"]
COPYRIGHT = "Copyright (c) 2026 Pedro Anisio de Luna e Silva"
LICENCE = "MIT"


def digest(path: Path, algorithm: str = "sha256") -> str:
    return hashlib.new(algorithm, path.read_bytes()).hexdigest()


def version() -> str:
    text = (CODEC / "cadapt.c").read_text(encoding="utf-8")
    match = re.search(r'#define CADAPT_VERSION "([^"]+)"', text)
    if match is None:
        raise SystemExit("cadapt.c does not define CADAPT_VERSION")
    return match.group(1)


def commit() -> str | None:
    try:
        out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def make_variables() -> dict[str, str]:
    """CC, CFLAGS and LDFLAGS exactly as the codec's Makefile resolves them."""
    out = subprocess.run(
        ["make", "-s", "--no-print-directory", "-C", str(CODEC), "--eval=cadapt-variables: ; @printf '%s\\n%s\\n%s\\n' '$(CC)' '$(CFLAGS)' '$(LDFLAGS)'", "cadapt-variables"],
        capture_output=True,
        text=True,
        check=True,
    )
    cc, cflags, ldflags = (out.stdout.rstrip("\n").split("\n") + ["", "", ""])[:3]
    return {"CC": cc.strip(), "CFLAGS": " ".join(cflags.split()), "LDFLAGS": " ".join(ldflags.split())}


def compiler_line(cc: str) -> str:
    try:
        out = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=30)
    except OSError:
        return f"{cc} (version unknown)"
    return out.stdout.splitlines()[0].strip() if out.stdout else f"{cc} (version unknown)"


# ---- the SBOM ----------------------------------------------------------------


def sbom() -> dict:
    ver = version()
    files = []
    file_ids = []
    for name in SOURCES:
        path = CODEC / name
        spdx_id = f"SPDXRef-File-{name.replace('.', '-').replace('_', '-')}"
        file_ids.append(spdx_id)
        files.append(
            {
                "fileName": f"./adpat/c/{name}",
                "SPDXID": spdx_id,
                "fileTypes": ["SOURCE"],
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": digest(path, "sha1")},
                    {"algorithm": "SHA256", "checksumValue": digest(path, "sha256")},
                ],
                "licenseConcluded": LICENCE,
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": COPYRIGHT,
            }
        )
    # SPDX's package verification code: SHA-1 over the sorted file SHA-1s
    sha1s = sorted(c["checksumValue"] for f in files for c in f["checksums"] if c["algorithm"] == "SHA1")
    verification = hashlib.sha1("".join(sha1s).encode("ascii")).hexdigest()
    corpus_manifest = ROOT / "static" / "corpus" / "scripts" / "manifest.json"
    rows = json.loads(corpus_manifest.read_text(encoding="utf-8"))["rows"] if corpus_manifest.exists() else []
    namespace = uuid.uuid5(uuid.NAMESPACE_URL, f"cadapt/{ver}/{verification}")
    packages = [
        {
            "name": "adapt",
            "SPDXID": "SPDXRef-Package-adapt",
            "versionInfo": ver,
            "supplier": "Person: Pedro Anisio de Luna e Silva (pedroanisio@faz.ai)",
            "originator": "Person: Pedro Anisio de Luna e Silva (pedroanisio@faz.ai)",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": True,
            "packageVerificationCode": {"packageVerificationCodeValue": verification},
            "hasFiles": file_ids,
            "licenseConcluded": LICENCE,
            "licenseDeclared": LICENCE,
            "copyrightText": COPYRIGHT,
            "summary": "The adapt self-adaptive lossless codec in C99: libadapt.a and the cadapt command line.",
            "primaryPackagePurpose": "LIBRARY",
        },
        {
            "name": "libc",
            "SPDXID": "SPDXRef-Package-libc",
            "versionInfo": "NOASSERTION",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "comment": "The platform's C standard library, the codec's only run-time dependency: glibc "
            "(LGPL-2.1-or-later) on the reference host, musl (MIT) or another where built. POSIX threads "
            "for the encoder's proof are part of it and optional (THREADS=).",
            "primaryPackagePurpose": "LIBRARY",
        },
        {
            "name": "c-compiler",
            "SPDXID": "SPDXRef-Package-c-compiler",
            "versionInfo": "NOASSERTION",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "comment": "Any C99 compiler; GCC 13 is the reference, Clang 18 is exercised; build-manifest.json names the one a build used.",
            "primaryPackagePurpose": "APPLICATION",
        },
        {
            "name": "python3",
            "SPDXID": "SPDXRef-Package-python3",
            "versionInfo": "3.10 or later",
            "downloadLocation": "https://www.python.org/",
            "filesAnalyzed": False,
            "licenseConcluded": "PSF-2.0",
            "licenseDeclared": "PSF-2.0",
            "copyrightText": "NOASSERTION",
            "comment": "The CLI tests, the coverage gate, the benchmark and this tool; standard library only.",
            "primaryPackagePurpose": "APPLICATION",
        },
        {
            "name": "enwik8",
            "SPDXID": "SPDXRef-Package-enwik8",
            "versionInfo": "2006",
            "downloadLocation": "https://mattmahoney.net/dc/enwik8.zip",
            "filesAnalyzed": False,
            "licenseConcluded": "CC-BY-SA-3.0",
            "licenseDeclared": "CC-BY-SA-3.0",
            "copyrightText": "Wikipedia contributors",
            "comment": "The benchmark corpus (static/corpus/enwik8.zip) and text8 derived from it; the text, "
            "segmented, cyrillic and cjk test vectors are cut from Wikipedia text under the same terms.",
            "primaryPackagePurpose": "DATA",
        },
        {
            "name": "corpus-scripts",
            "SPDXID": "SPDXRef-Package-corpus-scripts",
            "versionInfo": "NOASSERTION",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "CC-BY-SA-4.0",
            "licenseDeclared": "CC-BY-SA-4.0",
            "copyrightText": "Wikipedia contributors",
            "comment": "The per-script corpus rows under static/corpus/scripts/ (" + ", ".join(r["row"] for r in rows)
            + "), fetched by bench/fetch_scripts.py; the manifest there names every article and revision.",
            "primaryPackagePurpose": "DATA",
        },
    ]
    relationships = [
        {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package-adapt"},
        {"spdxElementId": "SPDXRef-Package-adapt", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-libc"},
        {"spdxElementId": "SPDXRef-Package-c-compiler", "relationshipType": "BUILD_DEPENDENCY_OF", "relatedSpdxElement": "SPDXRef-Package-adapt"},
        {"spdxElementId": "SPDXRef-Package-python3", "relationshipType": "TEST_DEPENDENCY_OF", "relatedSpdxElement": "SPDXRef-Package-adapt"},
        {"spdxElementId": "SPDXRef-Package-enwik8", "relationshipType": "TEST_DEPENDENCY_OF", "relatedSpdxElement": "SPDXRef-Package-adapt"},
        {"spdxElementId": "SPDXRef-Package-corpus-scripts", "relationshipType": "TEST_DEPENDENCY_OF", "relatedSpdxElement": "SPDXRef-Package-adapt"},
    ]
    relationships += [
        {"spdxElementId": "SPDXRef-Package-adapt", "relationshipType": "CONTAINS", "relatedSpdxElement": spdx_id}
        for spdx_id in file_ids
    ]
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"adapt-{ver}",
        "documentNamespace": f"urn:uuid:{namespace}",
        "creationInfo": {
            "created": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
            "creators": ["Tool: tools/provenance.py", "Person: Pedro Anisio de Luna e Silva (pedroanisio@faz.ai)"],
            "comment": "Generated by `make sbom`; tools/test_provenance.py fails when the committed file is stale.",
        },
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }


# ---- the build manifest ---------------------------------------------------------


def manifest(build: Path) -> dict:
    variables = make_variables()
    outputs = {}
    for name in OUTPUTS:
        path = build / name
        if not path.exists():
            raise SystemExit(f"{path} is missing: build first (make)")
        outputs[str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)] = {
            "sha256": digest(path),
            "bytes": path.stat().st_size,
        }
    return {
        "schema": "cadapt.build-manifest.v1",
        "version": version(),
        "commit": commit(),
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "sources": {f"adpat/c/{name}": digest(CODEC / name) for name in SOURCES},
        "makefile_sha256": digest(CODEC / "Makefile"),
        "toolchain": {
            "cc": compiler_line(variables["CC"]),
            "cc_command": variables["CC"],
            "cflags": variables["CFLAGS"],
            "ldflags": variables["LDFLAGS"],
            "make": subprocess.run(["make", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
        },
        "host": {"platform": platform.platform(), "machine": platform.machine()},
        "outputs": outputs,
    }


def reproduce(manifest_path: Path, build_name: str) -> int:
    record = json.loads(manifest_path.read_text(encoding="utf-8"))
    problems = []
    for rel, sha in record["sources"].items():
        if digest(ROOT / rel) != sha:
            problems.append(f"{rel} differs from the manifest's source")
    if digest(CODEC / "Makefile") != record.get("makefile_sha256"):
        problems.append("adpat/c/Makefile differs from the manifest's")
    if problems:
        print("\n".join(problems))
        print("not the manifest's sources: nothing to reproduce")
        return 1
    variables = make_variables()
    toolchain_note = ""
    if compiler_line(variables["CC"]) != record["toolchain"]["cc"] or variables["CFLAGS"] != record["toolchain"]["cflags"]:
        toolchain_note = (
            f"toolchain differs from the manifest's: {compiler_line(variables['CC'])!r} with {variables['CFLAGS']!r}"
            f" against {record['toolchain']['cc']!r} with {record['toolchain']['cflags']!r}"
        )
    build = CODEC / build_name
    if build.exists():
        shutil.rmtree(build)
    try:
        made = subprocess.run(["make", "-s", "-C", str(CODEC), f"BUILD={build_name}", "all"], capture_output=True, text=True)
        if made.returncode != 0:
            print(made.stderr)
            print("the build failed")
            return 1
        mismatches = []
        for rel, want in record["outputs"].items():
            fresh = build / Path(rel).name
            got = digest(fresh)
            if got != want["sha256"]:
                mismatches.append(f"{rel} differs: built {got[:16]}…, manifest {want['sha256'][:16]}…")
    finally:
        shutil.rmtree(build, ignore_errors=True)
    if toolchain_note:
        print(toolchain_note)
    if mismatches:
        print("\n".join(mismatches))
        print("not reproduced" + (" (the toolchain differs, which explains it)" if toolchain_note else ""))
        return 1
    print(f"reproduced: {len(record['outputs'])} outputs match the manifest byte for byte")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    s = commands.add_parser("sbom", help="write the SPDX document")
    s.add_argument("--output", type=Path, default=ROOT / "sbom.spdx.json")
    m = commands.add_parser("manifest", help="record a build already made")
    m.add_argument("--build", type=Path, default=CODEC / "build")
    m.add_argument("--output", type=Path, default=ROOT / "build-manifest.json")
    r = commands.add_parser("reproduce", help="rebuild and compare with a manifest")
    r.add_argument("--manifest", type=Path, default=ROOT / "build-manifest.json")
    r.add_argument("--build", default="build-reproduce", help="scratch build directory name under adpat/c, removed afterwards")
    args = parser.parse_args(argv)
    if args.command == "sbom":
        args.output.write_text(json.dumps(sbom(), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {args.output}")
        return 0
    if args.command == "manifest":
        build = args.build if args.build.is_absolute() else (ROOT / args.build)
        args.output.write_text(json.dumps(manifest(build), indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.output}")
        return 0
    return reproduce(args.manifest, args.build)


if __name__ == "__main__":
    sys.exit(main())
