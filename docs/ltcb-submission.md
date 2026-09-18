# LTCB artifact preparation

Part of [adapt](../README.md). `tools/ltcb_release.sh` prepares a benchmark
submission from a committed revision. Its binary ZIP includes the executable,
license and usage instructions. Source releases use the separate
[release pipeline](release-pipeline.md).

Build and verify the fixture containers with the exact executable being
submitted. Run the packaging script with explicit fixture locations:

```sh
tools/ltcb_release.sh --help
```

The script records the resolved source revision, executable and container
hashes, complete compressed sizes, and verification results in its manifest.
Report decompressor ZIP size separately from the enwik9 container size; their
sum is the ranked quantity. Identify the fixture, host, command and timing
measure for every reported timing. An incomplete or interrupted run is not a
submission artifact. Inspect the exact ZIP and verify its manifest before
publishing. The public comparison inputs and their column meanings are in
[ltcb-tables.md](../research/ltcb-tables.md).
