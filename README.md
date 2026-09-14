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
22, 23 and 24 respectively to carry filesystem metadata. The decoder reads
all six versions. This experimental worktree also reads versions 25–52 for
causal adaptive predictors and a reversible BMP24 transform. The CLI uses
native filesystem APIs to restore metadata.

The [experimental implementation report](research/causal-byte-implementation.md)
specifies the new algorithms, format versions, limits and measurement protocol.

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
| [bench/](bench/README.md) | Encoding telemetry on the enwik8 corpus, run history and plots |
| [research/](research/arxiv-compression-review.md) | A review of the compression literature against this codec, the enwik8 baseline, and the [per-script measurements](research/scripts-measurement.md) |
| [static/corpus/](static/corpus/scripts/manifest.json) | The benchmark's fixtures: enwik8, text8, and the six per-script rows with their manifest |
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
  Clang, it is compiled twice — baseline and x86-64-v3 — and bound at
  program start through an ifunc, which needs a glibc-style loader; on
  musl or another platform without ifunc support pass the switch.
  `-DADAPT_NO_HUGEPAGES` likewise takes the `calloc` path for the model's
  tables instead of an anonymous mapping advised `MADV_HUGEPAGE`.

## Command line

```
cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --max] [-0 .. -9]
              [--max-memory <bytes>] [--threads <n>] [--segment-bytes <bytes>]
cadapt decode <input> [-o <output>] [--force] [--max-output <bytes>] [--max-memory <bytes>] [--threads <n>]
cadapt info   <file>
cadapt --version
```

`<input>` or `<output>` may be `-` for standard input or output. Without
`-o`, `encode` writes `<input>.adpt` and `decode` strips that suffix (or
appends `.out`). An existing output is kept unless `--force` is given. Both
directions stream: codec working memory is the model, window, block workspace
and reserved headroom. `-0` … `-9` set the budget to what xz takes to compress
at the same preset — 3, 9, 17, 32, 48, 94, 94, 186, 370 and 674 MiB, a preset
below the smallest model raised to it — and `-9` (674 MiB) is the default:
memory buys the context table, the table buys ratio, and neither costs time
(see the [current baseline protocol](research/current-baseline.md)).
`--max-memory <bytes>` overrides any preset and bounds that allocation, which
chooses the model. Models shrink only to their supported minimum: **1 MiB is
insufficient**. `--help` prints the minimum normal single-model budget; an
insufficient budget fails before publication and reports required and allowed
bytes. Verification and segments can require more. A decoder holds the same
model, so a file made at `-9` needs about 370 MB to decode; `decode` allocates
whatever the container's header names — the container alone decodes (`FMT-6`
in [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md)) — and `--max-memory` is the one
cap that can refuse a valid container; the format bounds what a header can
name at about 1.2 GiB (the largest tables and window). `--max-output`
(default 1 GiB) caps the bytes `decode` writes. Folder encoding additionally
retains an entry/path inventory, and restoration retains pending directory
metadata; this CLI bookkeeping is outside the codec's working-memory bound.

- `--fast` writes the fast profile (version 14 or 17): two hashed contexts, one
  arbiter, about three times the decoding speed for about a twelfth more
  bytes. `--max` writes the max profile (version 15 or 18): more contexts and run
  memories, fewer bytes for about twice the time in the same memory.
- `--verify` proves the output as it is written, with a second model
  decoding every block, when the budget holds two models; for a file
  written to a file it falls back to a second pass with one. Encoding skips
  the proof by default and says so on stderr; `--no-verify` skips it
  explicitly.
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
- `--threads <n>` with `n ≥ 2` cuts the input into segments of
  `--segment-bytes` (default 16 MiB, whole 256 KiB blocks) that each start
  the model afresh. This implementation codes and decodes the segments in
  order, on one thread; the option exists for files that expect segments.
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
- **Every script, measured** (`FMT-8`): the corpus holds six rows of about
  2 MiB of Wikipedia article text each — Latin (English), Cyrillic
  (Russian), CJK (Chinese, Japanese) and diacritic-heavy Latin (Vietnamese,
  Czech) — in [static/corpus/scripts/](static/corpus/scripts/), fetched
  reproducibly by [bench/fetch_scripts.py](bench/fetch_scripts.py) with a
  manifest naming every article and revision. Their measurements, by the
  benchmark on one binary, are in
  [research/scripts-measurement.md](research/scripts-measurement.md); the
  model is byte-level throughout, so a script is never decoded, only seen.
- `make check` adds the Python tests — the CLI command by command
  (`tests/test_cadapt.py`), the coverage gate's own tests
  (`tests/test_check_gcov.py`), the benchmark's (`bench/test_telemetry.py`)
  — and then runs the coverage gate.
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
  disagree; its header holds the build line.

## Format

A file is an 8-byte fixed header (signature `ADPT`, version, table size and
class code, recall size and context-set code, window size and flags), an
optional 2-byte record period, an optional metadata extension, blocks, an
optional segment table, and a 13-byte trailer. The constants that pin it are the `ADAPT_*` macros at the
top of [adapt.h](adpat/c/adapt.h); a change to any of them is a new version.
Versions 19, 20 and 21 are the normal, fast and max byte-stream profiles.
Versions 22, 23 and 24 use the same predictors and append a 32-byte extension
after the record period (or fixed header when absent):

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
