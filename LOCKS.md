# Locked decisions

Decisions that are settled. A change to any of them is a new entry, not an edit.

## 2026-09-15 — Size and throughput floors re-locked on the merged text-transform binary

**Decision.** The floors of `tools/ratio_floor.json` move to what the
merged binary (a1ed995: the text transform, the recall index grown from
spare budget, the memory tiers) measures with the locked command
`encode --max --allocation context`, `tools/check_ratio_floor.py
--verify`, on the reference host (AMD Ryzen 5 5600X, load average about
3), every round trip verified byte for byte:

| Fixture | Size floor | Time floor | Was |
|---|---:|---:|---|
| enwik8-8MiB | 1,713,845 B | 13.3 s | 1,715,521 B, 17.9 s |
| enwik8-16MiB | 3,320,094 B | 24.4 s | 3,337,011 B, 33.8 s |
| enwik8 | 18,207,203 B | 130.5 s | 18,529,513 B, 189.4 s |
| enwik9 | 150,063,628 B | 1,385.4 s | 155,906,798 B, 3,123 s |

**Why.** The operator's rule: the floors are only ever improved from. The
transform made every fixture smaller and faster at once (the model codes
about 62 % of the bytes), so both axes move; a time floor left at the
old rate would let the transform's speed be given back unnoticed. The
10 % tolerance stays: wall time on a shared host is noisy.

**Consequences.**
- `make gate` guards 1,713,845 B and at least 567,650 B/s on the 8 MiB
  proxy (630,722 B/s less the tolerance).
- A change that turns the transform off for the proxy — a stricter length
  rule, a stricter detector — now fails the gate unless it pays otherwise.

**Status.** Locked 2026-09-15, enwik8 first (c279849) and enwik9 after
its measured round trip (decoded byte for byte in 1,748 s); the operator
asked for this lock ahead of every other session's work.

## 2026-09-15 — The memory tier is encode's only memory setting; `-10`/`-11`; decode's caps apply only when given

**Decision.** (Operator's, 2026-09-15.) `encode` chooses its memory by tier
alone: `-0` … `-9` are xz's presets as before, `-10` and `-11` are 1 and
2 GiB. `--max-memory` belongs to `decode`, where it is a cap; on `encode`
it is refused with a message naming the tiers. `decode --max-output` has
no default: a valid container decodes whatever it declares, and only a cap
the caller gives can refuse it (`FMT-6`, `RUN-1` in `docs/CONSTRAINTS.md`).

**Why.** The tier a file is written at is the memory every decoder of it
will need, so it is a promise about other machines, not a knob for this
run; a byte count on encode named a model no tier describes and made that
promise invisible. The 1 GiB output cap that the security hardening had
made a default was unrelated to the tiers (`-9` exists for inputs of
enwik9's size) and contradicted `FMT-6` as written; decompression bombs
are bounded by the declared size the header carries, which `cadapt info`
shows and a service caps with `--max-output`. Rejected: a byte-count
budget with units (`1G`, `50%`) on encode, the xz shape — it keeps the
knob the decision removes; a default output cap proportional to the tier —
disk and RAM are unrelated axes.

**Consequences.**
- A tier below the profile's floor is raised, never refused; the encode
  refusal "memory budget exceeded" is now decode's alone.
- The CLI tests pin the tiers' choices (a pipe at `-5`, `-7`; a named
  4 MiB input at `-4`, `-5`; `-9`/`-10`/`-11` on a pipe grow the max
  profile's table 2^24 → 9/8 of 2^24 → 10/8 of 2^25) and the two refusals.
- The library API is unchanged: `adapt_sizes_for_input(max_memory)` and
  `adapt_default_limits()` (1 GiB output for API callers) stay.
- `tools/ratio_floor.json` commands carry no `--max-memory`; the floors
  are unchanged. `bench/` scripts that passed it to encode are historical.

**Status.** Landed 2026-09-15 with the CLI, man page, README, CONSTRAINTS
codes, CLI and safety tests.
## 2026-09-15 — The text transform builds its book as it codes (versions 62–71)

**Decision.** A word both sides have seen written out three times becomes,
from then on, a code of one to three bytes at or above 0x80; each side
keeps the same bounded book (65,536 words, 1 MiB of letters), entering a
word at its third sighting and never sending the book. Words are maximal
runs of ASCII letters, 2 to 32 long, lower-case, capitalised or upper-case
(a flag byte before the code); everything else passes as it is, a byte at
or above 0x80 behind an escape. The encoder takes it for a file of 8 MiB
or more whose first block looks like text (two fifths of its bytes in
letter runs, under one in twenty-five at or above 0x80), or when asked
(`--transform text`); a pipe's length is unknown, so a pipe gets it only
when asked; never with the adaptive predictors, the BMP transform or the
x86 transform. Versions 62–66 front the normal, fast, max, compact and
lean models in that order, 67–71 with metadata; 58–61 are left to the
sized max profile of the `sized-table-2` branch, whose sizing byte has
room for a text bit when the two meet. The trailer's length and
checksum are the original's; the decoder undoes the transform as it
writes, and the proof alongside undoes it before checking the trailer and
holds the inverse of every produced byte against the input.

**Why.** The context table is the ratio at small budgets and text spends
it letter by letter; a word as one symbol reaches the same contexts with
a third fewer bytes. Measured on enwik8, the model unchanged:

| Run | Before | After | Change |
|---|---:|---:|---:|
| 128 MiB, `--max --allocation context` | 19,657,760 B | 18,971,412 B, 152.8 s, RSS 125.9 MB, decoded byte for byte | −3.49 % |
| default budget, same command (the locked floor) | 18,564,510 B, 189.4 s | 18,286,763 B, 158.9 s | −1.50 %, faster |
| 8 MiB proxy (`make gate`) | 1,716,715 B, 18 s | 1,716,213 B, 14 s | −0.03 %, faster |

Rejected, each measured: a dictionary built in a first pass and sent
ahead of the text (18,980,671 at 128 MiB, and a pass ahead of the model —
the operator's rule is that a transform is valid only when generated
during the coding, not prior to it); a book built into the codec
(FMT-2); implying the space before a word (worse, even with a
word-aware model: 1,736,886 vs 1,674,769 on the 8 MiB proxy at a 2^19
table); a case-sensitive book (+1.8 %); alphabetical or suffix-sorted
code order (+0.4 %, +0.2 %); entering a word at its second sighting
(18,993,159 vs 18,958,016 in the prototype). Below 8 MiB the transform
costs (64 KiB +3.2 %, 1 MiB +1.3 %, 4 MiB +0.4 %, 8 MiB −0.03 %, 32 MiB
−0.75 %), hence the length rule; on a text repeated many times the
recall model already owns the repetition and the transform costs, which
the CLI test notes.

**Provenance.** Designed and written in this repository. Word-replacement
transforms ahead of a compressor are published prior art (WRT and XWRT,
Skibiński and Grabowski; DRT, Mahoney); no code or data of theirs is
used, and this transform differs in building its book online on both
sides with no transmitted or built-in dictionary.

**Consequences.**
- Both locked floors improve on both axes on enwik8; the enwik9 floor is
  re-measured with this binary before the merge (see the commit).
- Each side's book, index and buffers are under 3 MiB (`TEXT_STATE_BYTES`),
  covered by the memory bounds' 8 MiB HEADROOM: the 128 MiB run above
  peaked at 125.9 MB against a bound of 127.3 MiB.
- A file under the transform whose blocks say fewer raw bytes than the
  trailer declares is a checksum error, as any other length lie.
- Changing the sighting count, the book's bounds, the byte classes or the
  length rule is a new entry with the enwik8 runs that justify it.

**Status.** Landed 2026-09-15 with `adapt_text.h`, `adapt_sizes.text`,
versions 62–71, decoder support on every path (stream, in memory,
threads), the proof alongside and after the fact, `ADAPT_ERROR_BAD_TEXT`,
`--transform text|none`, `cadapt info`, the vector
`tests/vectors/text-words.adpt`, C tests (the transform's own suite of
6,477 checks, the encoder's and decoder's), CLI tests and the gate runs
above.

## 2026-09-15 — A budget between two powers of two is spent on the context table (the sized max profile, versions 58/61)

**Decision.** The context table's block count is no longer bound to a
power of two: the sized max profile (version 58, 61 with metadata) carries
one sizing byte after the fixed header and the period bytes, which names
the whole model — bit 0 the compact side tables of the entries below, bit
1 their lean context set (never without bit 0), bits 2–4 the table's
fraction f, the table holding (8 + f) << (table_log − 3) blocks; bits 5–7
are reserved and zero. For a fractional table a hash picks its line by a
range multiply of its low 32 bits (`block_line`) — the idea is Daniel
Lemire's "fastrange" (2016, a published technique; the three lines here
are this project's own) — for a power of two by the mask as before, so
every unsized container's bytes are unchanged.
Under `--allocation context` the encoder, once the table has reached what
the budget holds as a power of two and the window has grown, gives the
slack to the table an eighth of its next doubling at a time; it never
shrinks the window for it, and a table at its target takes none. The
version is written only for a fraction above 0: a power-of-two table
keeps versions 21, 53 and 54, so the files those versions wrote — the
pinned `text-compact.adpt` and `text-lean.adpt` among them — decode as
written. The alternative, the byte on 53/54 themselves, was rejected for
that reason: it would have re-laid a version already on `main`.

**Why.** Measured on enwik8, `--max --allocation context`, this binary:

| Budget | Table before | Table after | Container | Change |
|---:|---:|---:|---:|---:|
| 256 MiB | 128 MiB (2^23), window 32 MiB | 160 MiB (10/8), window 32 MiB, bound bypassed | 19,185,877 → 19,066,780 B | −0.62 % |
| 256 MiB, first 32 MiB | 128 MiB | 144 MiB (9/8), window 32 MiB — the sizer's choice | 6,539,402 → 6,524,109 B | −0.23 % |
| 512 MiB | 256 MiB (2^24), window 128 MiB | 288 MiB (9/8), window 128 MiB — the sizer's choice | 18,831,629 → 18,780,150 B | −0.27 % |
| 256 MiB, first 32 MiB | 128 MiB, window 32 MiB | 160 MiB, window 16 MiB | 6,539,402 → 6,513,205 B | −0.40 % (not chosen: the window is not traded) |
| 128 MiB, first 32 MiB | 64 MiB, window 16 MiB (compact) | 80 MiB, window 1 MiB | 6,659,483 → 6,679,868 B | +0.31 % (why the window is kept) |

The table's exchange rate on this file is −2.46, −2.10, −1.85, −1.42 % per
doubling from 32 MiB (`docs/performance-audit-128mib-2026-09-15.md`); the
fraction spends what the power-of-two steps skip. At 128 MiB the compact
model leaves no slack, so the fraction is 0 and the entry below stands.

**Consequences.**
- The locked floors (enwik8 at 674 MiB, enwik9) reach their table target
  and take no fraction; their bytes are unchanged, `cmp`-identical on the
  8 MiB proxy.
- A decoder reads `compact`, `lean` and the fraction from the sizing
  byte on versions 58/61, where the byte is required (a header cut before
  it is truncated; a reserved bit set, or the lean bit without the
  compact one, is a bad header); on 53/54 it reads them from the version
  as before. Versions 59 and 60 name no format and are refused.
- The window policy (`--allocation window`) and the threaded sizers take
  no fraction; extending them is a new entry with its runs.
- Peak RSS follows the model: 240 MiB at 256 MiB (was 224).

**Status.** Landed 2026-09-15 with `adapt_sizes.table_frac`,
`adapt_table_blocks()`, the sizer rule, `cadapt info`, and C and CLI tests
(header parsing and its failure paths, the sizer's choices at 160 and 256
MiB, sized round trips of the full, compact and lean models through both
instruction-set paths, a named sized container with metadata).
## 2026-09-15 — Lean context set for compact tables of 2^22 blocks or fewer (versions 54/57)

**Decision.** A compact model whose context table is at most 2^22 blocks
leaves out three of its twenty hashed contexts — order-8, the third word
back, the word with its classes. The context policy sets `lean` with
`compact` at that table size; the container carries version 54 (57 with
metadata); a decoder sets `lean` from the version, so a version-53 file,
which the compact tables' first release wrote with every context, decodes
as it was written. `tests/vectors/text-compact.adpt` was written by that
release and `text-lean.adpt` by this one; both are pinned.

**Why.** At that table size the three contexts' claims evict more than
they predict. Measured on enwik8 with the compact tables, same binary,
same command but for the budget:

| Budget | Table | Every context | Lean set | Change |
|---:|---:|---:|---:|---:|
| 94 MiB | 2^21 | 20,258,968 B | 20,150,130 B | −0.54 % |
| 128 MiB | 2^22 | 19,715,577 B | 19,657,760 B | −0.29 % |
| 192 MiB | 2^23 | 19,284,703 B | 19,293,827 B (not chosen) | +0.05 % |

Leaving out order-8 alone gained 0.06 % at 128 MiB; order-8 with the
indirect and nesting contexts lost 0.33 %; so the set is those three, and
the bound is the compact tables' own (`ADAPT_COMPACT_TABLE_LOG`).

**Consequences.**
- The locked floors (enwik8 at 674 MiB, enwik9) keep the full tables and
  every context and are byte-identical; `make gate` was run on this build.
- The lean set has its own wire versions rather than a rule over the
  header's fields: the compact versions 53/56 are on `main` and a file
  they wrote must not decode under another context set.
- Changing the set, or applying it to the full tables, is a new entry with
  the enwik8 runs that justify it.

**Status.** Landed 2026-09-15 with `adapt_sizes.lean`, versions 54/57,
decoder support, the two vectors, C and CLI tests (round trips of the lean
and the compact model at the same sizes, the version-byte checks, the
sizing boundaries, `cadapt info`) and the gate run above.

## 2026-09-15 — Small max budgets buy context table with compact side tables (versions 53/56)

**Decision.** Under `--allocation context`, when the budget stops the max
profile's context table at 2^22 blocks or fewer and short of the input's
target, the encoder halves the third and fourth arbiters' weight sets (2^16
→ 2^15) and the second estimation stage's rows (2^18 → 2^17) — 26.5 MiB —
and grows the table with what that frees. The container carries version
53 (56 with metadata); a decoder sets `compact` from the version, so every
existing max file keeps its full tables.

**Why.** At small budgets the fixed side tables (60.9 MiB with the full
set) crowd out the table that carries the ratio. Measured on enwik8, same
binary, same command but for the budget:

| Budget | Full tables | Compact tables | Change |
|---:|---:|---:|---:|
| 94 MiB | 20,712,600 B | 20,258,968 B | −2.19 % |
| 128 MiB | 20,091,771 B | 19,715,577 B | −1.87 % |
| 192 MiB | 19,597,276 B | 19,284,703 B | −1.60 % |
| 256 MiB | 19,185,877 B | 19,251,456 B (not chosen) | +0.34 % |

A control at 90 MiB with the compact tables but the same 2^21 table as the
full ones cost +1.34 %: the gain is the table doubling, not the smaller
side tables, which is why the rule is tied to the table's size and why
budgets whose table already passes 2^22 keep the full set.

**Consequences.**
- The locked floors (enwik8 at 674 MiB, enwik9) reach their table target
  with the full tables and are byte-identical; `make gate` and
  `tools/check_ratio_floor.py --verify` on enwik8 were run on this build.
- The window policy (`--allocation window`, the CLI default) is unchanged:
  it spends a small budget on the window, where the compact tables were
  not measured to pay.
- The minimum max budget is still set by the full tables (about 70 MiB):
  the switch happens after the full-table sizing, never instead of it.
- Raising or lowering `ADAPT_COMPACT_TABLE_LOG` (22) or the compact sizes
  is a new entry with the enwik8 runs that justify it.

**Status.** Landed 2026-09-15 with `adapt_sizes.compact`, versions
53/56, decoder support for both, C and CLI tests (a compact round trip,
the version-byte checks, the sizing boundaries) and the gate runs above.

## 2026-09-15 — Size floors re-locked: the recall index grows from the budget the table and the window leave

**Decision.** The sizing capped the recall index at 2^20 entries whatever
the budget; enwik8 has a hundred positions per entry at that size. Both
sizing paths now grow it last, to 2^22, from whatever the budget has left
once the table and the window have all they can use, never displacing
either; the index is capped at the input's positions. The header carries
the index size, so the format is unchanged, and the size floors move to
what the sizing now writes:

| Input | Was | Now |
|---|---:|---:|
| enwik8, `--max --allocation context`, 674 MiB | 18,564,510 B | **18,529,513 B** |
| enwik8, first 16 MiB | 3,340,447 B | **3,337,011 B** |
| enwik8, first 8 MiB (the CI proxy) | 1,716,715 B | **1,715,521 B** |

At 128 MiB nothing is left after the compact tables and the window, the
index stays at 2^20 and the container is unchanged (19,657,760 B). The
enwik9 floor (155,906,798 B) stands unmeasured; the throughput floors are
unchanged (this container: 476 s for the enwik8 encode, within its noise
of the previous codec's 462 s).

**Why, measured.** On the 16 MiB enwik8 prefix at the default budget, a
2^22 index codes 3,330,164 B against 3,333,487 with 2^20 (−0.10 %) and 2^24,
which enwik8's budget cannot hold, 3,329,340; at 128 MiB an index that
displaced the window cost 0.01 to 0.2 %, which is why it takes only what is
left. Also measured on the same prefix and rejected as within noise or
worse: a lower mixer update threshold (0 and 2), estimation rate 1/64,
estimation shares 4/8 and 7/8, mixer rate floor 4 and ceiling 256, decay 48
and 192, map count limits 127 and 1023, a third estimation stage keyed by
the recall state, dual-rate state maps (rates 1/8, 1/16, 1/32), a 16-bit
coder probability (0.006 %), seven alternative contexts in the weakest
slot, and aligned arbiter rows (no speed change on the hand-written dot
product). A first-layer starting weight of 1/32 instead of 1/4 measured
0.21 % smaller at the default budget and 0.18 % at 128 MiB, but changes
every file's bytes and so waits for a format version.

**Status.** Implemented 2026-09-15; `make gate` passes with the round trip
verified.

## 2026-09-14 — benchmark independence and regression-file ownership

General benchmark orchestration belongs to the standalone codecbench project.
Codec builds and correctness tests do not depend on it or on `bench/`. The
canonical locked-floor data and checker have moved to `tools/ratio_floor.json`
and `tools/check_ratio_floor.py`; all numbers and verification rules in the
existing floor decision are preserved. References to their old `bench/` paths
below identify retained historical snapshots. `make gate` and `make ratio-floor`
use the canonical tools. Allocation and adaptive-search correctness stay with
the codec. No historical measurements or plots are removed by this separation.

## 2026-09-14 — enwik8 throughput floors raised: the per-bit path at 0.55 of its time, the same bytes

**Decision.** The three enwik8 floors below are raised to what the per-bit
path landed in `085dff0` measured with the floor checker; the containers are
the ones already locked, byte for byte. enwik9 is not re-measured (a
52-minute run) and keeps its entry.

| Input | Container | Wall time (was) | Throughput (was) |
|---|---:|---:|---:|
| enwik8 (100,000,000 B) | **18,564,510 B** | 189.4 s (263 s) | 527,983 B/s (380,228) |
| enwik8, first 16 MiB | **3,340,447 B** | 33.8 s (62.76 s) | 496,367 B/s (267,323) |
| enwik8, first 8 MiB | **1,716,715 B** | 17.9 s (36.58 s) | 468,637 B/s (229,322) |

**The run.** `tools/check_ratio_floor.py --baseline-binary <build of 815b7a3>
--verify`, binary sha256 `fb4ff848…4b26c` (`cadapt 0.12.0`, format version
19), 2026-09-14 22:31–23:10 local on the reference host while other agents'
jobs shared it (load average about 3 for the full file, 1.2 for the proxies).
The locked-commit binary in the same jobs took 347.3 s, 59.1 s and 31.7 s —
the new binary is at 0.55–0.57 of its time on every fixture, and each container
decoded byte for byte. On the operator's quiet run the old binary took 263 s;
the new times are from a loaded host, so they are lenient floors in the
sense of the entry below, to be re-locked from a quiet run.

**Why.** The path is instruction-bound, not memory-bound; the gains came from
removing branches, an explicit vector dot product, running the memories as
vectors, and exact prefetches planned from the byte the encoder is coding.
`docs/performance-audit-2026-09-14.md` ("Results") has the step-by-step
measurements and the variants that measured as no gain.

**Consequences.**
- The gate's tolerance stays 10 %: a build must reach 475,185 B/s on full
  enwik8 on the reference host (or 90 % of a locked-commit build's rate in the
  same job elsewhere), and 421,773 / 446,731 B/s on the proxies.
- `ADAPT_ISA=baseline` runs the portable per-bit path; it is held to the same
  bytes by the CLI tests but not to these throughput floors.
- The halving the operator asked for (131 s against the 263 s run) is not
  reached: the gate's own paired run puts the new binary at 0.55 of the old
  one's time, a 1.83× speed-up.

**Status.** Locked 2026-09-14, `tools/ratio_floor.json` and its test updated
in the same commit.

## 2026-09-14 — enwik8 and enwik9 with `--max --allocation context` are floors: size and throughput may only improve

**Decision.** The operator's measurements of `cadapt encode --max --allocation
context` on this host (AMD Ryzen 5 5600X) are locked as floors:

| Input | Container | Wall time | Throughput |
|---|---:|---:|---:|
| enwik8 (100,000,000 B, md5 `a1fa5ffd…b36a`) | **18,564,510 B** (18.56 %) | 263 s | 380,228 B/s |
| enwik9 (1,000,000,000 B, md5 `e206c345…a12d`) | **155,906,798 B** (15.59 %) | 3,123 s | 320,205 B/s |

A change that makes either container larger by one byte, or the throughput
lower by more than the 10 % tolerance recorded in `bench/ratio_floor.json`, is
a regression and does not ship. The floors are checked by
`bench/check_ratio_floor.py` (`make ratio-floor`), which also refuses to run
against any input whose size or MD5 differs from the locked one.

**CI proxy.** No CI gate may run longer than 120 s, and the full inputs take
minutes and an hour, so CI gates on a proxy: the first 8 MiB of enwik8
(md5 `237766a9…0458`), locked at **1,716,715 B** and 36.58 s (measured under
load; about 22 s quiet), run by `make gate` in encode-plus-verify form. A 16 MiB
prefix (md5 `01d6ef7e…48a4`, **3,340,447 B**, 62.76 s) is locked for pre-merge
use. Calibration on 2026-09-14 across three configurations (default, `--max`,
`--max --allocation context`) showed the prefixes preserve the configurations'
ordering from 1 MiB up and approach the full-file gap by 16–32 MiB (default is
+3.65 % on the full file, +2.2 % at 8 MiB, +2.9 % at 16 MiB).

**Why.** The two numbers are the codec's public position: #31 of 224 on LTCB's
enwik9-plus-decompressor ranking, #20 of 215 on the enwik8 column
(`research/ltcb-tables.md`). Time matters as much as size for the product goal
(ratio first, at speeds people will use), so both are locked.

**Consequences.**
- The proxy is blind to the allocation policy: on every prefix up to 32 MiB
  `--allocation context` and plain `--max` produce byte-identical output. The
  267,119-byte gain that policy delivers on full enwik8 is guarded only by the
  full floors, so `make ratio-floor` runs before any release, and the enwik9
  floor on demand.
- Throughput is host-bound. On the reference host it is gated against the
  locked figure; elsewhere `--baseline-binary` gates it relative to a build of
  the locked commit run in the same job, and without a baseline the checker
  prints the throughput and states that it is not gated. Size is gated
  everywhere.
- The locked proxy times were measured while five encoders and a paq8px job
  shared the host; they are lenient floors, to be re-locked from a quiet run.
- Raising a floor is a new entry here, with the run that justifies it.

**Status.** Locked 2026-09-14. The enwik8 container was decoded byte-exact by
the frozen baseline decoder (`research/current-baseline.md`); the enwik9
container's decode verification was in progress when this entry was written
and its result is recorded in `research/ltcb-local-entries.csv`.

## 2026-09-14 — The encoder's memory profile follows xz

**Decision.** `cadapt encode` takes presets `-0` … `-9` with xz's
compression-side memory figures (3, 9, 17, 32, 48, 94, 94, 186, 370, 674 MiB;
the lowest ones clamp to this codec's ~15 MiB floor), and the default is `-9`,
674 MiB. Before this the default budget was 64 MiB. `--max-memory` still
overrides any preset. Context: the product goal is the best practical
compression tool — ratio first, at speeds people will use.

**Why.** Measured on enwik8 (100 MB), same speed at every budget; the gain is
the context table, not the window (a 4× larger window alone gave 0.25 %):

| Budget | Output bytes | vs 64 MiB |
|---|---|---|
| 64 MiB | 21,221,772 | — |
| 256 MiB | 20,321,065 | −4.2 % |
| 674 MiB (xz -9) | 19,943,347 | −6.0 % |

**Consequences.**
- Decoding needs the same model memory — context mixing is symmetric, so a
  file made at `-9` takes ~372 MB of RAM to decode on any machine. The
  decoder's default budget rises to 674 MiB with it. xz's small
  decompression footprint cannot be copied.
- Inputs under 8 MB are unaffected: their model already fitted in 64 MiB.
  The `--fast` profile is unaffected: its table is capped at 2 MiB.
- `--verify` needs two models; at `-9` on large inputs they no longer fit
  the budget together, so the proof runs as a second pass.

**Status.** Implemented 2026-09-14: `ADAPT_DEFAULT_MAX_MEMORY` = 674 MiB,
presets `-0` … `-9` in `cadapt encode`, README and tests updated; the plain
default on enwik8 measured 19,943,379 bytes at 372 MB RSS and decodes with
default settings.

## 2026-09-14 — Format 19–24: four arbiters, a second estimation stage, ten max-profile contexts, tables to 2^26

**Decision.** The model gains two first-layer arbiters (keyed by the order-2
hash and by the word hash, each with the partial byte), a second APM stage
keyed by an order-2 hash and averaged with the first, an arbiter
learning-rate floor of 8 (was 20), and a context table that grows to a block
per four input bytes up to 2^26 blocks (was per eight, up to 2^24). The max
profile gains six more hashed contexts — markup nesting, orders 5 and 8, the
case-folded word, the last three words, the word with the byte classes before
it — for twenty in all. The context-set chooser keeps a leaner set only when it
beats the full one by 1/512 of the sample and, on a long block, by 1/256 of
the block (was 1/1024 and any margin). Byte streams are versions 19/20/21,
named files and folders 22/23/24; 13–18 are refused. Context: the goal is the
best practical compressor, ratio first; the target is enwik8 under
17,674,700 bytes at 1 GiB.

**Why.** Measured on enwik8 (100 MB) and the eight-file corpus (22 MB:
libc, python, CSV, JSON, XML, two prose files, 8 MiB of enwik8):

| Run | Before | After | Change |
|---|---|---|---|
| enwik8, `--max`, 1 GiB | 19,370,336 | 18,563,312 | −4.2 % |
| enwik8, `--max`, `-9` (674 MiB) | — | 18,831,629 | table 2^24: the 2^25 one no longer fits beside the window |
| enwik8, normal, `-9` | 19,943,347 | 19,519,006 | −2.1 % |
| enwik8, normal, 64 MiB | 21,221,772 | 20,703,181 | −2.4 % |
| corpus, normal | 4,509,618 | 4,447,785 | −1.37 % |
| corpus, fast | 5,041,743 | 5,042,704 | +0.02 % |
| corpus, max | 4,448,746 | 4,358,469 | −2.03 % |

Each part earned its place on the 16 MiB enwik8 prefix under `--max`: the
two arbiters −1.48 %, the rate floor −0.35 %, nesting with orders 5 and 8
−0.84 %, the second APM stage −0.32 %, the three word contexts −0.22 %, the
2^25 table −0.27 % (full file). Rejected as null or worse: six arbiters, an
arbiter keyed by the recalled byte, a longer hash key, last-bit states, a
recall minimum of 7, skipping arbiter updates, a map limit of 511, larger run
memories, indirect contexts, a capitalization transform, a dictionary
transform, profile-guided optimization.

**Consequences.**
- Time: the normal profile takes about 30 % longer per byte (8.8 → 11.4 s on
  8 MiB of enwik8, same table), the fast profile about 2.5 % (its model is
  unchanged), the max profile about twice what it took and about 2.5× the
  normal profile (enwik8: 123 s normal, 326 s max at 1 GiB).
- Memory: the smallest normal model is 28,178,567 bytes (was 14,928,007), the
  smallest max model 73,283,719 (was 17,143,943) — the arbiters' 2 × 65,536
  weight sets and the second APM stage are fixed costs. Presets `-0`, `-1`
  and `-2` therefore all clamp to the normal floor; under `--max`, `-0` to
  `-4` clamp to its floor (the CLI raises a preset to the floor of the profile
  in force instead of refusing). A file made at `-9` needs about 650 MB to
  decode (was ~372 MB): the table is 2^25 blocks for inputs of 64 MiB and
  more. The largest model a header can name is about 2.1 GiB.
- The fast profile carries the larger floor without using it: its arena still
  reserves the arbiters and estimation stages it never reads.
- The bench (`bench/telemetry.py`) defaults to the codec's 674 MiB budget; at
  its old 64 MiB a two-segment max run no longer fits.

**Status.** Implemented 2026-09-14 in `adpat/c`: model, encoder chooser,
CLI presets and help, vectors regenerated at versions 19–24, tests and bench
pinned to the new versions and sizes; `make check` passes. The 18,563,312-byte
archive decodes to the corpus byte for byte.
