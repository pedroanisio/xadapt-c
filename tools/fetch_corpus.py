#!/usr/bin/env python3
"""Fetch the mixed acceptance corpus DLV-3 measures against, reproducibly.

The codec is a general tool; enwik8 and enwik9 are benchmark rows, not the
judge of a default or a lever (`DLV-3` in docs/CONSTRAINTS.md). Defaults
and levers are accepted on this corpus: the Silesia corpus (text, an
executable, a medical image, a database, office documents, source, XML,
an X-ray, a star catalogue, a chemical database — 212 MB in twelve files)
and the Canterbury corpus (eleven small files, 4 KB to 1 MB: prose, HTML,
C, Lisp, a spreadsheet, a fax image, an executable, a manual page).

The files are fetched from their homes and unpacked under
static/corpus/mixed/, which is git-ignored: they are not this project's
to redistribute (`DLV-4`), so nothing but this script and the manifest is
committed. The manifest pins the archives and every file by SHA-256 and
size; a fetch that does not match it fails rather than measuring
something else.

    python3 tools/fetch_corpus.py                 # both corpora into static/corpus/mixed/
    python3 tools/fetch_corpus.py --only silesia
    python3 tools/fetch_corpus.py --check         # verify what is on disk against the manifest

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-15,
with Claude Opus 5 (Claude Code).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tarfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "static" / "corpus" / "mixed"
MANIFEST = DESTINATION / "manifest.json"
SCHEMA = "cadapt.corpus.mixed.v1"
TIMEOUT = 300.0


class FetchError(RuntimeError):
    """A source could not be fetched or did not match the manifest."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path = MANIFEST) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise FetchError(f"{path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
        raise FetchError(f"{path}: not a {SCHEMA} document")
    corpora = manifest.get("corpora")
    if not isinstance(corpora, list) or not corpora:
        raise FetchError(f"{path}: no corpora")
    for corpus in corpora:
        for key in ("name", "url", "archive_sha256", "archive_bytes", "licence", "files"):
            if key not in corpus:
                raise FetchError(f"{path}: corpus {corpus.get('name')!r} lacks {key}")
        if not isinstance(corpus["files"], list) or not corpus["files"]:
            raise FetchError(f"{path}: corpus {corpus['name']!r} lists no files")
        for item in corpus["files"]:
            for key in ("name", "bytes", "sha256"):
                if key not in item:
                    raise FetchError(f"{path}: {corpus['name']}/{item.get('name')!r} lacks {key}")
    return manifest


def download(url: str, expected_sha256: str, expected_bytes: int) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT) as response:
            data = response.read()
    except (urllib.error.URLError, OSError) as error:
        raise FetchError(f"{url}: {error}") from error
    if len(data) != expected_bytes or sha256_bytes(data) != expected_sha256:
        raise FetchError(f"{url}: {len(data):,} bytes, sha256 {sha256_bytes(data)}; the manifest pins "
                         f"{expected_bytes:,} bytes, {expected_sha256}")
    return data


def unpack(name: str, data: bytes) -> dict[str, bytes]:
    """Every regular file in the archive, by its base name."""
    files: dict[str, bytes] = {}
    if name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    files[Path(info.filename).name] = archive.read(info)
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
            for member in archive.getmembers():
                if member.isfile():
                    handle = archive.extractfile(member)
                    if handle is not None:
                        files[Path(member.name).name] = handle.read()
    return files


def fetch(corpus: dict, destination: Path) -> list[str]:
    """Fetch one corpus into destination/<name>/; returns report lines."""
    lines = []
    target = destination / corpus["name"]
    target.mkdir(parents=True, exist_ok=True)
    data = download(corpus["url"], corpus["archive_sha256"], int(corpus["archive_bytes"]))
    files = unpack(corpus["url"].rsplit("/", 1)[-1], data)
    for item in corpus["files"]:
        content = files.get(item["name"])
        if content is None:
            raise FetchError(f"{corpus['name']}: the archive holds no {item['name']!r}")
        if len(content) != int(item["bytes"]) or sha256_bytes(content) != item["sha256"]:
            raise FetchError(f"{corpus['name']}/{item['name']}: {len(content):,} bytes, sha256 "
                             f"{sha256_bytes(content)}; the manifest pins {int(item['bytes']):,}, {item['sha256']}")
        (target / item["name"]).write_bytes(content)
        lines.append(f"{corpus['name']}/{item['name']}: {len(content):,} bytes")
    return lines


def check(corpus: dict, destination: Path) -> list[str]:
    """Verify one corpus on disk against the manifest; raises on any mismatch."""
    lines = []
    for item in corpus["files"]:
        path = destination / corpus["name"] / item["name"]
        if not path.is_file():
            raise FetchError(f"{path}: missing; run tools/fetch_corpus.py")
        size = path.stat().st_size
        if size != int(item["bytes"]) or sha256_file(path) != item["sha256"]:
            raise FetchError(f"{path}: {size:,} bytes, sha256 {sha256_file(path)}; the manifest pins "
                             f"{int(item['bytes']):,}, {item['sha256']}")
        lines.append(f"{corpus['name']}/{item['name']}: {size:,} bytes, matches")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--only", action="append", default=[], metavar="NAME", help="fetch only this corpus")
    parser.add_argument("--check", action="store_true", help="verify the files on disk instead of fetching")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--destination", type=Path, default=DESTINATION)
    args = parser.parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        chosen = [c for c in manifest["corpora"] if not args.only or c["name"] in args.only]
        if args.only and len(chosen) != len(set(args.only)):
            known = sorted(c["name"] for c in manifest["corpora"])
            raise FetchError(f"unknown corpus in {args.only}; the manifest names {known}")
        for corpus in chosen:
            for line in (check if args.check else fetch)(corpus, args.destination):
                print(line)
    except FetchError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
