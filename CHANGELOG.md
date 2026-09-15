# Changelog

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). Seeded from the commit history. -->

All notable changes to the codec, in the shape of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The version a binary reports
(`cadapt --version`) and the container format versions it reads and writes
are both named per release. Security fixes are listed under **Security**
and announced as [SECURITY.md](SECURITY.md) describes.

## [Unreleased]

The binary reports `0.12.0`; the tag is pending.

### Performance

- The second max model, bit 7 of the sized max profile's sizing byte
  (`adapt_sizes.model2`, `ADAPT_SIZING_MODEL2`), written by every `--max`
  encode: the first-layer arbiters' weights as int16 in 1/32768 with a
  rounded train step (`vpmulhrsw`) and sixteen-lane dot products
  (`vpmaddwd`), four context slots re-purposed (o5 carries the 21-byte
  class history, o8 the order-1 indirect context, word3 recall's expected
  byte, sparse2 its next two), recall buckets of four positions, and a
  fifth arbiter keyed by the bytes since a space and the previous byte's
  class. On the first 8 MiB of enwik8 (`--max --allocation context`)
  1,700,354 → 1,686,110 bytes with 4.9 % fewer instructions and a decode
  5 % faster; on the first 32 MiB 6,351,878 → 6,297,375. The int16 tables
  halve the arbiters' 36.5 MiB, so a small tier reaches its table sooner.
  Versions 21, 24, 53, 54, 56 and 57 are read and no longer written; every
  file decodes as written. The cast intrinsic that leaves a vector's upper
  half undefined is not used: gcc and clang builds write the same bytes.

### Security

- Anchor POSIX folder restoration, metadata, cleanup and publication to
  retained directory handles inside a private wrapper. Replacing the staging
  pathname cannot redirect writes or delete unrelated files during rollback.
- Remove inherited Linux access/default ACLs from folder roots before creating
  children, and from staged files before applying final permissions. Preserve
  archived ACLs without adding permissions inherited from the destination.
- Publish folders without `--force` using atomic no-replace operations, so
  a destination created during decoding is preserved. Unsupported platforms
  refuse the operation instead of using a racy existence check.
- Clean up staged file and folder outputs on POSIX `SIGINT`, `SIGTERM`,
  `SIGHUP`, `SIGPIPE` and `SIGXFSZ`, including blocked pipe I/O and file-size
  limits during encoding, decoding and sparse-file creation. Signal handlers only
  notify the command thread; cleanup runs through the ordinary failure path.
- Store CPU dispatch selection in each model, removing concurrent writes to
  shared dispatch state. Add model-isolation and concurrent prediction tests.
- Add CLI regression coverage for explicit text transforms with fractional
  context tables, already supported by the sizing-byte text flag.
- Keep POSIX output staging files at mode `0600` during processing and
  verification; apply recorded or umask-derived permissions only after success.
- Return worker failures safely when threaded codec callers pass `error=NULL`.
- Preserve folder-root extended attributes and Linux access/default ACLs in
  `ADPTARC4`. Restore root attributes after children; reject restoration failures
  and SDK readers without a root-attribute callback. Existing archive versions
  remain readable; older decoders reject the new archive magic.
- Enforce `decode --max-output` on the combined logical sizes of restored
  files, including sparse holes and each hard-link alias. Refuse an entry
  before creating or extending it when the remaining budget is too small,
  and discard the staging tree on refusal.
- Restore modeled-path coverage in the decoder fuzzer: allow a 128 MiB
  model working-memory bound and reserve 32 KiB separately for output.
  Regression tests run the real harness on modeled normal, fast and max
  fixtures; `make -C adpat/c fuzz-smoke` runs mutations on build-local copies
  of the committed vectors.
- Refuse archive entries whose parent components are symbolic links, and
  hard-link targets outside the extraction root or of a non-file kind.
  POSIX extraction resolves parents through directory descriptors with
  symlink following disabled and keeps the staging root private. A failed
  checksum can no longer leave files outside the extraction directory.
- Enforce the decoder's memory budget before buffering a segmented input.
  Inputs that cannot fit alongside a decoder, and pipes, use streaming
  decoding. The library counts retained input when it falls back to one
  worker as well as when it decodes in parallel.

### Fixed
- Restore nested folders under restrictive umasks, including `0777`. Keep
  temporary entries accessible to the owner, then apply recorded permissions
  or the saved umask for legacy archives. Publish read-only roots with their
  exact final modes restored through the retained directory handle.
- Add an SDK regression for the merged fast-profile conversion fix: fractional
  max configurations remain valid after conversion and encode/decode correctly.
- The folder walker's hard-link table is a hash table over (device,
  inode). It was a flat list scanned for every name of a shared inode,
  quadratic in the hard-linked inodes: 0.3 s over 30,000 and 6.3 s over
  100,000 on the reference host, so a snapshot tree of a million would
  have spent minutes scanning. 100,000 hard-linked pairs now walk in
  3.5 s, less than the same number of plain files.
- A regular-file output takes each block in one `write`. Every sink was
  chunked at `PIPE_BUF` with a cancellation check either side of each
  chunk — four system calls per 4 KiB, a seventh of the wall time of a
  stored-block decode (68,694 calls and 0.09 s of kernel time for 64 MiB,
  now 4,183 and 0.03 s). Pipes and terminals, which can block, are still
  chunked.

### Changed
- `--threads n` runs no more workers than the host has CPUs online and
  notes a request past them; the sizes stay fitted to `n`, so the bytes
  do not change with the host (RUN-5).
- The throughput floors are checked in CPU seconds (`cpu_seconds` in
  `tools/ratio_floor.json`, at the figures the wall floors had — CPU time
  is never more than wall time, so the floors are equal or stricter;
  `wall_seconds` floors stay valid and are what the corpus floor files
  still use). `tools/check_ratio_floor.py --runs N` encodes a fixture N
  times for the median, holds every run to the same bytes, and with
  `--baseline-binary` runs the two binaries turn and turn about, so a
  neighbour's load weighs on both. The gate had compared one wall-clock
  run against floors locked beside other jobs, a verdict one hiccup could
  turn; the re-lock runs showed what CPU time does and does not fix — no
  runqueue wait at load 3–4 on 12 hardware threads (CPU time equalled wall
  time run for run), but a 17 % slowdown from two other encodes sharing
  the cache, which only a quiet host or the interleaved relative gate
  answers. `make gate` runs the proxy three times; `make check` prints
  its wall time and the load average beside its budget.

### Added
- `make ltcb-release` (`tools/ltcb_release.sh`) builds the assets of a
  Large Text Compression Benchmark entry from the committed HEAD into
  `dist/`: the stripped Linux binary and its zip (the decompressor the
  ranking counts; the sources are not published), the fixtures' containers
  written and decoded byte for byte by that binary or taken over from an
  earlier run, `SHA256SUMS` and a manifest. `docs/ltcb-submission.md` has
  the rules, the 2026-09-15 release (enwik8 18,207,203 B, enwik9
  150,063,628 B, zip 209,893 B) and the email to send.
- The documents are checked against the code by `make check`
  (`tools/test_docs.py`): the README's usage block and the flags of the
  manual against `cadapt --help`, the container versions of `adapt.h`
  against the README and the manual, every relative link, the README's
  layout table against the tracked tree, and the locked floors, which
  `tools/render_floors.py` renders from `tools/ratio_floor.json` into a
  marked block of the README and the manual — the prose no longer restates
  a number a re-lock would leave behind. `make ltcb` builds the LTCB
  renderings `research/ltcb-tables.md` links; `research/README.md` indexes
  the dated notes and `research/current-baseline.md` is
  `baseline-2026-09-14.md`, marked as the record it is. The audit that
  found what these fix is `docs/doc-hygiene-report-2026-09-15.md`.
- The text transform, container versions 62 to 66 (67 to 71 with
  metadata), ahead of the normal, fast, max, compact and lean models: a
  word both sides have seen written out three times becomes, from then
  on, a code of one to three bytes at or above 0x80, so the model meets a
  word as one symbol and codes fewer bytes. Each side builds the same
  bounded book as it goes; nothing is sent ahead and no data is built in
  (FMT-2, FMT-8); other bytes pass unchanged, escaped above 0x7F, and the
  decoder undoes it as it writes. Taken by itself for a file of 8 MiB or
  more whose first block looks like text; `--transform text` asks for it
  (a pipe's length being unknown), `--transform none` refuses it; never
  with the adaptive predictors or the BMP transform. Measured on enwik8
  with the model unchanged: at 128 MiB (`--max --allocation context`)
  19,657,760 → 18,971,412 bytes in fewer seconds, the model coding 62 %
  of the bytes; the 8 MiB proxy 1,716,715 → 1,716,213 in 14 s instead
  of 18. Below 8 MiB it costs (64 KiB +3.2 %, 4 MiB +0.4 %), which is
  what the length rule is for. The proof alongside undoes the transform
  before checking the trailer and holds the inverse of every byte
  produced against the input; `cadapt info` names it; `adapt_sizes`
  gains `text`, `adapt_stats` `text_bytes` and `text_words`, and
  `ADAPT_ERROR_BAD_TEXT` names a malformed stream.
- Container versions 53 and 56 (with metadata): the max profile with
  compact side tables — the third and fourth first-layer arbiters keep
  2^15 weight sets instead of 2^16 and the second estimation stage 2^17
  rows instead of 2^18, 26.5 MiB less. `--allocation context` chooses them
  only when the budget would otherwise stop the context table at 2^22
  blocks or fewer, short of the input's target, and spends the difference
  on a table twice the size. Measured on enwik8: 20,712,600 → 20,258,968
  bytes at 94 MiB, 20,091,771 → 19,715,577 at 128 MiB (peak RSS 124 MB),
  19,597,276 → 19,284,703 at 192 MiB; at 256 MiB and the default 674 MiB
  the full tables are kept and every byte is unchanged, the locked floors
  included. Every earlier max container decodes as before, with its full
  tables. `adapt_sizes` gains `compact`.
- Container versions 54 and 57 (with metadata): the compact model with a
  lean context set, leaving out its three costliest hashed contexts
  (order-8, the third word back, the word with its classes). The context
  policy chooses it with the compact tables while the table stays at 2^22
  blocks or fewer, where those contexts evict more than they predict.
  Measured on enwik8: 20,258,968 → 20,150,130 bytes at 94 MiB and
  19,715,577 → 19,657,760 at 128 MiB (peak RSS 124 MB), with seventeen
  table claims a nibble instead of twenty; at 192 MiB the table is 2^23
  and the full set is kept (the lean one lost 0.05 % there). A version-53 file keeps every
  context; `cadapt info` names the compact tables and the lean set.
  `adapt_sizes` gains `lean`.
- Container versions 58 and 61 (with metadata): the max profile sized to
  its budget. One sizing byte follows the fixed header (and the period
  bytes): bit 0 the compact side tables, bit 1 the lean context set, bits
  2–4 the context table's fraction f, so a table may hold
  (8 + f) << (table_log − 3) blocks and a budget between two powers of
  two is spent on it rather than left idle — a hash then picks its line
  by a range multiply instead of a mask. `--allocation context` takes
  only the fraction the budget's slack holds after the window, never a
  smaller window, and writes the version only for a fraction above 0, so
  a power-of-two table keeps versions 21, 53 and 54 and every file they
  wrote decodes as written. Measured on enwik8 at 256 MiB: a 160 MiB
  table against the 128 MiB one, 19,185,877 → 19,066,780 bytes
  (−0.62 %); the sizer's own choice there, 9/8 (144 MiB), −0.23 % on the
  first 32 MiB, and at 512 MiB, 9/8 (288 MiB), 18,831,629 → 18,780,150
  (−0.27 %). The default budget reaches its target and takes none, so
  the locked floors are byte-identical. `adapt_sizes` gains `table_frac`;
  `adapt_table_blocks()` gives the block count; `cadapt info` prints it
  for a sized table.

### Added
- The mixed acceptance corpus of `DLV-3`: `tools/fetch_corpus.py` (`make
  corpus`) fetches the Silesia and Canterbury corpora into
  `static/corpus/mixed/` (git-ignored; the manifest pins every archive and
  file by SHA-256, and a fetch that differs fails), and `make corpus-floor`
  checks every file against its locked floor under the default command and
  under `--max --allocation context -6` (`tools/corpus_floor_default.json`,
  `tools/corpus_floor_max6.json`; `check_ratio_floor.py --corpus DIR`).
  Locked at 15d0cbe: 214,749,364 → 40,478,327 B under the default command.

- The sized max profile's sizing byte carries the text transform (bit 5),
  so a table fraction and the transform are taken together where before
  the transform dropped the fraction: enwik8 `--max --allocation context
  -6`, 19,306,669 → 19,252,902 B (−0.28 %), version 61, peak RSS 89 MB,
  decoded byte for byte. A text version (62–71) still names a
  power-of-two table; a sized header without the bit reads with the
  transform off.

### Changed
- The max profile under the text transform writes its sized version (58,
  61 with metadata) instead of 64–66 (69–71), which stay readable: bit 6
  of the sizing byte starts every arbiter's trust in a context's
  stretched probability at 1/32 rather than 1/4 (`adapt_sizes.low_start`,
  `ADAPT_SIZING_LOW_START`), which the encoder sets under the transform.
  Measured on the first 16 MiB of enwik8 under the transform: 3,336,047 →
  3,330,628 bytes at 128 MiB, 3,320,062 → 3,296,290 at the default tier;
  on enwik8 at the default tier 18,207,203 → 18,110,558. `cadapt info`
  prints the arbiters' start; bit 7 of the sizing byte stays reserved and
  refused.
- `make check` is held to 120 s of wall time: it runs under `timeout`
  (`CHECK_SECONDS`, root Makefile) and fails when exceeded, with the
  codec's check and the tools' tests at once. The coverage pass runs every suite at
  once under `make -j`, each process writing its gcov counts under its own
  `GCOV_PREFIX` and `gcov-tool` merging them (the instrumented binary is
  5–9 times slower per byte); the two large C suites take a shard `k/n`
  and the CLI suite runs class by class; `make test` runs the C suites
  the same way. The coverage build is `-O1` (same bytes, 1.6× faster).
  The CLI suite's megabyte cases — a 4 MiB named input's sizes, the 8 MiB
  text policy, the tiers above xz on a pipe — run only under
  `ADAPT_LONG_TESTS=1` (`make check-long`); the structural C tests
  (segments, threads, streams, the proof alongside, the transforms across
  segments) use the fast profile, since the model is not what they test.
  Measured on a host with load average 10: `make coverage` 830 s → 115 s
  with identical branch counts (library 82.5 %, CLI 72.3 %), `make test`
  74 s → 21 s; `make check` end to end 105 s. `make gate`, the CI
  proxy, is unchanged at about 25 s.

### Changed
- The memory tier is encode's only memory setting. `-10` and `-11` are new,
  1 and 2 GiB, the tiers above xz's ladder that the max profile's largest
  tables and a 1 GB input's window need; `-9` stays the default and the
  last tier given wins. `encode --max-memory` is refused with a message
  naming the presets: the tier a file is written at is the memory every
  decoder of it needs (`RUN-1`), and a byte count named a model no tier
  describes. A tier below the profile's smallest model is raised to it,
  never refused. `decode --max-memory` is unchanged, the cap that refuses a
  container with the bytes it needs. Historical scripts under `bench/`
  that passed `--max-memory` to encode are records, not gates.
- `decode --max-output` applies only when given: a valid container decodes
  whatever it declares (`FMT-6`), and a service that decodes untrusted
  containers sets the cap itself. The library's `adapt_default_limits()`
  keeps its conservative 1 GiB for API callers; the command line applies
  neither cap unless asked.
- `docs/CONSTRAINTS.md` gains the runtime codes `RUN-1`–`RUN-5` (the tier
  as a decode promise, streaming, nothing half-published, refusals with
  their numbers, the same bytes on every machine), `DLV-1` (every
  published version decodes forever), `DLV-3` (general-purpose first:
  enwik8/enwik9 are benchmark rows, never the judge of a default) and
  `DLV-4` (provenance); `FMT-6` names both caps, and `FMT-8` allows a
  transform or dictionary the encoder builds from the input itself and
  carries in the container. The README's command line, build switches and
  format section follow, with a map of every container version.

### Changed
- The per-bit path of the normal and max profiles is faster and writes the
  same bytes: on the x86-64-v3 instantiation the twenty-one memories are
  read, learned and advanced as vectors (the slots' lanes in planes), the
  four first-layer arbiters are one pass over the inputs with an explicit
  sign-extending multiply, the block claims test their four tags without a
  branch per way, the third lane is derived from a per-state mode, and the
  lines the next nibble's and the next byte's claims will take, the next
  bit's arbiter sets and estimation rows are prefetched — exactly, from the
  byte the encoder is coding, over the bits before they are needed. On
  enwik8 (100 MB, `--max --allocation context`) the container is byte for
  byte the one before; the measurements are in
  `docs/performance-audit-2026-09-14.md`. The x86-64-v3 pair is now chosen
  once per block from a CPU test rather than through an ifunc, and
  `ADAPT_ISA=baseline` in the environment selects the portable pair, which
  the CLI tests use to hold the two to the same bytes.

### Changed
- The recall index grows to 2^22 entries from whatever budget the table and
  the window leave once both have all they can use (its cap was 2^20):
  enwik8 under `--max --allocation context` at the default budget,
  18,564,510 to 18,529,513 bytes; at 128 MiB nothing is left and the
  container is unchanged. The size floors are re-locked in `LOCKS.md`.

### Added
- Segments on threads: `encode --threads <n>` codes a segmented file's
  segments at once, on as many of the `n` threads as the memory budget
  holds a model, a segment and a block's workspace for, and writes them in
  order; `decode --threads <n>` reads a file's segment table and decodes
  the segments the same way. The bytes are unchanged (each segment's model
  starts afresh either way). With `--verify` every thread proves its own
  segment before it is written. The library gains
  `adapt_encode_stream_threads()`, `adapt_decode_threads()`, the worker
  counts and memory bounds behind them, and `adapt_stats.workers`. On the
  8 MiB enwik8 proxy under `--max`, two 4 MiB segments encode in 32.0 s on
  two threads against 66.1 s in order, and four 2 MiB segments in 24.8 s
  on four threads against 67.6 s.

### Changed
- Speed, with the output bytes unchanged: the model prefetches both
  possible next bytes' table lines, run slots and recall slot at the
  seventh bit, and the next bit's arbiter and estimation rows after every
  bit (the 8 MiB enwik8 proxy under `--max`: 63.1 s to 56.7 s on the
  reference container); the arbiters' update counts moved out of their
  weight rows so every row is vector-aligned (56.7 s to 54.5 s). The
  memory a model takes is unchanged.
- Rejected after measurement, for the record: constant-count
  instantiations of the per-bit path (no gain, a 47 % larger binary);
  the chooser's trials on threads (within noise); 16-bit arbiter
  arithmetic (17 % faster, but the proxy container grew 8.2 %, past the
  locked floor).

### Fixed
- Two CLI tests compared whole containers of the same named input across
  two runs; the metadata header carries the access time, which the first
  run's read may move. They compare the streams under it.
- Folders: a directory is coded as one container and restored byte for
  byte — every file, directory and symbolic link in its place — with
  permission bits and access/modification timestamps (nanoseconds on POSIX)
  for named files and folders, verified after application. Container
  versions 16, 17 and 18 carry the metadata; 13–15 stay readable.
- `FMT-7` in full: hard links restored as hard links, sparse files with
  their holes kept, extended attributes (`user.*`) and POSIX ACLs packed
  and restored with read-back verification, a directory's applied after
  its children. Archive version 3 (`ADPTARC3`); versions 1 and 2 decode.
  The portability rules are in the README.
- `FMT-8` measured: six corpus rows of about 2 MiB of Wikipedia text —
  English, Russian, Chinese, Japanese, Vietnamese, Czech — fetched
  reproducibly by `bench/fetch_scripts.py`, benchmarked in
  `research/scripts-measurement.md`, with Cyrillic and CJK test vectors.
- Memory presets `-0` … `-9` after xz's, `-9` (674 MiB) the default for
  encoding.
- `make coverage` (the branch-coverage gate, 80 % library / 60 % CLI),
  `make sanitize` (ASan, UBSan, LeakSanitizer), larger committed vectors
  pinning the class, context-set, period and segment choices, and CLI
  tests for every command's exact messages.
- Delivery (`DLV-2`): the MIT licence, an SPDX SBOM and a build manifest
  with `make reproduce`, this changelog, `SECURITY.md` and `SUPPORT.md`.
- A root README; `docs/CONSTRAINTS.md` states what the codec is held to.

### Changed
- `decode` allocates the model the container's header names; `--max-memory`
  is the only cap that can refuse a valid container (`FMT-6`).
- `encode` skips the round-trip proof unless `--verify` is given;
  `--no-verify` is still accepted.
- Staged outputs are published at the mode the umask leaves (0666 less the
  umask), not the staging file's private 0600.

### Fixed
- The in-memory readers no longer offset a NULL source for an empty input
  (undefined behaviour reported by UBSan).
- A test buffer leak in `test_x86_transform`.

## [0.11.0] — 2026-09-14

Container format versions 13 (normal), 14 (fast) and 15 (max): an x86
branch transform before the model for machine code, four blocks to a table
line instead of two, a context table twice the size for the same input, and
a max profile that adds contexts, run memories and per-context evidence.
Versions 10–12 are refused. Encoding telemetry (`bench/`) and the enwik8
corpus fixture.

## [0.10.0] — 2026-09-12 (untagged)

The C codec as a standalone repository: the port of the Rust crate's
container format (versions 10–12) and predictor, the `cadapt` command line,
the C test suites on committed vectors, and the gcov coverage gate.

Only `v0.11.0` is tagged so far; the git history under `git log` is the
record between versions.
