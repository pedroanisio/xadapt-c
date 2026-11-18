# adapt

adapt is a lossless compression library in C99 and a command-line tool,
`cadapt`, for files, streams and directory trees. Containers carry the model
settings needed to decode them; no external dictionary, model or service is
required. The codec accepts arbitrary bytes, including text in any encoding.

## Build and install

Build with a C99 compiler and Make on Linux or macOS:

```sh
make
./adpat/c/build/cadapt --version
make install PREFIX="$HOME/.local"
```

The install layout is `bin/cadapt`, `lib/libadapt.a`, `include/adapt.h` and
`share/man/man1/cadapt.1`. Add the chosen prefix's bin directory to PATH.
`PREFIX` selects the installation prefix; `DESTDIR` adds a staging root.
`make uninstall` with the same settings removes these four files.
Python and benchmark packages are needed for validation, not for building or
using the codec. `CC`, `CFLAGS` and `CPPFLAGS` select the compiler and build flags.

Platform tarballs contain the install layout above; source tarballs require
a build. Check downloads against the release's SHA256SUMS before installation. See the
[release notes](docs/release-notes/0.12.0.md) for tarball installation and
[SUPPORT.md](SUPPORT.md) for platform and compatibility policy.

## Compress and restore

```sh
cadapt encode report.pdf                     # creates report.pdf.adpt
cadapt decode report.pdf.adpt -o report-copy.pdf
cadapt encode photos/ -o photos.adpt          # one container for the tree
cadapt decode photos.adpt -o photos-copy
cadapt encode records.csv --max -6 --verify   # compression profile, memory tier, proof
cadapt info records.csv.adpt
```

Existing outputs are preserved unless `--force` is given. Without `-o`, encoding
appends `.adpt`; decoding removes that suffix or appends `.out`. Use `-` for
standard input or output:

```sh
printf 'example data\n' | cadapt encode - --fast -3 > example.adpt
cadapt decode - < example.adpt
```

Named outputs are staged before publication. Standard output and library stream
callbacks can receive bytes before a final checksum or I/O failure: accept their
output only after successful completion. File metadata is part of named-file
containers, so two encodes can differ if the source timestamps changed.

## Profiles and memory

Choose the model independently from its memory budget. Compression ratio and
speed depend on the input; compare profiles on representative data.

| Profile | Intended use |
| --- | --- |
| normal (default) | General-purpose compression with automatic model selection. |
| `--fast` | A smaller predictor with fewer contexts. |
| `--mid` | Seven fixed contexts and one arbiter. |
| `--max` | A larger predictor with additional contexts and model policies. |

The last tier option wins; `-9` is the default. Budgets cover codec model,
window and workspace, rather than an exact process RSS limit.

| Tier | Requested budget (MiB) |
| --- | ---: |
| `-0` | 3 |
| `-1` | 9 |
| `-2` | 17 |
| `-3` | 32 |
| `-4` | 48 |
| `-5` | 94 |
| `-6` | 94 |
| `-7` | 186 |
| `-8` | 370 |
| `-9` | 674 |
| `-10` | 1024 |
| `-11` | 2048 |

A request below the profile's minimum is raised to that minimum: normal needs
28,184,711 bytes and max needs 57,816,199 bytes for the smallest supported sizing
request. The selected model may need less than the requested budget. Its actual
decoder requirement is recorded in the container and reported by `cadapt info`.
Folder path and metadata bookkeeping, verification and parallel work have
additional resource costs.

Decoding has no memory or output cap by default. Set explicit limits for a
resource-constrained consumer:

```sh
cadapt decode example.adpt -o example-copy --max-memory 134217728 --max-output 104857600
```

`--max-memory` bounds decoder working memory. `--max-output` bounds decoded
bytes; for folders it also bounds the combined logical size of restored files,
including holes and hard-link aliases. See [SECURITY.md](SECURITY.md).

## Command reference

```
cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--explain] [--fast | --mid | --max] [-0 .. -11]
              [--threads <n>] [--segment-bytes <bytes>]
              [--adaptive rates|sparse|retained|auto] [--transform bmp|text|none] [--allocation auto|window|context]
              [--contexts auto|lean]
cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]
cadapt info   <file>
cadapt --version
```

| Option | Behavior |
| --- | --- |
| `--verify`, `--no-verify` | Enable or explicitly disable encoder round-trip proof. Proof is off by default. It uses two models when the budget permits, or a second pass for suitable named-file input/output. |
| `--explain` | Report model, allocation and transform selection. This diagnostic output is not a stable machine interface. |
| `--threads n`, `--segment-bytes bytes` | Encode independent segments when n is at least two; the default segment is 16 MiB. Segment sizes must be whole codec blocks. Worker count is limited by memory and available CPUs. The first block's independent model trials also run concurrently, within the workers' memory. Segmentation changes compression; identical requested settings produce identical bytes regardless of worker scheduling. |
| `--adaptive rates`, `sparse`, `retained` | Select an experimental causal predictor with normal or max. |
| `--adaptive auto` | Encode a finite set of candidates and keep the smallest complete container. Up to eight trials use temporary disk for the input and two containers. |
| `--transform bmp`, `text`, `none` | Select a reversible transform or disable automatic transforms. BMP requires eligible 24-bit input. Text builds its dictionary from the input itself. |
| `--allocation auto`, `window`, `context` | Choose automatic sizing, window-first sizing or context-first sizing. |
| `--contexts auto`, `lean` | Select contexts from the first block or force the lean set. Not supported with fast or mid. |
| `--help`, `--version` | Show the command interface or release/format version information. |

The [manual](adpat/c/cadapt.1) describes option combinations, resource costs and
exit statuses. [Adaptive predictors](docs/ADAPTIVE.md) describes search and
transform semantics.

## Filesystem behavior

Named files and folders preserve mode and access/modification timestamps.
Folder archives also represent symbolic links, hard links, sparse extents,
extended attributes and supported ACLs. Owners and groups are not restored.
Support depends on the operating system and destination filesystem; a required
attribute that cannot be restored causes an error rather than silent loss.

[Archive and restoration reference](adpat/c/ARCHIVE.md) documents metadata
portability, path validation, staging and publication. Forcing a folder output
has platform-specific restrictions. Catchable failures trigger cleanup;
uncatchable termination or cleanup failures can leave temporary entries.

## Library

Include `adapt.h` and link `libadapt.a`; the default threaded build also needs
`-pthread`. The library supports in-memory buffers, callback streams, optional
verification, segmented encoding/decoding, and archive packing/unpacking.

[adapt.h](adpat/c/adapt.h) defines ownership, callback lifetimes, thread behavior,
errors and resource bounds. The [examples](adpat/c/examples/) are compilable
programs for buffers, streams, archives and API contracts. Free returned buffers
with `adapt_free()` and check `adapt_status` before using a result.

## Format

Supported container versions: **19–54, 56–58, 61–72, 75–91**.
Archive versions **1–4** are readable; the CLI writes archive version **4**.
The exact wire layouts and model policies are defined by `ADAPT_VERSIONS` and
the accompanying declarations in [adapt.h](adpat/c/adapt.h).

Max-profile files written by this release carry versions 86–91, which pass
the final probability to the coder in 16 bits; cadapt 0.12.0 cannot decode them.
Versions 13–15, written by cadapt 0.11.0, require that release to decode. See the
[upgrade procedure](docs/release-notes/0.12.0.md#upgrading-from-0110) before
replacing an older installation. Unknown format numbers are rejected.
[SUPPORT.md](SUPPORT.md) defines the compatibility commitment for supported
formats and interfaces.

## Validation

```sh
make check                         # correctness, coverage, examples, tools and docs
make sanitize                      # C library sanitizer suites
make -C adpat/c sanitize-cli        # CLI sanitizer suite
make gate                          # 8 MiB size and throughput regression
make corpus-floor                  # mixed-corpus regressions
make memory-gate                    # measured codec memory bounds
make profile                       # hardware counters and hot symbols (Linux perf)
```

`make check` has a 120-second default wall budget. Set CHECK_SECONDS for the
validation host (CI uses 3600); this changes the timeout, not the assertions.
CHECK_LOG_DIR selects the check-log directory. `make check-long` enables long
CLI cases through ADAPT_LONG_TESTS. CADAPT selects the binary for documentation
and gate tests; ADAPT_TEST_THREADS=0 excludes thread-only safety cases in a
build without threads. `ADAPT_ISA=baseline` selects the portable predictor path.

The [release pipeline](docs/release-pipeline.md) lists all quality, performance,
install and packaging gates. [Measurement tools](bench/README.md) explains corpus
preparation and development-only settings, including ADAPT_MODEL2_SLOTS.

<!-- floors:start -->
Baselines recorded 2026-09-16 on AMD Ryzen 5 5600X 6-Core Processor,
`cadapt encode --max --allocation context --contexts lean`. Container sizes must not exceed these limits;
throughput may decrease by at most 10% (`make gate` checks the proxy,
`make ratio-floor` the other inputs):

| Fixture | Input | Maximum container | Baseline time |
|---|---:|---:|---:|
| enwik8-8MiB | 8,388,608 B | 1,669,716 B | 10.7 CPU s |
| enwik8-16MiB | 16,777,216 B | 3,236,535 B | 19.8 CPU s |
| enwik8 | 100,000,000 B | 17,800,141 B | 107.4 CPU s |
| enwik9 | 1,000,000,000 B | 145,784,134 B | 1,054.0 CPU s |
<!-- floors:end -->

These are regression baselines for the named command and host, not predictions
for arbitrary input. [Benchmark reference data](research/README.md) contains
dated LTCB comparisons and their reproduction tools.

## Further reading

- [Product constraints](docs/CONSTRAINTS.md) and [engineering contracts](LOCKS.md).
- [Predictor synchronization and accounting](docs/MODEL.md).
- [Release notes](docs/release-notes/0.12.0.md) and [changelog](CHANGELOG.md).
- [Security policy](SECURITY.md) and [support](SUPPORT.md).

adapt is distributed under the [MIT license](LICENSE). Source attribution and
dependencies are recorded in [sbom.spdx.json](sbom.spdx.json); release artifacts
also include build and payload manifests and a NOTICE.
