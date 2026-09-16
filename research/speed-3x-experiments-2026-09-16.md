<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-16, from the
operator's question of that day: what three reference codecs suggest for a
threefold speedup at the locked ratio and the locked memory. Every number
below is from a run on the host named under Measurements, or is quoted from
the named reference's own README. -->
# Three times the speed at the locked ratio — what the reference codecs are worth

Part of [adapt](../README.md).

A threefold speedup at the locked ratio comes from segment sizing on large
inputs and from narrowing the per-bit path — not from the front-end bypasses
that make `mcm` and `kanzi` fast — because two measurements taken here show
that enwik8-class text carries almost no easy-byte population for a bypass to
remove. The bypasses remain worth building, but as input-detected paths under
`DLV-3`, priced on the mixed corpus, not as a route to the text benchmark.

## Provenance

`kanzi-cpp` is Apache-2.0; `mcm` and `zmix` are GPL-3, which is incompatible
with this tree's MIT licence. Under `DLV-4` no source file of `mcm` or `zmix`
was opened: only their `README.md`, `NOTICE.md`, `writeup.md` and
`docs/ALGORITHMIC-IDEAS.md`, which are prose descriptions, and their directory
listings. Of `kanzi-cpp`, the `README.md` and the file inventory of
`src/transform/` and `src/entropy/` were read; no source file was transcribed.
Every technique named below is attributed to its published description, and
each experiment is to be implemented from that description alone.

`zmix` additionally carries pretrained transformer weights as a shipped asset.
That design is closed to this codec by `FMT-2`, whatever its licence.

## The exchange rate the references establish

A threefold speedup is not a rung on a ratio ladder, because the published
ladders are far too shallow to hold one. `mcm` measures five rungs of the same
implementation on one host, so its rungs are comparable to each other:

| `mcm` v0.83, enwik8 | Bytes | Encode | vs `-x9` bytes | vs `-x9` time |
|---|---|---|---|---|
| `-t9` (turbo) | 20,199,979 | 40 s | +9.9 % | −38 % |
| `-f9` (fast) | 19,313,858 | 43 s | +5.1 % | −33 % |
| `-m9` (mid) | 18,627,061 | 49 s | +1.3 % | −23 % |
| `-h9` (high) | 18,504,139 | 58 s | +0.7 % | −9 % |
| `-x9` (max) | 18,379,121 | 64 s | — | — |

Source: the `mcm` README, measured by its author on an i7-2630QM. This codec's
locked figure is 17,902,645 B in 117.1 CPU s on a Ryzen 5 5600X
(`tools/ratio_floor.json`) — a better ratio than every rung above, on a much
faster machine. The two hosts are not comparable, so the ladder is quoted for
its *shape*, not its absolute times: within one implementation, giving up
1.3 % of ratio bought 23 % of the time, and giving up 9.9 % bought only 38 %.
Spending ten percent of ratio does not even reach twofold, so a threefold
speedup has to come from a source outside the ratio trade.

`mcm`'s README names where its own speed comes from: "LZP for faster
compression of highly compressible files", "binary + text detection for model
switching", and an E8E9 filter. The first two are the bypass families priced
below. `kanzi` names a different source — block-parallel compression across
cores, with a transform chosen per block from a catalogue
(`src/transform/`: BWT/BWTS, ROLZ, LZ, RLT, ZRLT, SRT, SBRT, EXE, FSD, UTF,
alias/pack). `zmix`'s `docs/ALGORITHMIC-IDEAS.md` names a third: deleting
model slots at compile time when they stop earning their cycles, narrowing
caches to int8 where the layer is memory-bound, and its measured finding that
a better mixer is worth about one byte over a 10⁹-byte file. That last
result is the licence for the whole of group B below: mixer *quality* has
almost no ratio value left, so mixer *cost* can be attacked hard.

## Measurements

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container. This
is **not** the reference host of `tools/ratio_floor.json`, so no time below
may be compared to a locked floor. Container sizes are host-independent and
they do match: the 8 MiB and 16 MiB rows measured here are 1,681,725 B and
3,258,121 B, the two locked figures for those fixtures byte for byte. Sizes
below are therefore evidence; times are this container's only, and are quoted
as ratios between runs in the same job.

Command throughout: `cadapt encode --max --allocation context --contexts lean`.
Coverage rows come from `bench/redundancy_probe.c` (tests in
`bench/test_redundancy_probe.py`).

### An LZP bypass is worth at most 1.4× on this text, at an unusable error rate

A literal-prediction bypass hashes the last N bytes, predicts the byte that
followed that context last time, and codes one flag instead of eight modelled
bits while the last K predictions were all right (C. Bloom, "LZP: a new data
compression algorithm", DCC 1996; M. Mahoney, *Data Compression Explained*).
Measured over the first 8 MiB of enwik8, its best configurations are:

| Order | Run K | Hit rate | Covered | Error inside | Model work left |
|---|---|---|---|---|---|
| 4 | 1 | 55.24 % | 55.24 % | 35.46 % | 71.3 % |
| 6 | 1 | 51.92 % | 51.92 % | 31.62 % | 71.0 % |
| 6 | 4 | 51.92 % | 18.47 % | 24.46 % | 88.4 % |
| 8 | 8 | 41.57 % | 6.45 % | 16.81 % | 95.4 % |
| 16 | 32 | 13.99 % | 0.95 % | 5.04 % | 99.2 % |

The ceiling is 71 % of the model work, so **1.4×**, and it is reached only
where roughly a third of the bypassed bytes are wrong — each of which pays a
flag *and* a full modelled byte, and each of which denies the context models
the update they would have had. Raising K to buy a tolerable error rate
collapses coverage to a few percent. This is consistent with what `mcm`'s
README actually claims for LZP: it is for "highly compressible files", and
enwik8 at 1.43 bits per byte is not one.

### A flag-free determinism gate covers under a tenth of the bytes

The second family avoids LZP's flag entirely. A context is *settled* when it
has occurred at least `--min-seen` times and was always followed by the same
byte; the gate fires where two orders are both settled and agree. Both sides
derive that predicate from state they already hold, so it carries no side
channel (`FMT-2`) and costs no flag bit — only whatever a reduced model loses
on the bytes it covers. Measured on the same 8 MiB:

| Orders | Settled after | Covered | Error inside |
|---|---|---|---|
| 4 + 6 | 2 | 6.26 % | 1.475 % |
| 6 + 8 | 2 | 9.22 % | 2.156 % |
| 4 + 6 | 8 | 4.18 % | 0.285 % |
| 6 + 8 | 8 | 5.20 % | 0.388 % |

The error rate is excellent and the coverage is irrelevant. At 9.2 % coverage,
a gated path five times cheaper than the full twenty-slot model saves 7 % of
the work — 1.08×. English text simply does not hold long deterministic
stretches: the population of bytes a CM finds trivially easy, and would be
glad to hand to a cheaper path, is under a tenth of the file.

Together these two results close the front-end route on text. They do not
close it on the mixed corpus, where records, sorted data, binaries and logs
carry exactly the redundancy enwik8 lacks — which is why both families appear
below as group C rather than being abandoned.

### A model restart costs about 105 KB, whatever the segment size

Encoding prefixes of enwik8 prices the cold start directly:

| Prefix | Container | Marginal | Excess over 1.5034 b/B |
|---|---|---|---|
| 1 MiB | 226,919 B | 1.7313 b/B | 29,892 B |
| 2 MiB | 449,812 B | 1.7005 b/B | 55,757 B |
| 4 MiB | 873,491 B | 1.6162 b/B | 85,381 B |
| 8 MiB | 1,681,725 B | 1.5416 b/B | 105,506 B |
| 16 MiB | 3,258,121 B | 1.5034 b/B | 105,682 B |

The excess is measured against the marginal rate a warm model has reached by
16 MiB, not against enwik8's whole-file 1.4322 b/B, which keeps the figure
conservative. It plateaus: a model that starts empty costs about **105 KB and
then stops costing anything**. The penalty is a constant per restart, not a
rate.
Running the threaded encoder confirms it end to end on the 16 MiB prefix:

| Configuration | Segments | Container | vs 1 thread | Wall | Speedup |
|---|---|---|---|---|---|
| `--threads 1` | 1 | 3,258,121 B | — | 28.0 s | 1.00× |
| `--threads 2 --segment-bytes 16MiB` | 1 | 3,258,134 B | +13 B | 28.1 s | 1.00× |
| `--threads 2 --segment-bytes 8MiB` | 2 | 3,338,606 B | +80,485 B | 22.3 s | 1.26× |
| `--threads 4 --segment-bytes 4MiB` | 4 | 3,414,956 B | +156,835 B | 12.3 s | **2.28×** |

Two things fall out. Fitting the memory sizes to `n` threads costs 13 bytes,
so the whole parallel ratio cost is restarts and nothing else. And the cost is
per restart — 52 to 80 KB here, below the 105 KB plateau only because a
restart inside a 16 MiB file lands on a model that was never fully warm.

This reprices the audit's headline objection. `docs/performance-audit-2026-09-14.md`
measured parallel segments at **4.1 % of ratio** and the finding has stood
since. It is a property of cutting a 100 MB file into 16 MiB pieces, not a
property of parallelism: five restarts at 105 KB is 525 KB, which is 2.9 % of
enwik8's 17.9 MB and 0.36 % of enwik9's 146.8 MB. `ADAPT_SEGMENT_BYTES`
(`adapt.h:84`) is a fixed 16 MiB, so a larger file is cut into more pieces
rather than bigger ones — exactly backwards. Sizing segments from the input
length instead turns a 4.1 % lever into a 0.36 % one on enwik9.

## The experiments

Each is independent: it can be built, measured and accepted or rejected on its
own, against the locked floors. Complexity is XS to XL, never time.

| # | Experiment | Expected | Ratio risk | Cost |
|---|---|---|---|---|
| **A1** | Segment size from input length and worker count | 3–3.5× wall on enwik9 | ~0.4 % | XS |
| **A2** | Worker cap from measured memory bandwidth | protects A1's scaling | 0 | S |
| **A3** | Workers on distinct physical cores, `SCHED_BATCH` verifier | host impact | 0 | XS |
| **A4** | Budget per worker vs. total budget, re-measured under A1 | picks the RSS policy | measured | S |
| **A5** | Decode-side segment sizing to match A1 | 3× decode | 0 | XS |
| **B1** | Two first-layer arbiters instead of four | 1.25–1.4× | small | M |
| **B2** | A marginal price table for all twenty context slots | 1.1–1.3× | ~0 | M |
| **B3** | Line-aligned arbiter and estimation strides | 1.01–1.03× | 0 (layout) | XS |
| **B4** | int8 lane inputs, narrower context cells | 1.1–1.2× | small | L |
| **B5** | Update skipping on saturated cells | 1.1–1.25× | small | M |
| **B6** | Arbiter weights refreshed per nibble, not per bit | 1.2–1.4× | measured | L |
| **B7** | Profile-guided layout of the per-bit path | 1.0–1.05× | 0 | S |
| **C1** | LZP bypass as an input-detected path | 1.4× on text, more elsewhere | gated | M |
| **C2** | Run-length and zero-run transforms ahead of the model | large on records | 0 (reversible) | M |
| **C3** | Fixed-stride delta detection | large on tables | 0 (reversible) | M |
| **C4** | Small-alphabet packing | large on 2–16 symbol data | 0 (reversible) | M |
| **C5** | Per-block model choice from a measured cost estimate | 1.2–2× on mixed input | ~0 | M |
| **D1** | A `--high` rung between `--fast` and `--max` | 2–3× | 1–3 %, its own lock | L |

### Group A — the threefold that already exists

**A1. Derive the segment size from the input length and the worker count.**
`ADAPT_SEGMENT_BYTES` is a fixed 16 MiB (`adapt.h:84`), so the number of model
restarts grows with the file. Replace it, for a named file whose length is
known, with `max(ceil(length / workers), floor)` rounded to whole blocks, and
keep the fixed size only for a stream of unknown length, where `RUN-2` leaves
no choice. Measure: enwik8 and enwik9 at 1, 2, 4 and 6 workers under both
policies — container bytes against the locked floors, wall and CPU seconds,
and `cmp` on the round trip. The prediction to falsify is 0.36 % on enwik9 at
six workers. `--segment-bytes` already exists on the command line, so the
experiment needs no plumbing before it can be run. Risk: peak RSS is
`workers × budget`, which A4 prices; a reader of an old container is
unaffected, because segment boundaries are already in the format.
*Origin:* `kanzi`'s block-parallel design (its README), repriced here.

**A2. Cap workers by measured memory bandwidth rather than by `nproc`.**
The earlier audit measured six concurrent encoders reaching 3.46× with
per-process IPC falling from 2.27 to 1.39 at unchanged instruction and miss
counts, which is the memory system saturating rather than the cores filling.
A two-second calibration at start-up, sizing the cap at
`min(cores, bandwidth ÷ per-worker traffic)`, keeps A1 from spending cores
that return nothing. Measure: aggregate MB/s and per-worker IPC at 1 through
`nproc` workers, with and without the cap.
*Origin:* this tree's `docs/performance-opportunities-2026-09-14.md`, item 1.

**A3. Pin workers to distinct physical cores; run the verifier at `SCHED_BATCH`.**
SMT siblings share L1, L2 and the front end, and this codec misses L1 78 times
a bit. Expose as `--pin`, off by default, Linux-only. Measure: six workers
pinned to six cores against three sibling pairs against unpinned.
*Origin:* the same audit, item 3.

**A4. Re-price the budget policy under A1.** The audit measured 7.0 % of ratio
for six workers sharing one budget against 4.1 % for six workers each holding
the default — but both figures carried the restart cost that A1 removes. The
two-way split measured above cost 13 bytes, and that says nothing about
enwik9: a 16 MiB input does not fill the 674 MiB default, so the budget was
never binding. On enwik9 it binds, and the budget term is then the only term
A1 leaves. Measure: enwik9 at six workers with the budget whole per worker,
halved and quartered; bytes and peak RSS against the locked floor.

**A5. Give the decoder A1's segment sizing.** Parallel segment decoding
landed already (audit item 14); it inherits whatever the encoder wrote, so A1
carries the decode speedup with it. Measure: decode wall for the A1 containers
at 1, 2, 4 and 6 threads, byte-exact against the source.

### Group B — the per-bit path, at identical or near-identical bytes

**B1. Two first-layer arbiters instead of four.** The max profile runs four
72-lane first-layer arbiters plus a final one; `predict_max_v3` and
`update_max_v3` together hold 78 % of cycles, and the four dot products with
their four trains are the bulk of it. `zmix` measured mixer quality as worth
about one byte over a 10⁹-byte file, which says the marginal arbiter is
buying almost nothing. Against that, this tree's own `MIX34_LOG` comment
records the third and fourth arbiters as worth 1.5 % on text — so the two
findings disagree and the experiment is to settle which holds here. Measure:
arbiter sets of 2, 3, 4 and 5 on the 8 MiB and 16 MiB fixtures and on the
mixed corpus; bytes against the floors, instructions per bit, cycles.
*Origin:* `zmix`'s `docs/ALGORITHMIC-IDEAS.md` §6.

**B2. Build a marginal price table for all twenty context slots.** `zmix`
deletes slots at compile time — code, tables and mixer inputs together —
rather than skipping them at run time, choosing the set from a measured
per-slot loss. This tree has the same lever in `--contexts lean`, which was
locked at −0.09 % of bytes for −3.4 % of instructions, but the set was chosen
by hand rather than from a price table. Measure: for each of the twenty
slots, encode with that slot alone removed; record bytes and instructions per
bit on the 8 MiB fixture and on six mixed-corpus rows; publish the Pareto
front and lock the set that dominates. This is the disciplined way to find
the next ten percent, and it costs only machine time.
*Origin:* `zmix`'s compile-time slot masking (its §5).

**B3. Pad the arbiter and estimation strides to whole cache lines.**
`ARBITER_STRIDE` packs a row at 292 B, which covers six lines in 55 % of
positions and five in the rest. Padding to 320 B with the row 64-byte aligned
makes every row a whole number of lines. Bytes-identical by construction;
weight tables grow about 10 %. Measure: `cache-misses:u` per bit, interleaved.
*Origin:* this tree's audit, item 5; not yet run.

**B4. Narrow the lanes to int8 and the context cells to match.** int16 arbiter
weights already landed with the second max model, worth 4.9 % of instructions
and halving those tables. `zmix` reports its int8 attention cache as costing
**zero** bytes, because the layer was memory-bound rather than precision-bound.
The analogue here is the first-layer *inputs*: a stretched probability that
saturates at ±2047 does not need sixteen bits to feed a dot product, and
`vpmaddubsw` takes twice the lanes per instruction that `vpmaddwd` does.
Measure: bytes against the floors first — this changes them — then
instructions per bit and LLC misses. Reject unless bytes are equal or smaller.
*Origin:* `zmix` §5, and this tree's own int16 result.

**B5. Skip the update where the cell is saturated.** The mixer already has
`MIXER_SKIP`; the context maps do not. A cell whose count is at its cap and
whose probability moved by less than one quantisation step over its last
several updates is learning nothing, and the write-back is a dirty line that
the next bit will miss on. Gate the write, not the read. Bytes change, so the
floors decide. Measure: bytes, instructions per bit, LLC misses, and the
fraction of updates skipped.
*Origin:* the standard adaptive-update-skipping idea in the PAQ/LPAQ
literature (Mahoney, *Data Compression Explained*, the state-map section).

**B6. Refresh the arbiter weights once per nibble rather than once per bit.**
Eight predict/update rounds a byte each retrain five arbiters. Training four
times a byte instead of eight halves the train cost while the lane values
still move every bit. The model's accuracy loss is bounded by how much a
weight moves in one bit, which is small at the locked learning rates. This
changes the bytes and needs its own version number if it lands. Measure:
bytes on both locked fixtures and the mixed corpus against instructions per
bit; accept only at equal or smaller bytes, otherwise it belongs to D1.

**B7. Profile-guided layout of the per-bit path.** PGO was measured as noise
by the earlier audit, but that was before the byte-level plan landed and the
branch-miss count fell from 15 to 6.6 a bit. Re-run it: bytes-identical by
construction, so the only question is whether it still measures as noise.

### Group C — input-detected front ends, for `DLV-3`

These are the `kanzi` and `mcm` ideas that the two measurements above rule out
for text and leave open everywhere else. Each is reversible and carries its
own detector, so a file it does not match passes through unchanged (`FMT-8`),
and each is accepted on the mixed corpus, never on enwik8.

**C1. LZP as an input-detected bypass.** Priced above at a 1.4× ceiling on
text with an unusable error rate — but never measured on the mixed corpus,
where `mcm` claims it. Run `bench/redundancy_probe.c` over every mixed-corpus
row first; build it only for the rows where coverage exceeds 80 % at an error
rate under 2 %. That measurement is the cheapest experiment on this list and
it should be run before any of the C group is built.
*Origin:* Bloom, DCC 1996; `mcm`'s README.

**C2. Run-length and zero-run transforms ahead of the model.** `kanzi` carries
RLT and ZRLT as separate transforms. Long byte runs and long zero runs cost
this codec eight modelled bits each, and a run memory that predicts them well
still pays the full per-bit path. Measure on the mixed corpus's binaries and
records.

**C3. Fixed-stride delta detection.** Tables, sensor logs and record files
repeat a period; subtracting the value one period back turns a strong
correlation into a near-zero residual the model codes cheaply. Detect the
stride from the first block by autocorrelation, carry it in the header.

**C4. Small-alphabet packing.** Data over two to sixteen distinct symbols
still costs eight coded bits a byte. Packing to the alphabet's width before
the model is a direct multiple on such files. Detect from the first block's
symbol census.

**C5. Choose the model per block from a measured cost estimate.** `mcm` names
"binary + text detection for model switching" as one of its three sources of
speed; this codec chooses its context set per *file* (`--contexts auto`) and
its profile per *invocation*. A per-block choice, made from the coded cost of
the previous block, would let a mixed archive spend the max model where it
pays and a lean one where it does not. Measure on a concatenation of corpus
rows, against encoding each row separately.

### Group D — the product answer

**D1. A `--high` rung between `--fast` and `--max`.** This is what `mcm`
actually sells, and its ladder shows why: a rung at +1.3 % of bytes for −23 %
of time is a rung many callers want. The B-group experiments that fail their
floors by a small margin are not waste — they are this rung's content. Land
it as its own profile with its own format version and its own locked floors,
so `DLV-1` holds and the `--max` floors are untouched. The decision it needs
from the operator is the one number this note cannot supply: what ratio the
rung is allowed to give up.

## Position

Run A1 first: it is XS, `--segment-bytes` already exists, and it is the only
experiment on this list that reaches threefold on its own. Run C1's probe
sweep over the mixed corpus beside it, because it is an afternoon of machine
time and it decides whether the whole C group is worth building. Then take
B2's price table, because it makes every later B experiment cheaper to judge.

The honest limit: **threefold on one core at strictly zero ratio cost is not
available from anything these three codebases do.** Group B's plausible
product is 1.5× to 2×, and every factor in it must clear a floor that only
moves downward. Threefold in wall-clock at a ratio cost under half a percent
*is* available, today, on inputs large enough to segment — and that is what
A1 buys. If the operator wants threefold on one core, the answer is D1: a new
rung, with its own floors, and a stated budget of bytes to spend.
