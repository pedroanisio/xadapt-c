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
transform ahead of the normal and fast models uses 62 and 63, 67 and 68
with metadata, and ahead of the max profile it is version 72
or 75, its sizing byte naming the transform and the arbiters' 1/32
starting weight (64 to 66 and 69 to 71, the transform's own max versions,
are read and no longer written). Every `--max` encode is now the sized
version 72 (75 with metadata), its sizing byte's bit 7 naming the second max model —
int16 arbiters, four re-purposed context slots, four-way recall buckets
and a fifth arbiter (the `Second max model` entry in `LOCKS.md`) — so
21, 24, 53, 54, 56 and 57 are read and no longer written; a max encode
whose first block prefers the class-keyed arbiters writes 80 or 81 (82,
83 with metadata) instead, and a normal encode 84 (85), whose policy
byte also carries the text transform. The mid
profile writes 76 (78 with metadata), 77 (79) under the text transform. The decoder
reads every version ever published; versions 10–18 and the unused
numbers (55, 59, 60, 73, 74) are refused. The CLI uses native filesystem APIs to
restore metadata.

Versions 58/61 briefly carried two model2 predictors under the same header.
For tables smaller than 2^21, the decoder tries them sequentially and checks
the recovered checksum before emitting bytes. This uses temporary disk for
the consumed input prefix and tentative output, with only one model in memory.
Versions 72/75 identify the current predictor without that fallback.

The [experimental implementation report](research/causal-byte-implementation.md)
specifies the adaptive algorithms, their format versions, limits and
measurement protocol; the other dated notes are indexed in
[research/README.md](research/README.md). The numbers that stand are the
locked floors in [tools/ratio_floor.json](tools/ratio_floor.json), each
with the decision that moved it in [LOCKS.md](LOCKS.md).

The code began as an integer-for-integer port of a Rust crate and keeps that
crate's layout and names; since format 13 it defines its own format and
refuses versions 10–18. The header of
[adapt.h](adpat/c/adapt.h) records what changed.

## Layout

| Path | What |
|---|---|
| [adpat/c/](adpat/c/) | The codec: `adapt.h` (public interface), `adapt_model.h` (the predictor, coder and framing, shared by both units), `adapt_text.h` (the text transform), `adapt_bmp.h` and `adapt_adaptive.h` (the BMP transform and the adaptive predictors), `adapt_decode.c`, `adapt_encode.c`, `adapt_archive.c` (the folder archive stream), `cadapt.c` (the command line) with `cadapt_fs.c`/`cadapt_fs.h` (the extended attributes and sparse maps it preserves) and `cadapt_platform.h` (the feature macros and the calls whose names differ off POSIX), its manual `cadapt.1`, `Makefile` |
| [adpat/c/tests/](adpat/c/tests/) | C unit tests for both units, the CLI's Python tests, the coverage gate's tests, and the committed vectors |
| [adpat/c/fuzz/](adpat/c/fuzz/) | A libFuzzer harness for the two decoders |
| [adpat/c/check-gcov.py](adpat/c/check-gcov.py) | The branch-coverage gate: 80 % for the library, 60 % for the CLI |
| [bench/](bench/README.md) | Retained historical benchmark scripts, measurements and plots |
| [docs/](docs/CONSTRAINTS.md) | What the codec is held to ([CONSTRAINTS.md](docs/CONSTRAINTS.md)) and the dated performance audits |
| [LOCKS.md](LOCKS.md) | The decisions that are settled, each with the measurements that settled it |
| [research/](research/README.md) | Dated research notes — the literature, the byte-only strategy, the adaptive experiments, the LTCB tables — indexed with their dates |
| [static/corpus/](static/corpus/scripts/manifest.json) | The benchmark's fixtures: enwik8, text8, and the six per-script rows with their manifest |
| [tools/](tools/provenance.py) | The provenance tool (the SPDX SBOM, the build manifest, and the rebuild that confirms it), the floor gate [check_ratio_floor.py](tools/check_ratio_floor.py) with its [ratio_floor.json](tools/ratio_floor.json) and [render_floors.py](tools/render_floors.py), and the [LTCB tooling](tools/ltcb/) |
| [sbom.spdx.json](sbom.spdx.json), [build-manifest.json](build-manifest.json) | What the codec is made of, and the build it reproduces |
| [LICENSE](LICENSE), [SECURITY.md](SECURITY.md), [SUPPORT.md](SUPPORT.md), [CHANGELOG.md](CHANGELOG.md) | The terms, the security policy, what support exists, and what changed |
| [CLAUDE.md](CLAUDE.md) | Working rules for contributors and agents |
| [Makefile](Makefile) | The root targets: build, `test`, `check`, `gate`, `ratio-floor`, `sbom`, `manifest`, `reproduce`, `ltcb` |

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
cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--fast | --mid | --max] [-0 .. -11]
              [--threads <n>] [--segment-bytes <bytes>]
              [--adaptive rates|sparse|retained|auto] [--transform bmp|text|none] [--allocation window|context]
              [--contexts auto|lean]
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
  49 KB `text` vector, about a twelfth on enwik8). `--mid` writes the mid
  profile (version 76, 78 with metadata; 77 and 79 under the text
  transform): the normal profile's tables and estimation stages with seven
  fixed hashed contexts (orders 2, 3, 4 and 6, word, record, the class
  history) and its first arbiter alone — measured against the normal
  profile, 1.4 % more bytes on enwik8 (18,761,302 against 18,500,473)
  in 29 % less encode time and 24 % less decode time, 2.4 % more on
  enwik9 in 28 % and 26 % less; 2.1 to 2.4 times faster than `--max`
  for 4.8 % (enwik8) to 17.5 % (enwik9) more bytes. `--max`
  writes the max profile (versions 72, 80 or 81; 75, 82 or 83 with
  metadata; the normal profile takes the same key policies as version
  84, 85 with metadata, its policy byte also carrying the text
  transform): ten more contexts, run memories and a wider
  second estimation stage, fewer bytes for about 1.8 times the time; its
  second, third and fourth arbiters take one of three key policies —
  classic, the byte classes with the nesting state, or the classes with
  recall — whichever the first block prefers (`cadapt info` names it as
  `arbiter keys`), the class keys writing 0.7 % fewer bytes on text at
  the same time and memory; its
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
  Under `--max`, a tier that would stop the table at 2^22 blocks or fewer,
  short of the input's target, buys a table twice the size with compact side
  tables (versions 53/56) and, while the table stays at that size, a lean
  context set (54/57); what the tier still holds past the window goes to
  the table an eighth of its next doubling at a time (versions 58/61). The
  measurements behind each rule are in the LOCKS.md entries that made it.
  Either allocation can produce a larger file; choose from measured complete
  containers. More of the allowed memory can be used by the context policy.
- `--contexts lean` takes the lean context set outright instead of choosing
  the set from the first block: word and field are left out and, under
  `--max`, o8, word3 and wordc too (the lean bit, now written with the full
  side tables as well — a reader older than this option refuses such a
  file). It buys time for bytes on text: the measurements are in the
  LOCKS.md entry that made it. `--contexts auto`, the default, chooses the
  set as before and writes the same bytes. Not with `--fast`.
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
  takes it by itself (versions 62/63, 67/68 with metadata; under `--max`
  the sized version 72/75, whose sizing byte names the transform and
  starts every arbiter's trust in a context at 1/32 rather than 1/4,
  measured on the first 16 MiB of enwik8 at 128 MiB as 3,336,047 →
  3,330,628 bytes and at the default tier as 3,320,062 → 3,296,290). A word both
  sides have seen written out three times becomes, from then on, a code
  of one to three bytes the model reads as one symbol; each side builds
  the same bounded book as the file is coded, nothing is sent ahead and
  nothing is built in, and every other byte passes as it is. On enwik8 it
  took about 3.5 % off the container and a quarter off the time at every
  tier (the LOCKS.md entry has the runs; the floors below carry what stands).
  Below 8 MiB it costs, which is what the length rule is for; a pipe's
  length is unknown, so a pipe gets it only when asked.
- `--threads <n>` with `n ≥ 2` cuts the input into segments of
  `--segment-bytes` (default 16 MiB, whole 256 KiB blocks) that each start
  the model afresh, and codes them at once, on as many of the `n` threads
  as the memory budget holds a model, a segment and a block's workspace
  for, and never more than the host has CPUs online (the report says how
  many ran, and notes a request past the CPUs); `decode --threads <n>` of
  a file reads its segment table and decodes the segments the same way.
  The sizes are fitted to the `n` asked for whatever the host, so the
  bytes are the same on every machine (RUN-5): `n` chooses the container,
  the host only how many workers code it at once. The bytes are those the
  segments coded in order produce; a segment's model starts afresh, so
  segmenting costs ratio (two 4 MiB segments of the 8 MiB enwik8 proxy
  under `--max`: 1,778,553 bytes against 1,716,715 whole) and is never
  the default. With `--verify`, each thread proves its own
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
  4, which `info` still reports as "a folder"). The portability rules:
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
    `system.posix_acl_default`) are packed for the root directory and every entry and restored by
    name, each read back after it is set; a filesystem that cannot hold an
    attribute fails restoration rather than dropping it. `security.*` and
    `trusted.*` belong to the kernel and to root and are left alone. A
    directory's attributes are applied after its children, with its
    metadata, so a default ACL is not inherited by children the archive
    records differently. Access ACLs are applied after other attributes so a
    read-only ACL cannot prevent their restoration. Linux restoration removes inherited access and default
    ACLs before creating children, so destination ACLs cannot add permissions
    absent from the archive. On macOS the attributes `listxattr` reports are
    packed and restored; macOS ACLs are not attributes there and are not
    preserved. Windows packs no attributes and refuses to restore an archive
    that carries any. Attribute values are limited to 64 KiB. Deferred directory
    and root attributes are journaled to temporary disk and replayed after the
    archive checksum passes, keeping memory independent of their count. The
    journal is automatically removed on success or failure; storage errors
    fail restoration before publication.
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

On POSIX, file outputs and folders are staged inside a private `0700` wrapper.
File metadata is applied through a retained file handle; publication uses
`linkat()` or `renameat()` relative to retained directory handles. Retained
directory handles anchor restoration, metadata, cleanup and publication even
if another process replaces the staging name. A wrapper renamed by another
process may remain as an empty directory after completion.
Temporary entries retain owner access under restrictive umasks; final modes
match the archive, or the original umask for legacy entries. Read-only root
modes are restored through the retained handle immediately after publication.

Folder publication without `--force` uses an atomic no-replace rename on
Linux and macOS; Windows rename also refuses existing destinations. If the
platform or filesystem lacks the operation, the command fails and asks for
`--force` rather than risking replacement after a separate existence check.

Forced folder publication preserves the existing target until an atomic rename
or exchange succeeds. POSIX rename replaces empty directories; replacing a file
or symbolic link uses Linux/macOS atomic exchange. If the filesystem lacks that
operation, the command refuses replacement and preserves the target. Windows
folder publication refuses existing targets even with `--force`. Replaced
entries are never recursively deleted. If cleanup after an exchange fails, the
command reports that publication succeeded and retains the previous target in
the private staging wrapper for recovery.

On POSIX, `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGPIPE` and `SIGXFSZ` (file-size
limit) cancel encoding or decoding through the normal cleanup path. Blocked stream I/O wakes to discard
staging; active codec work reaches its next I/O boundary first. The command
exits with status `128 + signal number`. Uncatchable termination (`SIGKILL`)
or machine failure can still leave staging files behind.

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
  2026-09-15). Every suite runs at once under `make -j`, so the budget is
  wall time by nature; the check prints the seconds it took and the load
  average, so a run that failed the budget on a busy host is told from a
  code failure, and `CHECK_SECONDS=240 make check` widens it for that run.
  The CLI suite's megabyte cases run only in `make check-long`
  (`ADAPT_LONG_TESTS=1`).
- `make check` adds the Python tests — the documents against the code
  (`tools/test_docs.py`: the usage block and flags against `--help`, the
  version map against `adapt.h`, the floors block, the layout table, every
  link), the CLI command by command
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
| 58 | max, sized (legacy) | 61 | sizing byte; both historical model2 policies remain readable |
| 72 | max, sized | 75 | sizing byte with model2 bit 7 required; pressure-gated slots and fifth arbiter; every current `--max` encode |
| 62 / 63 | normal / fast, text transform | 67 / 68 | the book of words built as the file is coded |
| 64–66 | max, compact, lean, text transform | 69–71 | read only: the max profile writes 72 (75) under the transform |
| 76 / 77 | mid, plain / under the text transform | 78 / 79 | the normal profile's tables, seven fixed contexts, one arbiter |
| 80 / 81 | max, sized, the arbiters keyed by the byte classes and nesting / by the classes and recall | 82 / 83 | version 72's model and sizing byte; the encoder picks the key policy per file on the first block, 72 when the classic keys win |
| 84 | normal, the arbiters keyed the same way | 85 | version 19's model with one policy byte after the header: the key policy (1 or 2) and the text transform; 19 or 62 when the classic keys win |
| 55, 59, 60 | refused | — | no format |

The encoder writes 19–24 by default, 53/54 (56/57) when a small tier under
`--allocation context` earns them, and 58 (61) when a fraction is taken or
the max profile runs under the text transform; the ranges 25–52 are
written only with `--adaptive` or `--transform`.
Versions 22, 23 and 24 use the same predictors as 19–21 and append a
32-byte extension after the record period (or fixed header when absent):

| Bytes | Field (little-endian) |
|---|---|
| 4 | Permission bits, restricted to `07777` |
| 8 + 4 | Access time: signed Unix seconds, nanoseconds below 1,000,000,000 |
| 8 + 4 | Modification time: signed Unix seconds, nanoseconds below 1,000,000,000 |
| 4 | CRC-32 of the entire header through the preceding metadata fields |

For folders, the extension holds the root metadata. The decoded archive
stream starts with `ADPTARC4`, carrying root attributes before the version-3
entries with metadata, extended attributes, hard links and sparse extents.
See [the archive format](adpat/c/ARCHIVE.md) for the prefix and SDK callback.
Earlier `ADPTARC1`, `ADPTARC2` and `ADPTARC3` archive streams
remain readable. Entry metadata is covered by the outer payload CRC-32. Older codec releases do not read
versions 22–24. The byte-oriented library encoders still emit versions
19–21; `adapt_header_add_metadata()` adds the extension and
`adapt_read_metadata()` reads it. Use `adapt_header_len()` for the actual
container header size; `adapt_sizes_header_bytes()` gives the base encoder
header size without metadata.

## The Large Text Compression Benchmark

`cadapt 0.12.0 --max --allocation context` is listed on Matt Mahoney's
[Large Text Compression Benchmark](https://mattmahoney.net/dc/text.html)
since 2026-09-15 (the page's own changelog line: "Sep 15 2026 - added
cadapt 0.12.0, zmix 1.0, zmix 1.0.1"): enwik8 18,207,203 B, enwik9
150,063,628 B, decompressor zip 209,893 B, **total 150,273,521 B, rank 30
of 226**, 1,385 / 1,748 ns per byte, 661 MB, note 114 (tested by the
operator on the reference host). The entry sits between nanozip 0.09a
(149,328,821 B, 32,000 MB) and xwrt 3.2 (151,223,933 B, 1,691 MB). The
whole table as of that day is
[research/ltcb-main-table.csv](research/ltcb-main-table.csv), transcribed
by [tools/ltcb/transcribe_ltcb_table.py](tools/ltcb/transcribe_ltcb_table.py)
under the rules [research/ltcb-tables.md](research/ltcb-tables.md) records;
the submission itself is [docs/ltcb-submission.md](docs/ltcb-submission.md).

Two goals stand on this table (`DLV-3`, which keeps the rows the judge
of nothing but themselves):

- **A place in the top 20 — the canonical enwik9 goal.** Place 20 on the
  2026-09-15 table is paq8px_v206fix1 at 125,099,359 B of enwik9 plus
  decompressor, so the goal is **enwik9 at most 124,889,465 B** with the
  entry's 209,893 B decompressor (that total, less the zip, less one byte:
  a tie does not overtake). The figure is computed from the snapshot by
  [tools/ltcb/ltcb_goal.py](tools/ltcb/ltcb_goal.py) and never typed —
  the table moves as entries are added, a re-fetched snapshot moves the
  figure, and `make check` (`tools/test_docs.py`) holds this section and
  `docs/CONSTRAINTS.md` to the figure the tool prints.
- **The leader on memory footprint.** At 661 MB the entry already uses
  the least memory of every entry ranked above it — the least above is
  drt|lpaq9m at 1,542 MB, and every entry in the top 20 uses 3,751 MB or
  more (gmix v1) — and it is to stay the smallest of every entry it
  overtakes: a gain that needs a larger model than the tier holds is not
  this goal's gain (`RUN-1`). The same tool states the claim from the
  snapshot.

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
and the two corpus floor files, rendered here by
`tools/render_floors.py` (`make check` fails when this block is stale).
A time floor is in the measure its file names: the canonical floors are
**CPU seconds** — the encoder is one thread of pure CPU, so a host with
more runnable threads than CPUs adds runqueue wait to its wall time but
not to its CPU time — and `make gate` encodes the proxy three times for
the median (`--runs 3`), every run held to the same bytes. CPU time does
not remove a neighbour: another compressor's tables sharing the cache
slow the coder itself (13.1 s alone, 15.3 s of both CPU and wall time
beside two other encodes on the reference host), so a floor is locked on
a quiet host, and on a shared one the gate to trust is the relative one,
`--baseline-binary`, whose two binaries take turns run by run. The
corpus floors, locked from single wall-clock runs at a 25 % tolerance,
stay in wall seconds until they are re-locked.

<!-- floors:start -->
Locked 2026-09-16 on AMD Ryzen 5 5600X 6-Core Processor, `cadapt encode --max --allocation context --contexts lean`; a change may only
improve on these (`make gate` checks the first, `make ratio-floor` the rest):

| Fixture | Input | Container floor | Time floor |
|---|---:|---:|---:|
| enwik8-8MiB | 8,388,608 B | 1,669,716 B | 10.7 CPU s |
| enwik8-16MiB | 16,777,216 B | 3,236,535 B | 19.8 CPU s |
| enwik8 | 100,000,000 B | 17,800,141 B | 107.4 CPU s |
| enwik9 | 1,000,000,000 B | 145,784,134 B | 1,054.0 CPU s |
<!-- floors:end -->

Their thresholds and verification behavior
are unchanged. The old copies in `bench/` are historical snapshots; update the
canonical files in `tools/` for any separately approved floor change. Allocation,
adaptive-search and filesystem-restoration correctness stay in the codec tests.
