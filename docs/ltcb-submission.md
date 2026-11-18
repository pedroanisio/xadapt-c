# Prepare an LTCB submission

`tools/ltcb_release.sh` packages a committed revision for the Large Text
Compression Benchmark. The decompressor ZIP includes the executable, license
and usage instructions; sources use the separate [release pipeline](release-pipeline.md).

The entry's command and the largest container it may write for each fixture
are in `tools/ltcb_floor.json`: `encode --max --allocation context --contexts
lean -10`, the 1 GiB tier, with enwik8 at 17,772,852 B and enwik9 at
143,751,760 B. The script encodes with that command and fails if a container
is larger. The file names no gating host, because its times were measured on a
shared cloud VM: encode time is reported for every host and gates none.

Provide ENWIK8 and ENWIK9 as paths to the verified, uncompressed benchmark
fixtures. The script checks their identities and verifies each round trip
using the executable being submitted. Choose a fresh output directory:

```sh
: "${ENWIK8:?Set ENWIK8 to the uncompressed fixture path}"
: "${ENWIK9:?Set ENWIK9 to the uncompressed fixture path}"
submission_dir=$(mktemp -d)
tools/ltcb_release.sh --rev HEAD --out "$submission_dir" \
    --enwik8 "$ENWIK8" --enwik9 "$ENWIK9"
```

The script replaces its output directory, so do not point --out at an existing
collection. Encoding and verifying full enwik9 is a substantial benchmark run.
An interrupted or failed run is not ready for submission.

The manifest records source revision, executable/container hashes, complete
compressed sizes, timing and verification. Inspect the exact ZIP and manifest
before publication. LTCB ranks the enwik9 container plus the decompressor ZIP;
report both components. Every timing needs its fixture, host, command and CPU
or wall-time measure. [Benchmark reference data](../research/ltcb-tables.md)
describes the published columns and dated comparison snapshot.
