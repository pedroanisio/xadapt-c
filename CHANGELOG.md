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

### Security

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

### Added
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
