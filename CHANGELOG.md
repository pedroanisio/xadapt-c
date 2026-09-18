# Changelog

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). Seeded from the commit history. -->

All notable changes to the codec, in the shape of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The version a binary reports
(`cadapt --version`) and the container format versions it reads and writes
are both named per release. Security fixes are listed under **Security**
and announced as [SECURITY.md](SECURITY.md) describes.

## [Unreleased]

Nothing yet.

## [0.12.0] — 2026-09-18

Container format versions 19 to 85, one live version per profile and
variant — the normal profile 84 (85 for a named file or folder; 19 and 22
when the classic arbiter keys win the first block), the fast profile 20
(23), the mid profile 76 to 79, the max profile 72 to 83 — every one of
them read forever from here on, each pinned by a vector. Against 0.11.0
under the same default commands on the mixed corpus (the release notes
give the rows): 4.2 % fewer bytes over the corpus under the default
command and 6.7 % under `--max`, fewer on every Silesia row, 27 to 96
bytes more on the five smallest Canterbury files. enwik8 under the
default command 18,249,169 B in 68
CPU s; enwik9 149,702,998 B in 683 s; the locked baseline
`--max --allocation context --contexts lean` 17,800,141 B (107.4 s) and
145,784,134 B (1,054 s), on the reference host. Versions 13 to 15, what
0.11.0 wrote, are refused (LOCKS.md, 2026-09-14): a file of 0.11.0's is
decoded with 0.11.0 before the upgrade, or with the two installed side
by side (`docs/release-notes/0.12.0.md`, "Upgrading"). The command line's
`--version` line changed; `adapt_stats` grew; the C interface is
otherwise source-compatible with 0.11.0's declarations it kept.

### Added

- The release pipeline (`docs/release-pipeline.md`). Two workflows:
  `ci.yml` on every push and pull request — builds and the full suites
  on Linux (GCC, Clang) and macOS (Clang) with the coverage gate, the
  examples and the provenance and documentation tests; the sanitizers on
  Clang, the CLI suite under them on Linux; identical bytes across GCC,
  Clang and the AVX2 and baseline paths; the corpus and proxy size floors,
  the memory promise and the throughput of the change against its base
  commit in the same run on a hosted runner; the absolute CPU-second
  floors on a self-hosted runner labelled `reference` (the host the floors
  were locked on), skipped where none exists; the install flow from a
  clean environment on Linux and macOS and inside a fresh Debian
  container; a Windows MinGW build that gates nothing. `release.yml` on a
  tag `v<version>` — the same gates, then the artifacts on both platforms,
  each binary tarball installed from its layout and used, the build
  reproduced from its manifest, and a GitHub release with one
  `SHA256SUMS` and the CHANGELOG entry as its notes.
- `make install` / `make uninstall` (`PREFIX`, `DESTDIR`): `cadapt`,
  `cadapt.1`, `libadapt.a`, `adapt.h`. `make release` (`tools/release.sh`):
  the artifacts of a committed revision from an export of it — the binary
  tarball in the install layout with `LICENSE`, a `NOTICE` generated from
  the SBOM, `CHANGELOG.md`, `sbom.spdx.json` and `build-manifest.json`;
  the source tarball; `SHA256SUMS`; `release-manifest.json` — the C suites
  run on the export first and the packaged binary made to round-trip a
  file after; a tag `v<version>` must name a revision whose `cadapt.c`
  says `<version>`. `make test-install` (`tools/test_install.sh`): an
  export built with `cc` and `make` alone, installed under a prefix,
  used (a file and a folder round trip, the manual, a program compiled
  against the installed header and library), uninstalled clean; the same
  for a release tarball (`--tarball`) or inside a container (`--docker
  IMAGE`, `INSTALL_IMAGE=`).
- `make memory-gate` (`tools/check_memory.py`, with its tests in the
  `floor-tool` part of `make check`): the peak resident set of an encode
  and of a decode, as the kernel accounts it to the child, against what
  the container declares decoding needs plus 16 MiB for the process —
  `RUN-1` and `RUN-2` as a gate that holds on any host. On the proxy every
  measurement is 4.7–7.8 MB under its declaration.
- The interface's contract, stated and executable. `adapt.h` opens with
  what every declaration is held to — ownership (a caller's buffers stay
  the caller's; `*out` is malloc'd and released with `adapt_free()`, and
  left NULL with `*out_len` 0 on any failure), lifetimes (a callback's
  pointers live for that call), threads (no global state; callbacks only
  on the calling thread, never on a worker, never two at once), errors
  (`adapt_error` written whole on every return, zero on success; a NULL
  required pointer is `ADAPT_ERROR_INVALID_ARGUMENT`; `data` may be NULL
  only with length 0) and limits (the ranges `adapt_sizes_valid()`
  tests, the segment and worker bounds, the archive maxima). Four
  programs in `adpat/c/examples/` — `roundtrip.c`, `stream.c`,
  `folder.c`, `contract.c` — are built against the header and the library
  alone and run by `make check`, each exiting 1 where the interface
  departs from the header. `SUPPORT.md` gains a "Compatibility" section
  stating what a release promises the next: containers and archives
  forever, the C interface source-stable within a major release with
  caller-allocated structs allowed to grow, the command line's commands,
  options, exit codes and report line stable, an option never removed or
  given another meaning within a major release.
- Every published container version has a pinned vector
  (`tests/test_adapt_decode.c`, `test_published_versions`): 17 new files
  for the adaptive predictors (25–27, 39–41), the BMP transform (31–34,
  45–48, with `gradient.bmp` as their source), the text transform over
  the fast and mid profiles (63, 77) and, written by the encoder of
  `e72f3e8` that carried them, over the max profile (64) and its compact
  lean form (66); the 23 metadata forms are derived from their stream's
  vector through `adapt_header_add_metadata()` and must decode to the same
  bytes and read the record back. Versions 65 and 70 have no vector:
  that encoder refused the shape ("invalid header field") at every
  budget that would have chosen it, and `a0732c1` retired the family the
  same day — no file of theirs was ever written, and the test says so.
- Two libFuzzer harnesses beside `fuzz_decode`: `fuzz_header` (the same
  oracle on bytes whose header checksum is made good first, so the fields
  a CRC-32 shielded from mutation are reached) and `fuzz_archive` (the
  archive unpacker in short pieces into a sink that holds it to its
  contract), with `archive_seeds` writing well-formed archives of every
  version as their corpus; `make fuzz-header-smoke`,
  `make fuzz-archive-smoke`, and `FUZZ_MODEL_MEMORY` to cap the decoder
  harnesses' model.
- `cadapt 0.12.0 --max --allocation context` is listed on the Large Text
  Compression Benchmark (mattmahoney.net/dc/text.html, 2026-09-15): enwik9
  150,063,628 B plus a 209,893 B decompressor, total 150,273,521 B, rank
  30 of 226, 661 MB, note 114. The enwik9 goal is now the canonical
  top-20 figure of that table (operator, 2026-09-16; `DLV-3`, LOCKS.md):
  place 20's total 125,099,359 B less the 209,893 B decompressor less one
  byte — **enwik9 at most 124,889,465 B** — computed from the snapshot by
  `tools/ltcb/ltcb_goal.py`, which also states the memory claim (661 MB,
  the least of every entry ranked above; the least above is 1,542 MB);
  `tools/test_docs.py` holds the README and `docs/CONSTRAINTS.md` to the
  figures it prints. `tools/ltcb/transcribe_ltcb_table.py` transcribes
  the page's main table into `research/ltcb-main-table.csv` under the
  rules `research/ltcb-tables.md` records, checked by reproducing the 224
  hand-transcribed rows of the previous snapshot; the snapshot is the page
  of 2026-09-15 (226 rows).
- `cadapt encode --explain`: three lines after the report naming what the
  first block decided — the tables and allocation (window-first,
  table-first, or auto's choice), the class width, context set, record
  period and arbiter keys, the sample codings that chose them and their
  bytes, and whether the text transform is on. `adapt_stats` carries the
  same (`class_code`, `set_code`, `keys`, `lean`, `period`, `trials`,
  `trial_bytes`) for library callers. The container is unchanged.
- The normal profile's arbiters keyed the same way (version 84, 85 with
  metadata; `ADAPT_NORMAL_KEYS_VERSION`): the model of version 19 with
  the key policies of 80 and 81 on its second, third and fourth
  arbiters, chosen per file on the first block as under `--max`, the
  classic keys keeping 19, 22, 62 and 67. The normal profile has no
  sizing byte and its text transform is a version family of its own, so
  84 carries one policy byte after the fixed header — the key policy in
  bits 0 and 1, the text transform in bit 2. Under the default command,
  same time and memory: the 8 MiB proxy 1,726,147 -> 1,699,643 B
  (-1.54 %), enwik8 18,500,473 -> 18,249,169 (-1.36 %), enwik9
  168,451,193 -> 165,865,476 (-1.53 %), the mixed corpus 40,421,105 ->
  40,047,647 (-0.92 %; 20 rows smaller, 3 equal, none larger), whose
  floors are lowered; the max, fast and mid streams are unchanged. `cadapt info` prints the
  policy for normal files too (`arbiter keys    classic` for a
  version-19 file).
- The max profile's arbiters keyed by the byte classes (`adapt_sizes.keys`,
  versions 80/81, 82/83 with metadata; `ADAPT_MAX_KEYS_VERSION`): the
  second, third and fourth arbiters' weight sets are chosen by other state
  than before — the third by the classes of the last five bytes under both
  new policies; under the text policy the second by the last two and two
  thirds classes and the fourth by the nesting state with the last tag
  and two classes, under the binary policy the fourth by recall's expected
  byte and state. The encoder tries both against the classic keys on the
  first block's sample (`choose_keys`) and keeps version 72 where the
  classic keys win, so no file is written larger than before; every
  earlier version decodes as written. `cadapt info` prints the choice as
  `arbiter keys`. Same tables, memory and cycles: on the 8 MiB proxy of
  the locked baseline 1,681,725 -> 1,669,716 B (-0.71 %); the enwik8,
  enwik9 and mixed-corpus figures are in LOCKS.md. The idea is the
  mixer-selection context of the PAQ family (Mahoney, *Data Compression
  Explained*), the keys this codec's own (DLV-4).
- `--mid` on encode (`adapt_sizes.mid`, `adapt_sizes_mid()`): a third
  profile between `--fast` and the default, versions 76/77 (plain, under
  the text transform) and 78/79 with metadata. It runs the normal
  profile's tables with seven fixed contexts — orders 2, 3, 4 and 6,
  word, record and the class history; order-1, field and sparse left
  out — and the first arbiter alone as the verdict, the two estimation
  stages kept. The shape is the one Mahoney's *Data Compression
  Explained* gives for the lpaq family, re-implemented here (DLV-4).
  Against the default: enwik8 18,761,302 B (+1.4 %) in 29 % less
  encode time and 24 % less decode time, enwik9 172,472,731 B (+2.4 %)
  in 28 % and 26 % less; the 8 MiB proxy +1.3 % at 37 % / 25 % fewer
  cycles. An opt-in profile: no default and no floor moves. `cadapt
  info` prints "profile mid". Not with `--fast`, `--max`,
  `--adaptive`, `--transform bmp` or `--contexts lean`. The enwik8 and
  enwik9 measurements are in LOCKS.md.
- `--contexts auto|lean` on encode (`adapt_sizes.contexts`,
  `ADAPT_CONTEXTS_LEAN`): `lean` takes the lean context set outright —
  word and field left out and, under `--max`, o8, word3 and wordc (the
  sizing byte's lean bit, which the format now allows with the full side
  tables; a reader older than this release refuses such a file) —
  instead of choosing the set from the first block, trading bytes for
  time on text; `auto`, the default, writes the same bytes as before.
  `cadapt info` prints "lean contexts" for it. Not with `--fast`. The
  measurements on enwik8 and enwik9 are in LOCKS.md.
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

- **Breaking, with migration:** `cadapt --version` prints
  `cadapt 0.12.0 (format versions 19 to 85)` — the span of container
  versions the decoder reads, from the version table — where it printed
  `(format version 19)`, the normal profile's stream version, which
  stopped describing what the encoder writes when the keyed normal
  profile (84/85) landed. A script that parsed the parenthetical should
  take the release from the second word (`cadapt <release>`); the
  parenthetical is informational and may change again. `-V` is the same.
  `--help` and `-h` are in the manual.
- `adapt.h`'s first line names every version the table holds (19 to 85),
  not "19 through 75".
- `adapt_stats` gained `class_code`, `set_code`, `keys`, `lean`, `period`,
  `trials` and `trial_bytes` earlier in this release (`--explain`): a
  source-compatible change that grows a caller-allocated struct — code
  compiled against the previous header is rebuilt, as the compatibility
  policy now states.
- `--allocation auto`, the new default, grows the context table before the
  recall window only where the window-first sizes would leave the table
  below its target for the input — an input longer than the window the
  budget allows, whose table then stayed at its floor. Under the normal
  profile at `-9`, enwik9 was written with a 2^20-block table beneath a
  512 MiB window: 165,865,476 bytes; with the table first 149,702,998
  (-9.74 %) in less time (682 against 706 CPU s) at 667 MiB. Every input
  the two policies size alike is byte for byte unchanged: the 8 MiB proxy
  under all five commands, enwik8, and all 46 mixed-corpus rows under both
  gate commands; on the corpus at `-6`, where they differ, the table wins
  mozilla -1.30 %, webster -1.81 % and samba -0.70 % and loses 56 bytes on
  nci. `--allocation window` keeps the old sizes. At the max profile's
  floor budget the rule takes the compact side tables and a 2^18 table
  (32 MiB) instead of the full ones and a 2^10 table (46 MiB); the help
  now names the floors as the budgets `-0` is raised to. (LOCKS.md.)
  Reviewed together with the batch's other changes (2026-09-17, the first
  16 MiB of enwik8, sao, a folder, a BMP and a pipe, against the binary
  before the batch): every command sizes and codes alike except where the
  rule fires — `--fast -0` 4,406,827 → 4,245,747 bytes, `--max -1`
  3,882,929 → 3,418,737, `--max -6` 3,359,601 → 3,313,077, sao `--max -6`
  4,346,612 → 4,331,736 (the gate row) — at the same peak memory (87 MiB of
  the 94 MiB tier at -6) and the time of `--allocation context`; peak
  memory, CPU time, round trips and decoding by either binary are
  unchanged everywhere else, `--threads 2/4`, `--verify`, `--adaptive`,
  `--transform`, folders and pipes included; enwik8 under the default
  command is 18,249,169 bytes either way (window-first: the 128 MiB window
  holds it). On every Silesia row at `--max -6` the rule fires and wins
  ten of twelve, −1.13 % over them for 2.6 % more time, losing nci
  (+2.40 %) and reymont (+0.78 %). Not the table's doing: with the second
  max model's pressure-gated parts held off, 2^21 blocks beat 2^20 on
  both (reymont 907,794 → 893,039, nci 1,110,376 → 1,103,506); it is
  those parts, switched on by a table of 2^21 blocks or more
  (`MODEL2_PRESSURE_LOG`), that cost reymont 2.3 % and nci 0.4 % at the
  `-6` shape (mozilla +0.38 %, ooffice +0.55 %, sao +0.19 %, samba
  +0.14 % with them; the other rows within 0.02 %), and on nci the
  compact side tables with the lean set, 2.6 % against the full tables
  at 2^21. A first-block trial cannot choose the parts — untrained, they
  lose on every file's first block and 32 KiB sample (reymont, nci and
  dickens alike) and pay only across a long file — so the gate stays a
  size. `--allocation window` keeps the old sizes.
- The width trials of a given context set (`--contexts lean`) run on that
  set instead of the full one; the width verdict is unchanged on every
  locked fixture and the lean encode runs one trial fewer.
- The floors re-locked on the keyed-arbiters binary (LOCKS.md,
  2026-09-16): enwik8-8MiB 1,669,716 B, enwik8-16MiB 3,236,535 B, enwik8
  17,800,141 B, enwik9 145,784,134 B in 1,054.0 CPU s (the enwik8 rows'
  throughput floors stand: equal in the interleaved comparison); the
  `--max -6` mixed-corpus floors lowered on 19 of 23 rows (39,694,991 ->
  39,458,213 B in total), the default command's unchanged.
- The locked baseline: `tools/ratio_floor.json` names `encode --max
  --allocation context --contexts lean` and its floors are re-locked on
  that command (enwik8 17,902,645 B in 117.1 CPU s, enwik9 146,797,785 B;
  LOCKS.md). The default command and the mixed corpus floors are
  unchanged.
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
- The recall index grows to 2^22 entries from whatever budget the table and
  the window leave once both have all they can use (its cap was 2^20):
  enwik8 under `--max --allocation context` at the default budget,
  18,564,510 to 18,529,513 bytes; at 128 MiB nothing is left and the
  container is unchanged. The size floors are re-locked in `LOCKS.md`.
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
- `decode` allocates the model the container's header names; `--max-memory`
  is the only cap that can refuse a valid container (`FMT-6`).
- `encode` skips the round-trip proof unless `--verify` is given;
  `--no-verify` is still accepted.
- Staged outputs are published at the mode the umask leaves (0666 less the
  umask), not the staging file's private 0600.

### Fixed

- `tools/ltcb_release.sh` exported a fixed list of sources that stopped
  at the split of `adapt_model.h` and `cadapt.c`: its build failed with
  "No rule to make target 'adapt_constants.h'". It exports the codec
  directory whole, as `tools/release.sh` does.
- The memory bounds saturate. `adapt_threads_encode_memory_bound`,
  `adapt_parallel_encode_memory_bound` and
  `adapt_parallel_decode_memory_bound` multiplied a per-worker cost by the
  caller's thread count in plain `uint64_t` arithmetic, so a count that
  wrapped the product read as *less* memory than any budget: `cadapt
  encode --threads 9223372036854775808` was admitted ("the sizes are
  fitted to 9223372036854775808 models as asked") and fitted the file to a
  share that had underflowed — a recall index and window larger than
  `--threads 12` gives at the same tier — while 2^64−1 and 2^32+1 were
  refused. Every count that wraps is now refused as the budget's, and the
  C suites hold the three bounds to `UINT64_MAX` on such counts
  (`ThreadCountTest` in the CLI suite).
- An i/o error the output raised — a full disk, a closed pipe — was
  reported against the input's path; it names the output now (`standard
  output` for `-`).
- `attrs_free` dereferenced a NULL array when `attrs_read` failed to
  allocate the arrays after it had counted the names (gcc's analyzer); it
  tolerates the arrays a failed allocation left NULL. `read_packer` refuses
  a header stage without an entry instead of dereferencing it (an
  invariant of `packer_next`, made explicit), and the decoder refuses a
  version-table row whose stream is not a row (an invariant the table's
  test holds).
- The audit that found them: gcc `-fanalyzer` and clang `--analyze` over
  the six units; gcc with `-Wshadow -Wconversion -Wsign-conversion
  -Wnull-dereference -Wformat=2 -Wcast-qual …` (the rest triaged benign:
  bounded `int16` arithmetic, `int`→`size_t` lane counts, `ENOTSUP ==
  EOPNOTSUPP`); the C suites, the CLI suite (66 tests, leak detection on)
  and the safety suite under AddressSanitizer and UndefinedBehaviorSanitizer
  — the sanitizer build had never produced the `cadapt` binary, so the
  CLI suite had never run under them; the C suites under
  MemorySanitizer and ThreadSanitizer; libFuzzer on the two decoders
  (15 min × 4 workers at 128 MiB, 10 min × 3 at 64 MiB), on the decoders
  with the header checksum made good (10 min × 3) and on the archive
  unpacker (10 min × 3, ~15 M executions). No memory error, no undefined
  behaviour, no data race and no crash was found; the four safety-suite
  failures under ASan are its shadow reservation refused by the tests'
  own `RLIMIT_AS`.

- One version table, `ADAPT_VERSIONS` in `adapt.h`: every container version
  the decoder reads is a row naming its stream form, whether it carries the
  metadata record, the profile, the transform, the side tables and set, the
  experimental predictor or BMP mode, the trailing sizing or policy byte and
  the arbiters' keys. The decoder's dispatch, `adapt_header_has_metadata`,
  `adapt_header_len` and `adapt_header_add_metadata` read it (the two
  hand-written metadata-offset chains are gone), the C suite checks every
  declared version is a row and every row reads as the model it names, and
  the CLI suite derives its version knowledge from the table instead of
  keeping lists that had drifted twice. Bytes and behaviour identical.
- `adapt_model.h` is split into ten sections it includes in order —
  `adapt_constants.h`, `adapt_states.h`, `adapt_coder.h`, `adapt_tables.h`,
  `adapt_model_state.h`, `adapt_model_byte.h`, `adapt_model_bit.h`,
  `adapt_model_new.h`, `adapt_blocks.h`, `adapt_crc32.h` — each a guarded
  header of one concern, moved out textually so a reader finds the per-bit
  path or the coder without the 3,100-line file, and a change to the
  arbiters no longer sits in the same file as CRC32. The translation unit is
  the same: every function stays `static inline`, the profile
  specialization at compile time is unchanged, and the compiled bytes on
  every command and corpus row are identical. The coverage gate counts the
  sections; the Makefile's `MODEL_H` lists them as one dependency.
- `cadapt.c` is split the same way, along the boundaries it already drew:
  the folder side — a directory walked into an archive stream (`folder`,
  `packer`), an archive stream restored into a staged directory
  (`restorer`), and the staged directory — is `cadapt_folder.c` with its
  types and the functions the commands call in `cadapt_folder.h`; the
  metadata calls (a path's modes and times read, applied and checked,
  inherited ACLs cleared) join the filesystem layer `cadapt_fs.c`.
  `cadapt.c` keeps the commands, their arguments, streams and staged file:
  1,970 lines from 3,890. The code moved unchanged (every line of the
  original is in the new files; only prototypes, guards and header comments
  were written), the functions that cross a unit lost their `static`, and
  the coverage gate, the platform-header guard and the provenance tests
  name the new unit. Bytes and behaviour identical.
- `choose_model` returns one `model_choice` — width, set, keys and period
  from a single exit, the whole-block set proof shared by the fast and
  normal set choices (`prove_set`); `adapt_sizes` floors use designated
  initializers. Bytes identical.
- The folder side's refusals are tested: an entry the walker lists but
  the packer cannot open (a file or a directory without permission)
  refuses the folder by name and leaves nothing behind; a file that holds
  fewer or more bytes than it was listed with refuses it as changed while
  it was being coded (the encoder held inside its first entry through a
  pipe nobody reads, so the change is certain to land between the listing
  and the read); a destination whose parent refuses the staging directory
  refuses the restore by the target's name.
- `make check` builds the codec once before its parallel parts: the
  provenance part ran `make all` in the same build directory as the codec
  part, and two makes relinking `cadapt` at once left the safety suite
  executing a half-written binary (`Permission denied`) whenever the
  sources had changed since the last build.
- Restore Linux access ACLs after other attributes, preserving read-only ACLs
  together with user attributes on regular files, sparse files, directories
  and folder roots.
- Decode valid empty segmented containers with either thread count while
  preserving malformed-table, checksum and memory-budget rejection.
- Give pressure-gated model2 its own container versions 72/75. Preserve both
  historical variants written as 58/61: ambiguous small-table files are
  checked with one predictor at a time using temporary disk, and only
  verified bytes reach the output callback. Immutable historical fixtures
  cover memory, streaming and threaded APIs, text, metadata and segments.
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
- The in-memory readers no longer offset a NULL source for an empty input
  (undefined behaviour reported by UBSan).
- A test buffer leak in `test_x86_transform`.

### Performance

- The first block's width trials stop after the first when the sample
  codes below `CLASS_EVIDENCE` — that sample declines the class context
  whatever the other two widths would say, so they were two wasted
  encodes of the sample (bytes identical; a 4 KB input, whose encode is
  mostly its trials, 0.03 -> 0.02 s under the default command with the
  key trials included).
- A model's arbiter weights are initialized from one row copied per set
  instead of a lane-by-lane call for each of the 170,000 sets (bytes
  identical): 45 % of a small file's encode, whose first-block trials
  each build a model — the 4 KB corpus rows encode in 0.05 s instead of
  0.08, alice29.txt in 1.33 s instead of 1.4.
- The vector lane and learn paths of the max profile run only the slot
  groups a file's context set fills, and no longer zero the unused
  lanes bit by bit (a `rep stos` per bit): bytes identical, the lean
  baseline 8–9 % faster on encode and 5 % on decode (enwik8 117.1 →
  107.4 CPU s, enwik9 1,156.4 → 1,056.2 s, the 8 MiB proxy 11.7 → 10.7 s);
  the floors are re-locked on the measured times.
- The class-history width is proved on the whole first block when the
  32 KiB sample cannot rank it (an input of 4 MiB and up whose sample
  puts a width within 1/256 of none): the block's verdict is the file's
  on every corpus row, and the eight-byte width the sample kept missing
  is worth 0.1–0.35 % on text and binaries alike. The recall index grows
  to the format's 2^24 entries under `--max` when the tier has room. On
  the first 8 MiB of enwik8 1,686,110 → 1,680,015 bytes (−0.36 %), on
  the first 32 MiB −0.44 %; encoder policies only, no format change.
  Development builds (`-DADAPT_SIZES_OVERRIDE`) read `ADAPT_SIZES`,
  `ADAPT_CLASS_CODE` and `ADAPT_DROP_MASK` to code an input under a
  named model shape: the pressure proxy for the `-7` objective
  (LOCKS.md).
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
  The three parts that pay under table pressure — o8's and word3's
  slots, the fifth arbiter — run from a table of 2^21 blocks, the size
  the header names, after the mixed-corpus gate found them costing five
  small Canterbury rows at `-6` (kennedy.xls +3.7 %); gated, every row is
  under its floor (kennedy.xls −5.8 %) and nothing at 2^22 or above
  changes a byte. The floors re-locked on this binary on a quiet host,
  size and CPU seconds both tighter on every row: enwik8-8MiB 1,686,110 B in 12.2 s,
  enwik8-16MiB 3,266,940 B in 22.2 s, enwik8 17,978,253 B in 118.2 s,
  enwik9 147,896,517 B in 1,159.2 s, every round trip verified.

### Security

- Preserve existing targets when forced folder publication fails. Use atomic
  rename or exchange instead of deleting the destination first; refuse
  unsupported replacement and retain previous entries if cleanup fails.
- Put POSIX file outputs inside private staging wrappers and retain their
  file and directory handles through metadata, publication and cleanup.
  Replacing a staging name cannot publish an unrelated file, change its
  metadata or remove the replacement during rollback.
- Journal deferred directory and root metadata to temporary disk instead
  of retaining every attribute in RAM. Replay after checksum validation
  keeps memory bounded, preserves ACL/umask ordering and fails closed on
  temporary-storage errors.


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

Tags: `v0.11.0`, `v0.12.0`; the git history under `git log` is the
record between versions.
