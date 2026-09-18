#!/usr/bin/env bash
# The assets of a Large Text Compression Benchmark entry, from a committed
# revision: the decompressor as the benchmark counts it — a zip of the
# stripped Linux binary with its licence and a note for the verifier (the
# sources are not published, by the operator's decision of 2026-09-15) —
# the containers the locked command writes for the fixtures given, every
# file's SHA-256, and a manifest the submission email quotes. Nothing here
# reads the working tree: the revision is exported with `git archive` into
# the release directory and built there, so what is published is what is
# committed (DLV-2); the exported sources are deleted after the build.
#
#   tools/ltcb_release.sh [--rev REV] [--out DIR] [--enwik8 PATH] [--enwik9 PATH] [--containers DIR]
#
# Defaults: REV = HEAD, DIR = dist/ltcb-<version>-<short rev> (git-ignored).
# With a fixture, the container is written by the release binary through
# tools/check_ratio_floor.py --verify — encoded, decoded and compared byte
# for byte against the locked floors — and its size, time and hash go into
# the manifest. With --containers, the .adpt files and the floor-check.txt
# of an earlier run of this script are taken over instead of encoding
# again (they were written by the same revision's binary, which the
# manifest's hashes let a reader confirm). Source: Claude Opus 5 via
# Claude Code, 2026-09-15.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=HEAD
OUT=""
ENWIK8=""
ENWIK9=""
CONTAINERS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --rev) REV=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --enwik8) ENWIK8=$(readlink -f "$2"); shift 2 ;;
        --enwik9) ENWIK9=$(readlink -f "$2"); shift 2 ;;
        --containers) CONTAINERS=$(readlink -f "$2"); shift 2 ;;
        *) echo "usage: $0 [--rev REV] [--out DIR] [--enwik8 PATH] [--enwik9 PATH] [--containers DIR]" >&2; exit 2 ;;
    esac
done

cd "$ROOT"
FULL=$(git rev-parse "$REV")
SHORT=$(git rev-parse --short "$REV")
VERSION=$(git show "$FULL:adpat/c/cadapt.c" | sed -n 's/^#define CADAPT_VERSION "\(.*\)"/\1/p')
[ -n "$VERSION" ] || { echo "no CADAPT_VERSION in $REV" >&2; exit 2; }
OUT=${OUT:-dist/ltcb-$VERSION-$SHORT}
rm -rf "$OUT"
mkdir -p "$OUT/src" "$OUT/containers"
OUT=$(readlink -f "$OUT")

# 1. the sources of the revision, and a build of them
# the whole codec directory: the sources are the section headers and the
# command line's units as the Makefile of the revision names them, so a
# list here would go stale (it did, when adapt_model.h became ten
# sections and cadapt.c three units)
git archive "$FULL" adpat/c LICENSE | tar -x -C "$OUT/src"
( cd "$OUT/src/adpat/c" && make -s all > "$OUT/build.log" 2>&1 ) || { cat "$OUT/build.log"; exit 1; }
cp "$OUT/src/adpat/c/build/cadapt" "$OUT/cadapt"
strip "$OUT/cadapt"
"$OUT/cadapt" --version > "$OUT/version.txt"

# 2. what a verifier needs to know, beside the binary
cat > "$OUT/README-ltcb.txt" <<EOF
cadapt $VERSION (commit $FULL) — Large Text Compression Benchmark entry
Licence: MIT (LICENSE). Linux x86-64, needs only the C library and pthreads.

Compress (the entry's command; the tier -9 is the default):
    ./cadapt encode --max --allocation context enwik9 -o enwik9.adpt
Decompress (no option changes the output; the container names its model):
    ./cadapt decode enwik9.adpt -o enwik9
Inspect a container:
    ./cadapt info enwik9.adpt

The decoder allocates the model the container's header names (about 661 MB
for the entry above). Sizes, times and hashes are in manifest.json. The
sources are not published; the binary is the entry, and anyone may run it
on enwik8 and enwik9 to reproduce the sizes in the manifest.
EOF
cp "$OUT/src/LICENSE" "$OUT/LICENSE"

# 3. the zip the benchmark counts: the decompressor and what it needs at run time — the binary alone
#    runs; the licence names the terms and the note tells a verifier the commands. The exported
#    sources go: only the binary is published.
( cd "$OUT" && rm -f "cadapt-$VERSION-linux-x86_64.zip" && zip -9 -q "cadapt-$VERSION-linux-x86_64.zip" cadapt README-ltcb.txt LICENSE )
rm -rf "$OUT/src"
[ "$(strings "$OUT/cadapt" | grep -c 'adpat/c\|\.c:[0-9]')" = 0 ] || { echo "the binary names source files" >&2; exit 1; }

# 4. the containers, verified against the locked floors — or taken over from an earlier run
if [ -n "$CONTAINERS" ]; then
    cp "$CONTAINERS"/*.adpt "$OUT/containers/"
    [ -f "$CONTAINERS/../floor-check.txt" ] && cp "$CONTAINERS/../floor-check.txt" "$OUT/floor-check.txt"
    [ -f "$CONTAINERS/floor-check.txt" ] && cp "$CONTAINERS/floor-check.txt" "$OUT/floor-check.txt"
fi
fixtures=()
[ -n "$ENWIK8" ] && fixtures+=(--fixture "enwik8=$ENWIK8")
[ -n "$ENWIK9" ] && fixtures+=(--fixture "enwik9=$ENWIK9")
if [ ${#fixtures[@]} -gt 0 ]; then
    python3 -B tools/check_ratio_floor.py --binary "$OUT/cadapt" "${fixtures[@]}" --verify --timeout 14400 \
        --output-dir "$OUT/containers" | tee "$OUT/floor-check.txt"
    rm -f "$OUT"/containers/*.restored
fi

# 5. hashes and the manifest
( cd "$OUT" && sha256sum cadapt "cadapt-$VERSION-linux-x86_64.zip" containers/* 2>/dev/null > SHA256SUMS || true )
python3 - "$OUT" "$VERSION" "$FULL" <<'EOF'
import hashlib, json, os, platform, re, subprocess, sys, datetime
out, version, commit = sys.argv[1], sys.argv[2], sys.argv[3]
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()
files = {}
for name in ["cadapt", f"cadapt-{version}-linux-x86_64.zip"]:
    p = os.path.join(out, name)
    files[name] = {"bytes": os.path.getsize(p), "sha256": sha(p)}
containers = {}
for name in sorted(os.listdir(os.path.join(out, "containers"))):
    p = os.path.join(out, "containers", name)
    containers[name] = {"bytes": os.path.getsize(p), "sha256": sha(p)}
results = {}
check = os.path.join(out, "floor-check.txt")
if os.path.exists(check):
    for line in open(check):
        m = re.match(r"PASS (\S+) size: ([\d,]+) B", line)
        if m:
            results.setdefault(m.group(1), {})["container_bytes"] = int(m.group(2).replace(",", ""))
        m = re.match(r"PASS (\S+) throughput: ([\d,]+) B/s in ([\d,.]+) s", line)
        if m:
            results.setdefault(m.group(1), {})["encode_seconds"] = float(m.group(3).replace(",", ""))
        m = re.match(r"PASS (\S+) round trip verified", line)
        if m:
            results.setdefault(m.group(1), {})["round_trip"] = "verified byte for byte"
cc = subprocess.run(["cc", "--version"], capture_output=True, text=True).stdout.split("\n")[0]
manifest = {
    "schema": "cadapt.ltcb-release.v1",
    "program": f"cadapt {version}",
    "commit": commit,
    "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "command": "cadapt encode --max --allocation context <input> -o <output>; cadapt decode <output>",
    "decompressor": files,
    "counted_zip": f"cadapt-{version}-linux-x86_64.zip",
    "sources_published": False,
    "containers": containers,
    "results": results,
    "toolchain": cc,
    "host": {"cpu": next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), platform.processor()),
             "os": platform.platform()},
}
json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
print(json.dumps({k: manifest[k] for k in ("program", "commit", "counted_zip", "decompressor", "results")}, indent=2))
EOF
echo "release assets in $OUT"
