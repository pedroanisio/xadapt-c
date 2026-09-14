# Locked decisions

Decisions that are settled. A change to any of them is a new entry, not an edit.

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
