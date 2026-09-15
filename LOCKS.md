# Locked decisions

Decisions that are settled. A change to any of them is a new entry, not an edit.

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
