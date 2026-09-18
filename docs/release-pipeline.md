# Release validation and packaging

Run the validation targets applicable to a change before committing. Release
scripts export a committed revision, validate it and inspect packaged contents.
The configured checks are in [.github/workflows/](../.github/workflows/);
a passing local run does not establish the state of remote CI.

## The gates

| Gate | Holds | Target | Where it runs |
|---|---|---|---|
| Build on the supported platforms | Linux (GCC, Clang) and macOS (Clang) build with `-Wall -Wextra -Wpedantic -Werror` ([SUPPORT.md](../SUPPORT.md)); Windows compiles under MinGW, informational only | `make all` | `ci.yml` → `check` (three columns), `windows-build` (never blocks) |
| Correctness | the C suites, the CLI suite, the safety suite (bounded memory, staging, signals), the platform-header guard, the coverage gate (80 % library, 60 % CLI branches), the four library examples, the provenance and documentation tests | `make check` (GCC); `make -C adpat/c check-suites` and sanitizer targets (Clang on Linux/macOS: codec suites and examples; repository-wide Python suites and branch coverage run in the GCC column) | `check` |
| Sanitizers | the C suites under AddressSanitizer and UndefinedBehaviorSanitizer with every fault fatal; on Linux the CLI suite too, with leak detection | `make sanitize`; the CLI suite against a `build-sanitize` binary | `check` (Clang columns) |
| Identical bytes | GCC and Clang, the AVX2 and the baseline per-bit paths (`ADAPT_ISA=baseline`), write the same container for commands discovered from CLI help plus the canonical floor command and max at tier -6 and decode each other's (`RUN-5`) | workflow loop using `tools/profiles.py`; each codec command runs locally too | `identical-bytes` |
| Size floors | the gate proxy (the first 8 MiB of enwik8) and every row of the mixed corpus under both gate commands may not exceed its locked container size by one byte (`DLV-3`, [LOCKS.md](../LOCKS.md)); round trips verified | `make gate`, `make corpus-floor` | `size-and-memory-floors`, `throughput-floors` |
| Memory | the peak resident set of an encode and of a decode never exceeds what the container declares decoding needs (`cadapt decode --max-memory 1` names it) plus 16 MiB for the process (`RUN-1`, `RUN-2`) — measured on the proxy under the default profile, `--fast`, `--mid`, `--max`, the canonical floor command and `--max -6` | `make memory-gate` ([tools/check_memory.py](../tools/check_memory.py)) | `size-and-memory-floors` |
| Throughput, relative | on any host: the change's binary and its base commit's binary encode the proxy three times each, turn by turn, and the change must stay within the configured throughput tolerance of the base (`tools/check_ratio_floor.py --baseline-binary`) | `make gate GATE_ARGS="--baseline-binary <base cadapt>"` | `size-and-memory-floors` (the base is the pull request's base commit, or the previous commit on `main`) |
| Throughput, absolute | on the reference host the floors were locked on (`tools/ratio_floor.json` names it, CPU seconds): the proxy against its locked figure within 10 %, every corpus row within 25 % | `make gate`, `make corpus-floor` | `throughput-floors`, a self-hosted runner labelled `reference` |
| Install from a clean environment | an export of the revision (never the working tree) builds with `cc` and `make` alone, installs under a prefix, round-trips a file and a folder, renders its manual, compiles and runs a program against the installed header and library, and uninstalls clean; on Linux the same inside a fresh `debian:bookworm-slim` container | `make test-install`, `make test-install INSTALL_IMAGE=debian:bookworm-slim` | `install` |

Compressed sizes are deterministic. Memory checks use an allocation bound plus
process headroom. Throughput depends on the host, so hosted runners compare
candidate and base in the same run; the reference host also checks the recorded
absolute baseline.
The `throughput-floors` job runs only in this repository on a push to
`main` — a fork, or a pull request from one, has no reference runner and
skips it rather than failing.

### The reference runner

A self-hosted GitHub Actions runner on the host `tools/ratio_floor.json`
names (AMD Ryzen 5 5600X, with no concurrent encodes, as
[LOCKS.md](../LOCKS.md) requires of a lock), registered with the labels
`self-hosted`, `linux`, `x64`, `reference`, with GCC, Clang, Python 3,
`unzip` and network access to fetch the corpus once; the repository
variable `REFERENCE_RUNNER` set to `true` turns the job on (until then it
is skipped, so no run waits on a runner that is not there). A floor is re-locked
only from this host (`make gate GATE_ARGS="--runs 3"`, `make corpus-floor`),
never from a hosted runner.

## Local validation

```sh
make check CHECK_SECONDS=3600
make sanitize
make sanitize-cli
make check-long
make gate
make corpus-floor
make memory-gate
make test-install
```

The default check budget is 120 seconds; CI uses 3600. CHECK_SECONDS changes
only the wall timeout. CHECK_LOG_DIR selects where check logs are written.
Install the tool/test dependencies described in [bench/README.md](../bench/README.md)
and fetch missing mixed-corpus inputs with `make corpus`. Full canonical input
floors run through `make ratio-floor` with explicit FLOOR_ARGS fixture paths.
Fuzz smoke targets are `make -C adpat/c fuzz-smoke`,
`make -C adpat/c fuzz-header-smoke` and `make -C adpat/c fuzz-archive-smoke`;
FUZZ_CC selects a libFuzzer-capable Clang toolchain and FUZZ_ARTIFACT_DIR selects
where failing inputs are stored. Linux container installation uses
`make test-install INSTALL_IMAGE=debian:bookworm-slim` and requires Docker.

Keep timing gates sequential on the reference host. If a check fails, retain
its command, input/binary identity and diagnostic output and correct the cause;
do not re-lock a floor merely to make the gate pass.

## Artifacts

`make release` (or `tools/release.sh --rev v<version>`) builds the
artifacts of a committed revision from an export of it — nothing reads the
working tree, so what is published is what is committed (`DLV-2`) — into
`dist/<version>/`:

| Artifact | Holds |
|---|---|
| `cadapt-<version>-<os>-<arch>.tar.gz` | `bin/cadapt` (stripped), `lib/libadapt.a`, `include/adapt.h`, `share/man/man1/cadapt.1` — the layout `make install` writes, relocatable under any prefix — with `LICENSE`, `NOTICE`, `CHANGELOG.md`, `SUPPORT.md`, `SECURITY.md`, `sbom.spdx.json`, `build-manifest.json` and `payload-manifest.json` |
| `cadapt-<version>-src.tar.gz` | the classified sources, tools, documentation, workflow definitions, public benchmark data and pinned test fixtures. External mixed-corpus inputs are fetched by `tools/fetch_corpus.py`. Before it is packaged, `tools/test_install.sh --source` unpacks it, builds it with cc and make alone, installs it under a prefix, uses it and uninstalls it — what the README promises of a source tarball, proven from its contents |
| `sbom.spdx.json` | the SPDX 2.3 document of the sources: every file by SHA-256 and SHA-1, the licence, the run-time, build-time and test-time dependencies |
| `build-manifest.json` | the record of the build that made the binary: the sources' hashes, the compiler's version line and flags, the host, the hash and size of every output; `python3 tools/provenance.py reproduce --manifest dist/<version>/build-manifest.json` (or `make reproduce MANIFEST=…`) rebuilds and compares; not tracked in git, since it names the commit and the host it was made on |
| `NOTICE` | the licence and dependency statement, generated from the SBOM: MIT, no third-party code bundled, the C library and POSIX threads at run time, a C compiler to build, Python and the corpora under test only |
| `SHA256SUMS` | every artifact's SHA-256 |
| `release-manifest.json` | the version, the commit, the date, the platform, the toolchain, and each artifact with its hash and size |

Before publication all check suites run on the source export. GCC also runs the
branch-coverage pass; Clang uses `check-parts` with CHECK_CODEC_TARGET=check-suites.
Release builds use CHECK_SECONDS=3600 unless overridden. The binary tarball is
unpacked and used for a round trip before publication. A tag `v<version>` must name a
revision whose `cadapt.c` says `<version>`; the script refuses otherwise.

## Publish a release

1. Prepare release notes and set the version. In `CHANGELOG.md`, move the relevant `[Unreleased]` entries to `[<version>] — <date>`;
   `CADAPT_VERSION` in `adpat/c/cadapt.c` says the same; `make sbom`
   regenerates the SBOM.
2. `make check`, `make sanitize`, `make gate`, `make corpus-floor`,
   `make memory-gate` on the reference host. Commit after these checks pass.
   Validate that committed revision with `make test-install` and
   `make test-install INSTALL_IMAGE=debian:bookworm-slim` before tagging.
3. `git tag v<version>` and push the tag. `release.yml` runs every gate of
   `ci.yml`, builds the artifacts on Linux and macOS with `tools/release.sh`,
   installs each binary tarball from its unpacked layout and uses it,
   proves the build reproduces from its manifest, and publishes a GitHub
   release with one `SHA256SUMS` over every platform's files and
   `docs/release-notes/<version>.md` as its body (`tools/release.sh`
   refuses a revision without it).
4. Locally, `make release RELEASE_REV=v<version>` makes the same files under
   `dist/<version>/` for a release published by hand or verified against
   the workflow's.

The Large Text Compression Benchmark entry has its own assets
(`make ltcb-release`, [ltcb-submission.md](ltcb-submission.md)), built the
same way from an export.

## Release content validation

`make release-content-check` validates the explicit source inventory in
`tools/release_assets.json`. CI runs it on clean checkouts. The commit hook
uses `python3 -B tools/release_content.py staged` to inspect index contents,
including force-added ignored files. Run that command before committing when
a hook is unavailable. Inventory changes require review of purpose, accuracy,
audience, essential rationale and reference completeness; automated scans do
not establish those properties. Test exceptions name an exact rule and line
digest with a reason. Fixture exceptions pin the complete file digest.
A prompt or skill that implements product functionality can use `product_use`
with a `consumer` naming another production asset and a `reason` explaining its
runtime purpose. Review must verify that relationship. This narrow exception
does not admit unclassified local state alongside the product asset.

The source package includes the maintained tools, documentation, workflow
definitions, benchmark snapshots and pinned fixtures needed by `make check`.
`release-source.json` records its revision and source hashes so checks work
without a Git checkout. External mixed-corpus files are fetched from their pinned manifest.
The build manifest describes outputs before installation and stripping;
`payload-manifest.json` describes the exact installed files after stripping.
Both archives are inspected before publication. A failure names the file,
member or field and the corrective action; it blocks packaging or publication.

Floor definitions use `cadapt.ratio-floor.v2` with structured source-revision,
timing-measure and verification provenance. The floor reader also accepts v1
for comparisons against older releases; numerical acceptance rules are identical.

Publication assembly verifies every downloaded build artifact before assigning
platform-specific SBOM and manifest names. It checks source and installed
revision identity, rejects unexpected files, and requires all platform source
payloads to match. One source archive is retained even when tar metadata differs;
each published manifest records that archive's actual hash and the final download
names. The aggregate SHA256SUMS covers the complete published set. Build-directory
checksum files are omitted from published manifests to avoid stale names and a
checksum cycle.

For one-platform local builds, the artifact directory uses the unqualified
SBOM and manifest names shown above. Publication assembly renames them to
sbom-PLATFORM.spdx.json, build-manifest-PLATFORM.json and
release-manifest-PLATFORM.json; the binary archive keeps its internal names.
Inspect the assembled directory, not only the intermediate build directories.
