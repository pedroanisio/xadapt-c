<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-17, from the operator's
request to plan a path that wins ratio, memory and speed together. Every
number is from a run on the host named under The surface; nothing is
normalised across machines. -->
# Winning ratio, memory and speed — the path

Part of [adapt](../README.md).

Two of the three axes are already won at every reference operating point, and
the whole programme is one axis and one number: **1.60× of time**, at
`kanzi -l9`'s point, widening to 3.91× at its fastest. The only lever measured
so far that moves a number that large is the front end — the text transform is
worth **1.43× of time and 5.75 % of bytes at the same time** — and every
model-side lever tried has returned 1.11× or less. The path is therefore to
spend on transforms that cut the bytes the model codes, not on making the
model cheaper per bit.

## The goal, made checkable

"Winning on all three" has one testable meaning: for each operating point a
reference offers, hold a configuration that is at least as good on bytes, on
seconds and on peak memory, and better on one. Anything weaker — "we win on
ratio", "we own the region below 20 MB" — is a claim about territory the
reference never entered, and it does not survive a customer putting the two
binaries side by side at the same setting.

By that test, today, **we dominate none of the three reference points**:

| Reference | Bytes | Seconds | Peak RSS | Our best at ≤ its bytes *and* ≤ its memory | Time gap |
|---|---|---|---|---|---|
| `kanzi -l7` | 22,817,366 | 8.0 | 96.0 MiB | `--fast -4` — 21,996,885 B, 29.6 MiB | **3.91×** |
| `kanzi -l8` | 21,181,998 | 21.9 | 266.0 MiB | `--mid -4` — 20,253,568 B, 43.7 MiB | **2.63×** |
| `kanzi -l9` | 20,035,687 | 34.7 | 905.1 MiB | `--mid -9` — 18,761,302 B, 600.8 MiB | **1.60×** |

At every point we are smaller and we use less memory. At every point we are
slower, and the deficit grows as the reference gets faster. That is the whole
problem statement.

## The surface

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container — not
the reference host of `tools/ratio_floor.json`, so no second here may be
compared to a locked floor. Input enwik8, one core (`kanzi -j 1`), wall
seconds, peak RSS from `getrusage` on a fresh child per point. The
`--max --allocation context --contexts lean -9` row is the locked container
byte for byte. Data: [three-axis-surface.json](three-axis-surface.json).

| Configuration | Bytes | Seconds | Peak RSS |
|---|---|---|---|
| `--fast -4` | 21,996,885 | 31.3 | 29.6 MiB |
| `--fast -5` | 21,663,745 | 33.6 | 60.5 MiB |
| `--fast -7` | 21,453,294 | 33.1 | 94.6 MiB |
| `--fast -9` | 21,663,531 | 31.6 | 90.6 MiB |
| `--mid -4` | 20,253,568 | 57.7 | 43.7 MiB |
| `--mid -5` | 20,041,401 | 58.9 | 74.8 MiB |
| `--mid -7` | 19,902,024 | 59.7 | 108.8 MiB |
| `--mid -9` | 18,761,302 | 55.6 | 600.8 MiB |
| default `-4` | 19,954,449 | 81.4 | 43.7 MiB |
| default `-7` | 19,599,719 | 82.9 | 108.7 MiB |
| default `-9` | 18,500,473 | 79.8 | 600.7 MiB |
| `--max` lean `-4` | 19,862,298 | 118.7 | 50.8 MiB |
| `--max` lean `-7` | 18,542,411 | 126.5 | 181.6 MiB |
| `--max` lean `-9` | 17,902,645 | 126.3 | 654.9 MiB |
| `kanzi -l7` | 22,817,366 | 8.0 | 96.0 MiB |
| `kanzi -l8` | 21,181,998 | 21.9 | 266.0 MiB |
| `kanzi -l9` | 20,035,687 | 34.7 | 905.1 MiB |

## Four findings that determine the path

**Speed is the only blocking axis.** Ratio was never in doubt: the nearest
reference point to our best is 11.9 % larger. Memory turns out to be won too,
and comfortably — `kanzi -l9` peaks at 905 MiB where our `--mid -9` peaks at
601 and our `--mid -7` at 109 for 0.7 % fewer bytes than `-l9`. Nothing in the
plan below needs to defend ratio or memory; every phase is judged on seconds
at fixed bytes.

**Memory is bought by policy, and buys no speed.** Across every profile the
wall time is flat in the tier while peak RSS moves twentyfold: `--mid` runs
57.7 s at 43.7 MiB and 55.6 s at 600.8 MiB. The tables are not the bottleneck
— the prefetch plan already hides them — so shrinking them is free on time and
costs only the ratio the smaller tables give up. `--fast` is the extreme case:
`-5` and `-9` produce containers within 214 bytes of each other, at 60.5 MiB
against 90.6. A tier default is a policy change, not an engineering one.

**The front end is the only lever that has ever returned more than 1.11×.**
The text transform, measured today on `--mid` over the whole of enwik8:
81.7 s and 19,906,051 B with `--transform none`, **57.1 s and 18,761,302 B**
with it — 1.43× of time and 5.75 % of bytes together. Time falls in proportion
to the bytes the model is handed, so a transform that removes 30 % of the
stream removes 30 % of the work. Against that, every model-side experiment on
record returned little: context-slot deletion 1.11× on a full file, mixer lane
compaction a measured ceiling near 1 %, per-byte state gating +2.3 % on
`--fast` and nothing elsewhere, line-aligned arbiter rows negative.

**The model gap alone does not explain the deficit.** `--mid` is about 320
cycles per bit (`LOCKS.md`, the `--mid` entry) against kanzi's TPAQX at 229
(the speed batch of 2026-09-16), a factor of 1.40; the wall gap at equal bytes
is 1.72×. Slot ablation prices a kind at roughly 24 cycles per bit, so 7 kinds
down to 4 reaches about 248 — near TPAQX's budget, at a ratio cost the price
tables can quote per corpus row, and still short of closing 1.72× on its own.
Model surgery is a contributor, not the answer.

## The path

Each phase states what it buys, what it costs, and the gate that decides
whether the next one is needed.

**Phase 1 — take the memory axis by policy. XS.**
Give each profile a default tier at the knee of its own ratio-against-RSS
curve instead of `-9`. On the evidence above `--fast` should default near `-5`
(identical container, two thirds the memory) and `--mid` near `-7` (+6.1 % of
bytes for a fifth of the memory) — the exact points to be re-measured on the
mixed corpus, since the knee is input-dependent exactly as the context sets
were. The locked floors name explicit tiers and do not move. *Gate:* every
mixed-corpus row within its floor at the new default.

**Phase 2 — price the stream the model still codes. S.**
The transform is worth 1.43× because it removes about 30 % of the bytes. What
is left is the budget for everything after it, and nobody has counted it:
after the word transform, how much of enwik8 is markup structure, numbers,
whitespace runs, and UTF-8 that packs? Build the census, because it ranks
every candidate in Phase 3 and costs an afternoon. *Gate:* a table of the
post-transform stream by class, with the share of coded bytes each holds.

**Phase 3 — extend the front end against that census. M each, and they stack.**
`kanzi -l9` reaches its point with `EXE+RLT+TEXT+UTF+DNA` ahead of its mixer;
we run one transform. The candidates, each reversible, each carrying its own
detector so unmatched bytes pass through (`FMT-8`), each `FMT-2`-clean because
it is built from the input alone:
- **Run-length collapsing** of whitespace and repeated bytes, which XML has in
  quantity and which `kanzi` carries at every level from 8 up.
- **Structural coding of markup** — the repeated `<page>`, `<revision>`,
  `<timestamp>` skeleton that `nest` currently models context-by-context
  rather than removing.
- **UTF-8 packing** and **small-alphabet packing** for the rows of the mixed
  corpus that are not English prose.
*Gate:* each is accepted only if it returns time in proportion to the bytes it
removes, on the mixed corpus as well as enwik8. Phase 2's census says which to
build first; a candidate that covers under 5 % of the stream cannot repay an M.

**Phase 4 — the model, only if Phases 1–3 leave a gap. M to L.**
Two priced levers remain: kinds, at about 24 cycles per bit each with the
ratio cost quoted per row by `bench/slot_price.py`, and the second estimation
stage. Together they are worth roughly 1.3× from `--mid`'s 320 cycles per bit.
Spend them last, because they are the only levers here that cost ratio to buy
time, and because Phase 3 improves both at once.

**Phase 5 — the fallback, stated so it is a choice and not a surprise.**
If the gap survives, threads close it in wall time at 2.1–2.3× for 6 % of
ratio and a linear memory cost (measured, `speed-3x-experiments-2026-09-16.md`).
That is a worse trade than anything above and it is the right answer only for
a caller who has cores to spend and has said so.

## What not to spend on

Recorded so nothing here is tried a third time. Context-slot deletion alone
(1.11× on a full file, and the set does not transfer across inputs). Mixer
lane compaction (ceiling near 1 %). Line-aligned arbiter rows (1 % slower).
Per-byte state gating (nothing outside `--fast`). Segment sizing at fixed
memory (larger segments produce larger containers). An LZP-style bypass
(measured twice, on text and on the redundant rows, and dropped both times).
A flag-free determinism gate (under a tenth of the bytes on text).

## Position

Run Phase 2's census next, because it is cheap and because every Phase 3
candidate is a guess without it. Phase 1 can run beside it and is the only
part of this plan that improves a published axis without touching the codec.

The honest shape of the result: Phases 1–3 plausibly reach `kanzi -l9`'s point
— 1.60× is one more transform of the size we already have — and the fast end
is a harder question, because 3.91× at `kanzi -l7`'s 8.0 seconds is a
different class of compressor, not a tuning of this one. A codec that wins all
three axes at every reference point probably concedes the region above 22 MB
and wins everything below it; that is worth saying out loud before the work
starts, because it decides whether `--fast` is repaired or retired.

One caveat the table understates. `kanzi` was run at `-j 1` to make the
comparison per-core and therefore algorithmic. Its format is block-parallel by
design, so on a four-core host its wall time falls by roughly the core count
while ours falls at a cost of 6 % of ratio. Measured as a product on real
hardware rather than as an algorithm on one core, the speed gap is wider than
every figure above.
