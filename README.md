# adapt — a self-adaptive lossless codec, in C

`adapt` compresses bytes with a context-mixing predictor and a binary
arithmetic coder: every bit is predicted from a set of hashed contexts
(orders 1–6, word, field, record, sparse, class history), recall of earlier
matches, and run memories, arbitrated by small online mixers. The container
frames the input in 256 KiB blocks, each modelled, stored-and-observed, or
stored-and-skipped, and ends with the original length and its CRC-32. The
implementation is C99 with nothing beyond the C standard library (POSIX
threads for the encoder's optional proof). Byte streams use format versions
19 (normal), 20 (fast) and 21 (max); named files and folders use versions
22, 23 and 24 respectively to carry filesystem metadata. Experimental
profiles also use versions 25–52 for
causal adaptive predictors and a reversible BMP24 transform; the max
profile with compact side tables uses 53 and 56, with its lean context
set as well 54 and 57, and sized to its budget — a context table any
eighth past a power of two, named by a sizing byte — 58 and 61; the text
transform ahead of the normal, fast, max, compact and lean models uses
62 to 66, 67 to 71 with metadata. The decoder reads every version ever
published; versions 10–18 and the unused numbers (55, 59, 60) are
refused. The CLI uses native filesystem APIs to
restore metadata.

The [experimental implementation report](research/causal-byte-implementation.md)
specifies the new algorithms, format versions, limits and measurement protocol.
The [current-source results](research/current-baseline.md) record a 1.42%
enwik8 improvement at the default max-profile budget with `--allocation context`,
plus the measured limits of the predictor and BMP experiments.

The code began as an integer-for-integer port of a Rust crate and keeps that
crate's layout and names; since format 13 it defines its own format and
refuses versions 10–18. The header of
[adapt.h](adpat/c/adapt.h) records what changed.

## Layout

| Path | What |
|---|---|
| [adpat/c/](adpat/c/) | The codec: `adapt.h` (public interface), `adapt_model.h` (the predictor, coder and framing, shared by both units), `adapt_decode.c`, `adapt_encode.c`, `adapt_archive.c` (the folder archive stream), `cadapt.c` (the command line), `Makefile` |
| [adpat/c/tests/](adpat/c/tests/) | C unit tests for both units, the CLI's Python tests, the coverage gate's tests, and the committed vectors |
| [adpat/c/fuzz/](adpat/c/fuzz/) | A libFuzzer harness for the two decoders |
| [adpat/c/check-gcov.py](adpat/c/check-gcov.py) | The branch-coverage gate: 80 % for the library, 60 % for the CLI |
| [bench/](bench/README.md) | Retained historical benchmark scripts, measurements and plots |
| [research/](research/arxiv-compression-review.md) | A review of the compression literature against this codec, the enwik8 baseline, and the [per-script measurements](research/scripts-measurement.md) |
| [static/corpus/](static/corpus/scripts/manifest.json) | The benchmark's fixtures: enwik8, text8, and the six per-script rows with their manifest |
| [tools/](tools/provenance.py) | The provenance tool: the SPDX SBOM, the build manifest, and the rebuild that confirms it |
| [sbom.spdx.json](sbom.spdx.json), [build-manifest.json](build-manifest.json) | What the codec is made of, and the build it reproduces |
| [LICENSE](LICENSE), [SECURITY.md](SECURITY.md), [SUPPORT.md](SUPPORT.md), [CHANGELOG.md](CHANGELOG.md) | The terms, the security policy, what support exists, and what changed |
| [CLAUDE.md](CLAUDE.md) | Working rules for contributors and agents |

## Build

```sh
make            # adpat/c/build/libadapt.a and adpat/c/build/cadapt
make test       # the C unit tests on the committed vectors
make check      # build, the C tests, the Python tests, then the coverage gate
make clean
```

Two more targets live in [adpat/c/Makefile](adpat/c/Makefile): `make coverage`
(the branch-coverage gate on its own) and `make sanitize` (the C suites under
the sanitizers); both are described under *Tests and gates*.

The root `Makefile` forwards these to [adpat/c/Makefile](adpat/c/Makefile),
which honours `CC`, `CFLAGS`, `LDFLAGS` and `BUILD=`. The defaults are the
strictest the code is written for: `-std=c99 -O3 -Wall -Wextra -Wpedantic
-Werror`. Two build switches:

- `THREADS=` builds the encoder's proof in line instead of on a POSIX
  thread, with nothing beyond the C standard library.
- `-DADAPT_NO_MULTIVERSION` (add it to `CFLAGS`) builds the per-bit path for
  the baseline instruction set only. By default, on x86-64 under GCC or
  Clang, it is compiled twice — baseline and x86-64-v3 — and the pair is
  chosen once per model from a CPU test; `ADAPT_ISA=baseline` in the
  environment selects the portable pair on any CPU (both write the same
  bytes, which the CLI tests hold them to).
  `-DADAPT_NO_HUGEPAGES` likewise takes the `calloc` path for the model's
  tables instead of an anonymous mapping advised `MADV_HUGEPAGE`.

## Command line

```
cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --max] [-0 .. -11]
              [--allocation window|context] [--threads <n>] [--segment-bytes <bytes>]
              [--adaptive rates|sparse|retained|auto] [--transform bmp]
cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]
cadapt info   <file>
cadapt --version
```

The manual page, [`adpat/c/cadapt.1`](adpat/c/cadapt.1) (`man -l adpat/c/cadapt.1`),
sets out the two axes every run is made of — the profile (`--fast`, default,
`--max`: which model) and the memory tier (`-0` … `-11`: how much it may
fill) — with the floors, ceilings and the measured bytes each tier buys.

`<input>` or `<output>` may be `-` for standard input or output. Without
`-o`, `encode` writes `<input>.adpt` and `decode` strips that suffix (or
appends `.out`). An existing output is kept unless `--force` is given. Both
directions stream: codec working memory is the model, window, block workspace
and reserved headroom. The memory tier `-0` … `-9` is what xz takes to
compress at the same preset — 3, 9, 17, 32, 48, 94, 94, 186, 370 and
674 MiB — and `-10` and `-11` are 1 and 2 GiB, the two tiers above xz's
ladder; `-9` is the default, the last tier given wins, and a tier below the
profile's smallest model (`--help` prints the two: about 27 and 70 MiB) is
raised to it, never refused. The tier is encode's only memory setting: it
bounds the model, window and workspace, and the file records the model it
produced, so **the tier a file is written at is the memory every decoder of
it needs** (`RUN-1` in [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md)).
Verification and segments can require more than one model. `decode`
allocates whatever the container's header names — the container alone
decodes (`FMT-6`) — and its two caps apply only when given: `--max-memory
<bytes>` refuses a container whose model needs more, with the bytes it
needs, and `--max-output <bytes>` refuses one that declares more decoded
bytes (for a folder, the combined logical sizes of restored files, sparse
holes and each hard-link alias counted; a refusal discards the staged
output). The format also bounds table and window sizes. Threaded decoding
also counts the buffered container: regular files are buffered only when
the input and a decoder fit together; larger inputs and pipes use one
streaming decoder.
Folder encoding additionally
retains an entry/path inventory, and restoration retains pending directory
metadata; this CLI bookkeeping is outside the codec's working-memory bound.

- `--fast` writes the fast profile (version 20 or 23): two hashed contexts, one
  arbiter, about three times the decoding speed for more bytes (5 % on the
  49 KB `text` vector, about a twelfth on enwik8). `--max` writes the max
  profile (version 21 or 24): ten more contexts, run memories and a wider
  second estimation stage, fewer bytes for about 1.8 times the time; its
  smallest model is larger (`--help` prints both minimums), so the low tiers
  clamp higher under `--max`.
- `--verify` proves the output as it is written, with a second model
  decoding every block, when the budget holds two models; for a file
  written to a file it falls back to a second pass with one. Encoding skips
  the proof by default and says so on stderr; `--no-verify` skips it
  explicitly.
- `--allocation context` spends the budget on the context table before growing
  the recall window. `--allocation window` is the unchanged default. Both use
  existing header fields and compose with the profiles and candidate search.
  Under `--max`, a budget that would stop the table at 2^22 blocks or fewer,
  short of the input's target, buys a table twice the size with compact side
  tables (versions 53/56) and, while the table stays at that size, a lean
  context set (54/57): on enwik8, 19,657,760 bytes in 128 MiB. What the
  budget still holds past the window goes to the table an eighth of its
  next doubling at a time (versions 58/61): on enwik8, 18,780,150 bytes
  in 512 MiB against 18,831,629 with the table stopped at 256 MiB.
  Either allocation can produce a larger file; choose from measured complete
  containers. More of the allowed memory can be used by the context policy.
- `--adaptive rates|sparse|retained` selects experimental causal predictors:
  fast/slow calibration, then sparse byte contexts, then retained calibration
  states. These compose with `--max`. Each adds 2,837,272 bytes of model state on the tested platform,
  charged to the memory budget. These profiles can increase size or runtime.
- `--adaptive auto` spools the input once and tries the chosen base profile plus all three
  adaptive profiles that fit the budget. For eligible BMP24 files it also
  tries their transformed variants, retaining the smallest complete container
  including metadata. It uses up to eight sequential encodes and temporary
  disk space for the source plus two containers. Verification remains opt-in;
  `auto --verify` checks the selected container with one model, including
  for pipes. This is a finite candidate search, not a global optimality claim.
- `--transform bmp` explicitly selects reversible color differences and
  horizontal residuals for uncompressed 24-bit BMP files. It combines with
  normal, max or an explicit adaptive profile and preserves headers, row padding,
  tails and boundary pixels. It adds a 12-byte descriptor and a 256 KiB encoder
  workspace reservation. Automatic selection tries this transform on whole
  BMP inputs; embedded images inside folders/PDF/EPUB are not transformed.
- `--transform text` asks for the text transform and `--transform none`
  refuses it; a file of 8 MiB or more whose first block looks like text
  takes it by itself (versions 62–66, 67–71 with metadata). A word both
  sides have seen written out three times becomes, from then on, a code
  of one to three bytes the model reads as one symbol; each side builds
  the same bounded book as the file is coded, nothing is sent ahead and
  nothing is built in, and every other byte passes as it is. Measured on
  enwik8 at 128 MiB with `--max --allocation context`: 19,657,760 →
  18,971,412 bytes in fewer seconds. Below 8 MiB it costs, which is what
  the length rule is for; a pipe's length is unknown, so a pipe gets it
  only when asked.
- `--threads <n>` with `n ≥ 2` cuts the input into segments of
  `--segment-bytes` (default 16 MiB, whole 256 KiB blocks) that each start
  the model afresh, and codes them at once, on as many of the `n` threads
  as the memory budget holds a model, a segment and a block's workspace
  for (the report says how many); `decode --threads <n>` of a file reads
  its segment table and decodes the segments the same way. The bytes are
  those the segments coded in order produce; a segment's model starts
  afresh, so segmenting costs ratio (two 4 MiB segments of the 8 MiB
  enwik8 proxy under `--max`: 1,778,553 bytes against 1,716,715 whole) and
  is never the default. With `--verify`, each thread proves its own
  segment before it is written. A pipe is segmented the same way, its
  segments read ahead into memory during encoding. Decoding a pipe uses
  one streaming decoder, including when `--threads` is requested.
- An `<input>` that is a folder is walked into one archive stream and coded
  as one container (the header carries an archive flag; `info` reports
  "holds a folder"). `decode` restores it under the output name — every
  file byte for byte, every directory and symbolic link in its place — and
  refuses to write a folder to standard output. On Windows a symbolic link
  is neither packed nor restored.
- Named files and folders carry permission bits and access/modification
  timestamps in the container, including the folder root, empty directories
  and symbolic links. On POSIX, timestamps retain nanoseconds and signed Unix
  seconds; symlink timestamps are set without following the link. Directory
  metadata is applied after restoring its children. Reading the source can
  advance its access time, so identical containers require identical source
  bytes **and metadata** at the start of each encode.
- Metadata is checked after application and before publication. A filesystem
  that cannot reproduce the recorded mode or timestamp fails restoration
  instead of silently rounding or dropping it. Windows currently records
  the C runtime's modes and whole-second timestamps; it rejects unsupported
  modes, nanosecond timestamps and symlinks. This release's metadata tests
  run on POSIX; Windows behavior has not been validated.
- A folder's **hard links, holes and extended attributes** travel too
  (`FMT-7` in [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md); archive version
  3, which `info` still reports as "a folder"). The portability rules:
  - *Hard links.* Names that share one inode inside the folder are packed
    once, the first name (in the archive's byte order) carrying the bytes,
    every later name a link to it, and are restored as hard links. A name
    whose other names lie outside the folder is packed as a plain file. On
    Windows a hard-link entry is restored as a copy of its target's bytes.
  - *Holes.* Where the platform can tell holes from data (`SEEK_HOLE`:
    Linux, macOS, FreeBSD), a file with holes is packed as its data extents
    only and restored to its apparent length with the holes left as holes;
    a filesystem that keeps no holes fills them with zeros, which reads the
    same. A file of more than 65,535 extents is packed dense. Windows packs
    every file dense and restores extents by seeking, so holes come back as
    zeros.
  - *Extended attributes and ACLs.* On Linux, `user.*` attributes and the
    two POSIX ACL attributes (`system.posix_acl_access`,
    `system.posix_acl_default`) are packed for every entry and restored by
    name, each read back after it is set; a filesystem that cannot hold an
    attribute fails restoration rather than dropping it. `security.*` and
    `trusted.*` belong to the kernel and to root and are left alone. A
    directory's attributes are applied after its children, with its
    metadata, so a default ACL is not inherited by children the archive
    records differently. On macOS the attributes `listxattr` reports are
    packed and restored; macOS ACLs are not attributes there and are not
    preserved. Windows packs no attributes and refuses to restore an archive
    that carries any. Attribute values are limited to 64 KiB.
  - *Not preserved.* Creation and status-change times (filesystem-managed),
    and ownership (which needs root to restore and is not in `FMT-7`).
- Encoding standard input has no source filesystem metadata. Decoding to
  standard output emits only bytes. Older containers remain readable; when
  they lack metadata, restored files take the permissions allowed by umask.

A file output is staged beside its target and published by name only once
complete, so an interrupted run leaves no partial target. Without
`--force` the publish uses `link()`, which refuses a target that appeared
meanwhile but is unavailable on filesystems without hard links (FAT, exFAT,
some network mounts); pass `--force` there.

```sh
cadapt encode corpus.txt                   # corpus.txt.adpt, not verified
cadapt encode corpus.txt --verify --max    # proven as written, max profile
cadapt decode corpus.txt.adpt -o back.txt
cadapt info corpus.txt.adpt                # header, sizes, contexts, crc32
cat corpus.txt | cadapt encode - | cadapt decode - > back.txt
```

Exit codes: 0 success, 1 a codec or I/O failure, 2 a usage error.

## Library

[adapt.h](adpat/c/adapt.h) is the whole interface. In brief:

- `adapt_decode()` decodes a file held in memory and returns its bytes only
  after the checksum matched; `adapt_decode_stream()` pulls a file through a
  reader callback and pushes each block to a writer as it is recovered.
- `adapt_encode()` codes bytes in memory into one file;
  `adapt_encode_stream()` reads a stream once and writes as it goes.
  `adapt_encode_verified()` and `adapt_encode_stream_verified()` prove the
  result decodes back to the input before returning it;
  `adapt_verify_stream()` is the same proof after the fact.
- `adapt_sizes_for_input()` fits the model's tables and window to an input's
  length and a memory budget; `adapt_sizes_fast()` and `adapt_sizes_max()`
  turn sizes to those profiles; the `_memory_bound()` functions say what a
  run at given sizes needs, before anything is allocated.
- Every failure is a typed `adapt_status`, with the numbers a message needs
  in an `adapt_error`; `adapt_error_message()` renders it.

Link against `build/libadapt.a` with `-pthread` (or build with `THREADS=`).

## Tests and gates

- `make test` runs the C suites — `tests/test_adapt_decode.c` and
  `tests/test_adapt_encode.c` on the vectors in `tests/vectors/`, and
  `tests/test_adapt_archive.c` on the archive stream: the
  library's contract, every framing fault on hand-built bytes, the limits,
  the stream callbacks, the messages, round trips through every profile, and
  the encoder's output pinned byte for byte to the committed vectors. The
  short vectors (`abc`, `hello`, `empty`) cover one block; the larger ones
  pin the choices a short input never reaches — `text` (49,152 bytes of
  enwik8, the Wikipedia excerpt the benchmark uses, CC BY-SA) chooses a
  class width and a context set, in all three profiles; `records` (a
  synthetic raw RGB image) a record period; `segmented` (300,000 bytes of
  enwik8) two segments; `cyrillic` and `cjk` (32 KiB of Russian and Chinese
  Wikipedia text from the corpus rows below) two scripts of two and three
  bytes a character (`FMT-8`). Regenerate byte-stream vectors with the
  library encoder and their explicit sizes/profile/segment settings after a
  deliberate format change, never to make a test pass. Named CLI inputs
  now include metadata and are unsuitable as metadata-free golden vectors.
- **The mixed acceptance corpus** (`DLV-3`): the Silesia corpus (twelve
  files, 212 MB — prose, an executable, a medical image, a database,
  office documents, source, XML, an X-ray, a star catalogue, a chemical
  database) and the Canterbury corpus (eleven small files, 4 KB to 1 MB)
  are fetched by [tools/fetch_corpus.py](tools/fetch_corpus.py) (`make
  corpus`) into `static/corpus/mixed/`, which is git-ignored — they are
  not this project's to redistribute (`DLV-4`); the committed
  [manifest](static/corpus/mixed/manifest.json) pins every archive and
  file by SHA-256. `make corpus-floor` encodes every file under the default
  command and under `--max --allocation context -6` and fails when any
  container is larger than its locked floor
  ([tools/corpus_floor_default.json](tools/corpus_floor_default.json),
  [tools/corpus_floor_max6.json](tools/corpus_floor_max6.json)): a
  default or a lever is accepted on these files, enwik8 being one benchmark
  row beside them, never the judge.
- **Every script, measured** (`FMT-8`): the corpus holds six rows of about
  2 MiB of Wikipedia article text each — Latin (English), Cyrillic
  (Russian), CJK (Chinese, Japanese) and diacritic-heavy Latin (Vietnamese,
  Czech) — in [static/corpus/scripts/](static/corpus/scripts/), fetched
  reproducibly by [bench/fetch_scripts.py](bench/fetch_scripts.py) with a
  manifest naming every article and revision. Their measurements, by the
  benchmark on one binary, are in
  [research/scripts-measurement.md](research/scripts-measurement.md); the
  model is byte-level throughout, so a script is never decoded, only seen.
- `make check` is held to **120 s** of wall time (`CHECK_SECONDS` in the
  root Makefile; it runs under `timeout` and fails when exceeded — LOCKS.md,
  2026-09-15). Every suite runs at once under `make -j`; the CLI suite's
  megabyte cases run only in `make check-long` (`ADAPT_LONG_TESTS=1`).
- `make check` adds the Python tests — the CLI command by command
  (`tests/test_cadapt.py`), the coverage gate's own tests
  (`tests/test_check_gcov.py`), and the allocation SDK probe
  (`tests/test_allocation_probe.py`) — then runs the coverage gate. The root
  check also tests provenance and codec-owned ratio floors under `tools/`.
  No build or correctness test imports `bench/` or the standalone benchmark.
- **Branch coverage** is gated by `check-gcov.py` at 80 % for the library
  and 60 % for the CLI; `make coverage` builds every unit with GCC's
  instrumentation into `build-cov/` (one instantiation of the per-bit path,
  so the instruction-set clones a machine does not run are not counted as
  branches never taken), runs the C suites and the CLI tests against it, and
  feeds gcov's JSON to the gate.
- **Sanitizers**: `make sanitize` builds the C suites with clang under
  AddressSanitizer and UndefinedBehaviorSanitizer (leak detection included,
  every fault fatal) into `build-sanitize/` and runs them.

- **Fuzzing**: [fuzz/fuzz_decode.c](adpat/c/fuzz/fuzz_decode.c) feeds
  arbitrary bytes to both decoders under libFuzzer and aborts if they
  disagree. It admits models with a decoding memory bound up to 128 MiB,
  plus 32 KiB for output. `make -C adpat/c fuzz-smoke` builds with clang and
  processes 1,000 inputs using copies of the committed vectors in the build
  directory (`FUZZ_RUNS` changes the count). The normal test and coverage
  gates also prove that the actual harness accepts modeled normal, fast,
  and max fixtures, honors its output cap, and refuses oversized models.

## Licence, security, support, provenance

The code and documentation are **MIT** ([LICENSE](LICENSE)); the corpus
fixtures are Wikipedia text under CC BY-SA, as the licence file states.
Security problems go to the address in [SECURITY.md](SECURITY.md), not the
issue tracker — the policy names the threat model, the supported versions
and the response windows. [SUPPORT.md](SUPPORT.md) says what support exists
and how to arrange more; [CHANGELOG.md](CHANGELOG.md) what changed, release
by release.

Every release carries its provenance (`DLV-2` in
[docs/CONSTRAINTS.md](docs/CONSTRAINTS.md)): an SPDX 2.3 SBOM,
[sbom.spdx.json](sbom.spdx.json), naming the package, every source by hash
and each dependency with its licence; and a build manifest,
[build-manifest.json](build-manifest.json), recording the sources' hashes,
the compiler and flags, and the hash of every output. Both come from
[tools/provenance.py](tools/provenance.py):

```sh
make sbom        # regenerate sbom.spdx.json for the tree as it is
make manifest    # record the build in adpat/c/build
make reproduce   # rebuild in a scratch directory; identical bytes, or the reason
```

`make check` fails when the committed SBOM is stale, so a source change
carries its SBOM with it; `make reproduce` against the committed manifest
confirms that the toolchain it names rebuilds the same bytes.

## Format

A file is an 8-byte fixed header (signature `ADPT`, version, table size and
class code, recall size and context-set code, window size and flags), an
optional 2-byte record period, the sized max profile's 1-byte sizing byte,
a BMP transform's 12-byte descriptor, an optional metadata extension,
blocks, an optional segment table, and a 13-byte trailer — in that order,
each present only when its version or flag says so. The constants that pin
it are the `ADAPT_*` macros at the top of [adapt.h](adpat/c/adapt.h); a
change to any of them is a new version, and every version once published
stays readable (`DLV-1` in [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md)):

| Version | Stream | With metadata | Model |
|---|---|---|---|
| 10–18 | refused | — | earlier layouts, no published files |
| 19 / 20 / 21 | normal / fast / max | 22 / 23 / 24 | power-of-two tables |
| 25–27 | normal + `--adaptive` rates / sparse / retained | 28–30 | experimental causal predictors |
| 31–34 | normal + `--transform bmp`, alone or with each predictor | 35–38 | the BMP24 residual transform, a 12-byte descriptor |
| 39–41 | max + `--adaptive` | 42–44 | the same predictors over the max profile |
| 45–48 | max + `--transform bmp` | 49–52 | the transform over the max profile |
| 53 | max, compact side tables | 56 | table 2^22 blocks or fewer |
| 54 | max, compact and lean | 57 | as 53 without three contexts |
| 58 | max, sized | 61 | a sizing byte: compact, lean, the table's eighths |
| 55, 59, 60 | refused | — | no format |

The encoder writes 19–24 by default, 53/54 (56/57) when a small tier under
`--allocation context` earns them, and 58 (61) only when a fraction is
taken; the ranges 25–52 are written only with `--adaptive` or `--transform`.
Versions 22, 23 and 24 use the same predictors as 19–21 and append a
32-byte extension after the record period (or fixed header when absent):

| Bytes | Field (little-endian) |
|---|---|
| 4 | Permission bits, restricted to `07777` |
| 8 + 4 | Access time: signed Unix seconds, nanoseconds below 1,000,000,000 |
| 8 + 4 | Modification time: signed Unix seconds, nanoseconds below 1,000,000,000 |
| 4 | CRC-32 of the entire header through the preceding metadata fields |

For folders, the extension holds the root metadata. The decoded archive
stream starts with `ADPTARC3`, carrying metadata and extended attributes,
hard links and sparse extents. Earlier `ADPTARC1` and `ADPTARC2` archive streams
remain readable. Entry metadata is covered by the outer payload CRC-32. Older codec releases do not read
versions 22–24. The byte-oriented library encoders still emit versions
19–21; `adapt_header_add_metadata()` adds the extension and
`adapt_read_metadata()` reads it. Use `adapt_header_len()` for the actual
container header size; `adapt_sizes_header_bytes()` gives the base encoder
header size without metadata.

## Benchmark ownership

General benchmark runs belong to the independent `codecbench` tool. Install it
separately and describe the `cadapt` executable with an external command adapter;
its examples include normal, fast, max, adaptive and segmented configurations.
The codec builds and passes `make check` without either `bench/` or codecbench.
`make bench` and `make plot` report this migration instead of invoking the old
collector. Historical scripts, result bundles, plots and research links remain
under `bench/`; they have not been deleted.

The codec-specific ratio and throughput regression gates remain here:
`make gate`, `make ratio-floor` and `make corpus-floor` use
`tools/check_ratio_floor.py` with the canonical `tools/ratio_floor.json`
and the two corpus floor files. Their thresholds and verification behavior
are unchanged. The old copies in `bench/` are historical snapshots; update the
canonical files in `tools/` for any separately approved floor change. Allocation,
adaptive-search and filesystem-restoration correctness stay in the codec tests.
