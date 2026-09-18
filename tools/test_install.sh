#!/usr/bin/env bash
# The packaging and installation flow, from a clean environment: an export
# of a committed revision (never the working tree) is built with nothing
# but a C compiler and make, installed under a staging root, and the
# installation is used — the command round-trips a file and a folder, the
# manual renders, a program compiles against the installed header and
# library and runs, and `make uninstall` leaves nothing behind. A binary
# tarball from tools/release.sh is tested the same way from its unpacked
# layout. With --docker the tree test runs inside a fresh container of the
# image given, which is the cleanest environment there is: nothing of this
# host but the export reaches it.
#
#   tools/test_install.sh [--rev REV]                  the tree, here
#   tools/test_install.sh --tarball cadapt-*.tar.gz    a release tarball, here
#   tools/test_install.sh --docker IMAGE [--rev REV]   the tree, in a container
#
# Exit 0 when every step held, 1 otherwise, 2 for a usage error.
#
# Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17,
# with Claude Opus 5 (Claude Code).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=HEAD
TARBALL=""
IMAGE=""
INSIDE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rev) REV=$2; shift 2 ;;
        --tarball) TARBALL=$(readlink -f "$2"); shift 2 ;;
        --docker) IMAGE=$2; shift 2 ;;
        --inside) INSIDE=1; shift ;;
        *) echo "usage: $0 [--rev REV] | --tarball FILE | --docker IMAGE [--rev REV]" >&2; exit 2 ;;
    esac
done

fail() { echo "test_install: $*" >&2; exit 1; }

# What an installation must do, given its prefix and the examples' source.
use_installation() {
    local prefix=$1 examples=$2 scratch=$3
    [ -x "$prefix/bin/cadapt" ] || fail "no executable at $prefix/bin/cadapt"
    [ -f "$prefix/lib/libadapt.a" ] || fail "no library at $prefix/lib/libadapt.a"
    [ -f "$prefix/include/adapt.h" ] || fail "no header at $prefix/include/adapt.h"
    [ -f "$prefix/share/man/man1/cadapt.1" ] || fail "no manual at $prefix/share/man/man1/cadapt.1"
    "$prefix/bin/cadapt" --version | grep -q '^cadapt [0-9][0-9.]* (format versions [0-9]* to [0-9]*)$' || fail "--version"
    # a file, proven, and back
    head -c 200000 "$prefix/include/adapt.h" > "$scratch/sample"
    "$prefix/bin/cadapt" encode "$scratch/sample" -o "$scratch/sample.adpt" --verify > /dev/null 2>&1 || fail "encode"
    "$prefix/bin/cadapt" decode "$scratch/sample.adpt" -o "$scratch/sample.back" > /dev/null 2>&1 || fail "decode"
    cmp "$scratch/sample" "$scratch/sample.back" || fail "round trip"
    "$prefix/bin/cadapt" info "$scratch/sample.adpt" | grep -q '^format' || fail "info"
    # a folder, restored with its structure
    mkdir -p "$scratch/folder/sub"
    cp "$scratch/sample" "$scratch/folder/a.txt"
    printf 'nested\n' > "$scratch/folder/sub/b.txt"
    ln -s a.txt "$scratch/folder/link" 2>/dev/null || true
    "$prefix/bin/cadapt" encode "$scratch/folder" -o "$scratch/folder.adpt" > /dev/null 2>&1 || fail "folder encode"
    "$prefix/bin/cadapt" decode "$scratch/folder.adpt" -o "$scratch/folder.back" > /dev/null 2>&1 || fail "folder decode"
    diff -r "$scratch/folder" "$scratch/folder.back" > /dev/null || fail "folder round trip"
    # the manual renders, where a renderer exists (GNU man's -l, or mandoc/nroff)
    local page="$prefix/share/man/man1/cadapt.1"
    if man -l "$page" > /dev/null 2>&1 || mandoc "$page" > /dev/null 2>&1 || nroff -man "$page" > /dev/null 2>&1; then
        { man -l "$page" 2>/dev/null || mandoc "$page" 2>/dev/null || nroff -man "$page" 2>/dev/null; } | grep -q 'cadapt' || fail "manual"
    fi
    # a program against the installed header and library, and run
    ${CC:-cc} -std=c99 -Wall -Wextra -I"$prefix/include" -o "$scratch/roundtrip" "$examples/roundtrip.c" \
        "$prefix/lib/libadapt.a" -pthread || fail "compile against the installation"
    "$scratch/roundtrip" > /dev/null || fail "the compiled program"
}

if [ -n "$IMAGE" ] && [ "$INSIDE" = 0 ]; then
    # the export, then this script inside a fresh container of the image
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/cadapt-install.XXXXXX")
    trap 'rm -rf "$WORK"' EXIT
    cd "$ROOT"
    git archive "$(git rev-parse "$REV^{commit}")" adpat/c tools Makefile LICENSE | tar -x -C "$WORK"
    docker run --rm -v "$WORK:/src:ro" -w /src "$IMAGE" sh -c \
        'set -e; if command -v apt-get > /dev/null; then apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y gcc make man-db > /dev/null; fi; cp -r /src /work && cd /work && tools/test_install.sh --inside'
    echo "test_install: the tree installs and runs in a fresh $IMAGE container"
    exit 0
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/cadapt-install.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
if [ -n "$TARBALL" ]; then
    # a release tarball: unpack, use its layout as the prefix
    tar -xzf "$TARBALL" -C "$WORK"
    PREFIX=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'cadapt-*' | head -1)
    [ -n "$PREFIX" ] || fail "no cadapt-* directory in $TARBALL"
    for f in LICENSE NOTICE sbom.spdx.json build-manifest.json CHANGELOG.md; do
        [ -f "$PREFIX/$f" ] || fail "$f missing from the tarball"
    done
    grep -q 'Licence: MIT' "$PREFIX/NOTICE" || fail "NOTICE names no licence"
    mkdir -p "$WORK/scratch"
    use_installation "$PREFIX" "$ROOT/adpat/c/examples" "$WORK/scratch"
    echo "test_install: $(basename "$TARBALL") installs and runs from its unpacked layout"
    exit 0
fi

if [ "$INSIDE" = 1 ]; then
    SRC=$(pwd)
else
    SRC="$WORK/src"
    mkdir -p "$SRC"
    cd "$ROOT"
    git archive "$(git rev-parse "$REV^{commit}")" adpat/c tools Makefile LICENSE | tar -x -C "$SRC"
fi
STAGE="$WORK/stage"
( cd "$SRC/adpat/c" && make -s all > "$WORK/build.log" 2>&1 ) || { cat "$WORK/build.log"; fail "build"; }
( cd "$SRC/adpat/c" && make -s install DESTDIR="$STAGE" PREFIX=/usr/local > "$WORK/install.log" 2>&1 ) || { cat "$WORK/install.log"; fail "install"; }
mkdir -p "$WORK/scratch"
use_installation "$STAGE/usr/local" "$SRC/adpat/c/examples" "$WORK/scratch"
( cd "$SRC/adpat/c" && make -s uninstall DESTDIR="$STAGE" PREFIX=/usr/local > /dev/null 2>&1 ) || fail "uninstall"
[ -z "$(find "$STAGE" -type f)" ] || fail "uninstall left files: $(find "$STAGE" -type f)"
echo "test_install: the tree builds with cc and make alone, installs under a prefix, runs, and uninstalls clean"
