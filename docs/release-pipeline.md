<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17, with Claude Opus 5 (Claude Code). -->
# The release pipeline

Part of [adapt](../README.md). What a change must pass before it is on
`main`, what a release is made of, and how each is run — by hand from the
Makefile, and by the two workflows in [.github/workflows/](../.github/workflows/)
(`ci.yml`, `release.yml`). Every gate is a `make` target first and a
workflow step second, so a gate never exists only in CI.

## The gates

| Gate | Holds | Target | Where it runs |
|---|---|---|---|
| Build on the supported platforms | Linux (GCC, Clang) and macOS (Clang) build with `-Wall -Wextra -Wpedantic -Werror` ([SUPPORT.md](../SUPPORT.md)); Windows compiles under MinGW, informational only | `make all` | `ci.yml` → `check` (three columns), `windows-build` (never blocks) |
| Correctness | the C suites, the CLI suite, the safety suite (bounded memory, staging, signals), the platform-header guard, the coverage gate (80 % library, 60 % CLI branches), the four library examples, the provenance and documentation tests | `make check` (GCC); `make check-suites` and `make sanitize-cli` (Clang, macOS: every part but the coverage pass, through the same targets the gcc column's parts come from — `tools/test_gates.py` holds the two lists to each other) | `check` |
| Sanitizers | the C suites under AddressSanitizer and UndefinedBehaviorSanitizer with every fault fatal; on Linux the CLI suite too, with leak detection | `make sanitize`; the CLI suite against a `build-sanitize` binary | `check` (Clang columns) |
| Identical bytes | GCC and Clang, the AVX2 and the baseline per-bit paths (`ADAPT_ISA=baseline`), write the same container for five commands and decode each other's (`RUN-5`) | the step's loop | `identical-bytes` |
| Size floors | the gate proxy (the first 8 MiB of enwik8) and every row of the mixed corpus under both gate commands may not exceed its locked container size by one byte (`DLV-3`, [LOCKS.md](../LOCKS.md)); round trips verified | `make gate`, `make corpus-floor` | `size-and-memory-floors`, `throughput-floors` |
| Memory | the peak resident set of an encode and of a decode never exceeds what the container declares decoding needs (`cadapt decode --max-memory 1` names it) plus 16 MiB for the process (`RUN-1`, `RUN-2`) — measured on the proxy under the default profile, `--fast`, `--mid`, `--max` and `--max -6` | `make memory-gate` ([tools/check_memory.py](../tools/check_memory.py)) | `size-and-memory-floors` |
| Throughput, relative | on any host: the change's binary and its base commit's binary encode the proxy three times each, turn by turn, and the change may be no slower than the base minus the floor's tolerance (`tools/check_ratio_floor.py --baseline-binary`) | `make gate GATE_ARGS="--baseline-binary <base cadapt>"` | `size-and-memory-floors` (the base is the pull request's base commit, or the previous commit on `main`) |
| Throughput, absolute | on the reference host the floors were locked on (`tools/ratio_floor.json` names it, CPU seconds): the proxy against its locked figure within 10 %, every corpus row within 25 % | `make gate`, `make corpus-floor` | `throughput-floors`, a self-hosted runner labelled `reference` |
| Install from a clean environment | an export of the revision (never the working tree) builds with `cc` and `make` alone, installs under a prefix, round-trips a file and a folder, renders its manual, compiles and runs a program against the installed header and library, and uninstalls clean; on Linux the same inside a fresh `debian:bookworm-slim` container | `make test-install`, `make test-install INSTALL_IMAGE=debian:bookworm-slim` | `install` |

Why the split of throughput: bytes and resident sets are the same on every
machine, so they are gated absolutely everywhere; a CPU-second floor is a
property of one host, so a hosted runner gates it only relative to the base
commit in the same run, and the reference host gates the locked figure.
The `throughput-floors` job runs only in this repository on a push to
`main` — a fork, or a pull request from one, has no reference runner and
skips it rather than failing.

### The reference runner

A self-hosted GitHub Actions runner on the host `tools/ratio_floor.json`
names (AMD Ryzen 5 5600X, quiet — no other encodes beside it, as
[LOCKS.md](../LOCKS.md) requires of a lock), registered with the labels
`self-hosted`, `linux`, `x64`, `reference`, with GCC, Clang, Python 3,
`unzip` and network access to fetch the corpus once; the repository
variable `REFERENCE_RUNNER` set to `true` turns the job on (until then it
is skipped, so no run waits on a runner that is not there). A floor is re-locked
only from this host (`make gate GATE_ARGS="--runs 3"`, `make corpus-floor`),
never from a hosted runner.

## The artifacts

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
The
binary tarball is unpacked elsewhere and made to round-trip a file, so the
tarball is proven before it is published. A tag `v<version>` must name a
revision whose `cadapt.c` says `<version>`; the script refuses otherwise.

## Cutting a release

1. `CHANGELOG.md`: the `[Unreleased]` entry becomes `[<version>] - <date>`;
   `CADAPT_VERSION` in `adpat/c/cadapt.c` says the same; `make sbom`
   regenerates the SBOM; commit.
2. `make check`, `make sanitize`, `make gate`, `make corpus-floor`,
   `make memory-gate` on the reference host; `make test-install` and
   `make test-install INSTALL_IMAGE=debian:bookworm-slim`.
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
