#!/usr/bin/env bash
# The release artifacts of a committed revision, built from an export of
# that revision alone (nothing here reads the working tree), so what is
# published is what is committed (DLV-2):
#
#   cadapt-<version>-<os>-<arch>.tar.gz   bin/cadapt, lib/libadapt.a,
#       include/adapt.h, share/man/man1/cadapt.1, LICENSE, NOTICE,
#       CHANGELOG.md, sbom.spdx.json, build-manifest.json — the layout
#       `make install` writes, relocatable under any prefix
#   cadapt-<version>-src.tar.gz            the sources, tools and documents
#       (includes pinned test fixtures and benchmark input manifests)
#   sbom.spdx.json, build-manifest.json    the SPDX document of the sources
#       and the record of the build that made the binary (toolchain,
#       flags, host, the hash and size of every output)
#   NOTICE                                 the licence and dependency
#       statement, from the SBOM
#   SHA256SUMS                             every artifact's SHA-256
#   release-manifest.json                  version, commit, date, the
#       artifacts with their hashes, the toolchain
#
#   tools/release.sh [--rev REV] [--out DIR] [--no-tests]
#
# Defaults: REV = HEAD, DIR = dist/<version>/ (git-ignored). A tag named
# v<version> must name the revision whose cadapt.c says <version>; any
# other name is accepted. The full check suite runs on the source export
# (--no-tests skips it for a revision whose gates have already passed);
# the packaged binary is then unpacked elsewhere and made to round-trip a
# file, so the tarball is proven before it is published.
#
# Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17,
# with Claude Opus 5 (Claude Code).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=HEAD
OUT=""
TESTS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --rev) REV=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --no-tests) TESTS=0; shift ;;
        *) echo "usage: $0 [--rev REV] [--out DIR] [--no-tests]" >&2; exit 2 ;;
    esac
done

cd "$ROOT"
FULL=$(git rev-parse "$REV^{commit}")
SHORT=$(git rev-parse --short "$FULL")
VERSION=$(git show "$FULL:adpat/c/cadapt.c" | sed -n 's/^#define CADAPT_VERSION "\(.*\)"/\1/p')
[ -n "$VERSION" ] || { echo "no CADAPT_VERSION in $REV" >&2; exit 2; }
case "$REV" in
    v[0-9]*) [ "$REV" = "v$VERSION" ] || { echo "tag $REV names a tree whose cadapt.c says $VERSION" >&2; exit 2; } ;;
esac
# the release notes are the published release's body: a revision without
# them is not a release: installation and compatibility guidance must be
# present in the published body.
git cat-file -e "$FULL:docs/release-notes/$VERSION.md" 2>/dev/null \
    || { echo "$REV has no docs/release-notes/$VERSION.md: write the release notes before releasing $VERSION" >&2; exit 2; }
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
NAME="cadapt-$VERSION-$OS-$ARCH"
OUT=${OUT:-dist/$VERSION}
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cadapt-release.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# 1. the sources of the revision, exported, and the source tarball
SRC="$WORK/cadapt-$VERSION"
mkdir -p "$SRC"
python3 -B tools/release_content.py export --rev "$FULL" --destination "$SRC"
rm -rf "$SRC/adpat/c/build" "$SRC/tools/__pycache__"
( cd "$WORK" && tar -czf "$OUT/cadapt-$VERSION-src.tar.gz" "cadapt-$VERSION" )
# the source tarball as a user gets it: built, installed under a prefix, used
# and uninstalled from its own contents (the export list above is what it
# holds; a file missing from the list fails here, not on a user's machine)
tools/test_install.sh --source "$OUT/cadapt-$VERSION-src.tar.gz" > "$OUT/source-install.log" 2>&1 \
    || { cat "$OUT/source-install.log"; echo "the source tarball does not build and install from its contents" >&2; exit 1; }

# 2. the build, its tests, and the record of it — after the documents'
#    floors blocks (the README, the manual, this version's release notes,
#    which become the published release's body) are proven current
( cd "$SRC" && python3 -B tools/render_floors.py check ) || { echo "the floors blocks are stale in $REV: run python3 tools/render_floors.py update and commit" >&2; exit 1; }
( cd "$SRC/adpat/c" && make -s all ) > "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; exit 1; }
if [ "$TESTS" = 1 ]; then
    # GCC supplies the gcov branch gate. Clang runs the same source-export
    # suites without that GCC-specific pass. check-parts also avoids the
    # GNU timeout dependency on macOS; the CI job bounds total execution.
    if ${CC:-cc} --version | grep -qi clang; then
        CHECK_LOGS=$(mktemp -d "${TMPDIR:-/tmp}/cadapt-release-check.XXXXXX")
        ( cd "$SRC" && make -s -j check-parts CHECK_CODEC_TARGET=check-suites CHECK_LOG_DIR="$CHECK_LOGS" ) >> "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; echo "suite logs: $CHECK_LOGS"; exit 1; }
    else
        ( cd "$SRC" && make -s check ) >> "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; exit 1; }
    fi
fi
( cd "$SRC" && python3 -B tools/provenance.py sbom --output "$OUT/sbom.spdx.json" ) > /dev/null
( cd "$SRC" && python3 -B tools/provenance.py manifest --build adpat/c/build --output "$OUT/build-manifest.json" ) > /dev/null

# 3. the licence and dependency statement, from the SBOM
python3 - "$OUT/sbom.spdx.json" "$VERSION" "$FULL" > "$OUT/NOTICE" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
packages = {p["name"]: p for p in doc["packages"]}
codec = packages["adapt"]
print(f"cadapt {sys.argv[2]} — built from commit {sys.argv[3]}")
print()
print(f"Licence: {codec['licenseDeclared']} (see LICENSE). {codec['copyrightText']}")
print()
print("No third-party code is bundled in the binary or the library. What it")
print("depends on, as the SBOM (sbom.spdx.json) records it:")
kinds = {}
for r in doc["relationships"]:
    kinds.setdefault(r["relatedSpdxElement"], []).append((r["relationshipType"], r["spdxElementId"]))
by_id = {p["SPDXID"]: p for p in doc["packages"]}
for r in doc["relationships"]:
    a, t, b = r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]
    if t == "DEPENDS_ON" and a == codec["SPDXID"]:
        print(f"  - at run time: {by_id[b]['name']} ({by_id[b].get('licenseConcluded', 'NOASSERTION')})")
for r in doc["relationships"]:
    a, t, b = r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]
    if t == "BUILD_DEPENDENCY_OF" and b == codec["SPDXID"]:
        print(f"  - to build: {by_id[a]['name']}")
for r in doc["relationships"]:
    a, t, b = r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]
    if t == "TEST_DEPENDENCY_OF" and b == codec["SPDXID"]:
        print(f"  - under test only, not shipped: {by_id[a]['name']} ({by_id[a].get('licenseConcluded', 'NOASSERTION')})")
print()
print("The container format is the project's own; every version once published")
print("keeps decoding (SUPPORT.md, Compatibility).")
PY

# 4. the binary tarball in the layout `make install` writes
PKG="$WORK/$NAME"
( cd "$SRC/adpat/c" && make -s install DESTDIR="$PKG" PREFIX=/ ) >> "$OUT/build.log" 2>&1
strip "$PKG/bin/cadapt" 2>/dev/null || true
cp "$SRC/LICENSE" "$SRC/CHANGELOG.md" "$SRC/SUPPORT.md" "$SRC/SECURITY.md" "$OUT/NOTICE" "$OUT/sbom.spdx.json" "$OUT/build-manifest.json" "$PKG/"
( cd "$SRC" && python3 -B tools/release_content.py payload "$PKG" --revision "$FULL" )
( cd "$WORK" && tar -czf "$OUT/$NAME.tar.gz" "$NAME" )
( cd "$SRC" && python3 -B tools/release_content.py archive "$OUT/cadapt-$VERSION-src.tar.gz" --kind source && python3 -B tools/release_content.py archive "$OUT/$NAME.tar.gz" --kind binary )

# 5. the packaged binary, unpacked elsewhere, round-trips a file
PROVE="$WORK/prove"
mkdir -p "$PROVE"
tar -xzf "$OUT/$NAME.tar.gz" -C "$PROVE"
head -c 300000 "$SRC/adpat/c/adapt_model_bit.h" > "$PROVE/sample" 2>/dev/null || cp "$SRC/README.md" "$PROVE/sample"
"$PROVE/$NAME/bin/cadapt" encode "$PROVE/sample" -o "$PROVE/sample.adpt" --verify > /dev/null
"$PROVE/$NAME/bin/cadapt" decode "$PROVE/sample.adpt" -o "$PROVE/sample.back" > /dev/null
cmp "$PROVE/sample" "$PROVE/sample.back"
"$PROVE/$NAME/bin/cadapt" --version | grep -q "cadapt $VERSION " || { echo "the packaged binary is not $VERSION" >&2; exit 1; }

# 6. checksums (sha256sum, or shasum where the platform has only that) and the release manifest
sha256() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
( cd "$OUT" && sha256 "$NAME.tar.gz" "cadapt-$VERSION-src.tar.gz" sbom.spdx.json build-manifest.json NOTICE > SHA256SUMS )
python3 - "$OUT" "$VERSION" "$FULL" "$NAME" > "$OUT/release-manifest.json" <<'PY'
import hashlib, json, os, sys
from datetime import datetime, timezone
out, version, commit, name = sys.argv[1:5]
manifest = json.load(open(os.path.join(out, "build-manifest.json")))
artifacts = {}
for f in (f"{name}.tar.gz", f"cadapt-{version}-src.tar.gz", "sbom.spdx.json", "build-manifest.json", "NOTICE", "SHA256SUMS"):
    p = os.path.join(out, f)
    artifacts[f] = {"sha256": hashlib.sha256(open(p, "rb").read()).hexdigest(), "bytes": os.path.getsize(p)}
print(json.dumps({"schema": "cadapt.release-manifest.v1", "version": version, "commit": commit,
                  "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                  "platform": name[len(f"cadapt-{version}-"):], "toolchain": manifest["toolchain"], "host": manifest["host"],
                  "artifacts": artifacts}, indent=2))
PY
rm -f "$OUT/build.log"
echo "release $VERSION ($SHORT) in $OUT:"
( cd "$OUT" && ls -l | awk 'NR > 1 {printf "  %10d  %s\n", $5, $9}' )
