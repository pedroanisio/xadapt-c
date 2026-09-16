<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-16, from the
operator's question of that day: what three reference codecs suggest for a
threefold speedup at the locked ratio and the locked memory. Every number
below is from a run on the host named under Measurements, or is quoted from
the named reference's own README. -->
# Three times the speed at the locked ratio — what the reference codecs are worth

Part of [adapt](../README.md).

Deleting the context slots a file does not need is the only one of these
codecs' speed techniques that pays here, and it pays **per file, not as a
fixed set**: the six kinds the enwik8 price table names return 1.25× to 1.27×
on text, executables and database records alike, for between 0.13 % and 1.47 %
of ratio — and 5.38 % on XML, which is what rules out shipping them as a
constant. The other four techniques were built or configured and measured in
the same session, and all four are refuted: the two front-end bypasses on
text, deriving the segment size from the input length at fixed memory, and
padding the arbiter rows to whole cache lines. Threefold at the locked ratio
*and* the locked memory is not among them; the slot lever saturates at 1.43×
even with every context gone.

Every number below is from a run; the experiments still unbuilt say so.

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

On this fixture the cost is per restart — 52 to 80 KB, below the 105 KB
plateau only because a restart inside a 16 MiB file lands on a model that was
never fully warm — and fitting the memory sizes to two threads cost 13 bytes,
because a 16 MiB input never fills the 674 MiB default. Both of those stop
holding on a fixture that does fill it.

### On a full fixture the budget split dominates, and larger segments lose

Run on the whole of enwik8, where the budget binds, the picture inverts. The
single-threaded row reproduces the locked enwik8 floor byte for byte, so this
container's encoder is the locked one:

| Configuration | Segments | Container | vs 1 thread | Wall | Speedup |
|---|---|---|---|---|---|
| `--threads 1` | 1 | 17,902,645 B | — | 149.4 s | 1.00× |
| `--threads 2 --segment-bytes 48MiB` | 2 | 18,516,036 B | +3.43 % | 150.8 s | 0.99× |
| `--threads 4 --segment-bytes 16MiB` | 6 | 18,966,492 B | +5.94 % | 70.9 s | 2.11× |
| `--threads 4 --segment-bytes 25MiB` | 4 | 19,001,839 B | +6.14 % | 64.8 s | 2.31× |

**Enlarging the segment made the container bigger, not smaller.** Going from
16 MiB to 25 MiB segments removed two of the five model restarts — worth about
210 KB by the plateau above — and still cost 35,347 bytes more. Two mechanisms
explain it, and both follow from the same cap. Each worker reserves an output
buffer bounded by its whole *uncompressed* segment
(`adapt_segment_out_bound`, `adapt_encode.c:349`) and its own model, so
`threads × (model + segment)` must fit one budget: a larger segment buys fewer
restarts by taking memory away from every worker's model. At 48 MiB the cap
bites hardest — only one worker fits the 674 MiB budget, so two threads ran
serially for no speedup at all and still paid a restart.

Segment size and worker count therefore compete for one budget, and
**the experiment this note ranked first is refuted at fixed memory**: deriving
the segment size from the input length does not turn the audit's 4.1 % into
0.36 %, because the restart term it removes is the smaller of the two terms.
The audit's 4.1 % stands, and 3.4 % to 6.1 % is what parallel encoding costs
on enwik8 with the memory held at the locked tier.

What parallelism buys is 2.1× to 2.3× of wall time for 6 % of ratio, or the
locked ratio at four times the memory. It does not buy a threefold speedup at
the locked ratio *and* the locked memory, which is what the question asked.
That leaves group B as the only route to the question as posed.

### Line-aligned arbiter rows are 1 % slower, not 1–3 % faster

The audit's item 5 predicted 1–3 % from padding each arbiter row to a whole
number of cache lines. It was built: `ARBITER_STRIDE16` and `ARBITER_STRIDE32`
rounding the row to 32 int16 or 16 int32, every call site and both
initialisers converted, the padding never read because the dot products and
the trains run over `LANES` and the update count sits at `LANES`. The max
profile's row went from 73 int16 — four lines in 47 of 64 starting positions,
three in the rest, mean 3.73 — to 96, three lines always.

The container was identical (1,681,725 B on the 8 MiB fixture, round trip
byte-exact), so the change was purely a layout change, and it was **slower in
every interleaved pair**: 15.33/15.41, 15.07/15.27, 15.06/15.13 seconds, HEAD
against padded, a median 15.07 against 15.27. The row shrank in lines and the
tables grew 31 % — 7.9 MB more, two of them 19 MiB to begin with — and the
third and fourth arbiters' tables are far larger than any cache, so the extra
bytes cost more misses than the alignment saved. The change was reverted.

That closes the item for the int16 model the max profile runs; the audit's
estimate was made against the int32 layout it replaced, where the row was
292 B and the tables half as many. The general lesson is in the next result:
on this codec, what a table costs is its footprint, not its alignment.

### Deleting the context slots a file does not need pays, and its curve has a knee

Each of the max profile's twenty hashed contexts costs three arbiter lanes of
every coded bit and one claim of every nibble. `bench/slot_price.py` prices
each one by encoding with it dropped. On the first 8 MiB of enwik8, on top of
the locked lean set (whose four absent kinds price at exactly zero, which is
the harness's own control), the whole table is
[slot-price.json](slot-price.json) and its live kinds are:

| Kind | Worth | | Kind | Worth |
|---|---|---|---|---|
| nest | 9,198 B (0.547 %) | | o1 | 2,263 B (0.135 %) |
| class | 6,910 B (0.411 %) | | record | 1,631 B (0.097 %) |
| wordlc | 5,943 B (0.353 %) | | nibbles | 1,559 B (0.093 %) |
| o4 | 4,400 B (0.262 %) | | o5 | 1,537 B (0.091 %) |
| o3 | 4,102 B (0.244 %) | | indirect | 1,375 B (0.082 %) |
| sparse2 | 2,873 B (0.171 %) | | word2 | 1,280 B (0.076 %) |
| o2 | 2,471 B (0.147 %) | | sparse | 1,075 B (0.064 %) |
| o6 | 2,458 B (0.146 %) | | | |

No single kind is worth more than 0.55 %, and no single deletion is
measurable in time — three lanes of seventy-two is inside this host's noise.
The lever is the group. Dropping the kinds cheapest first, cumulatively:

| Kinds out | Container | vs baseline | Seconds | Speedup |
|---|---|---|---|---|
| none | 1,681,725 B | — | 14.79 | 1.00× |
| 3 | 1,685,413 B | +0.219 % | 14.23 | 1.04× |
| 4 | 1,686,851 B | +0.305 % | 13.56 | 1.09× |
| **5** | **1,689,262 B** | **+0.448 %** | 12.33 | **1.20×** |
| **6** | **1,692,951 B** | **+0.667 %** | 11.94 | **1.24×** |
| 8 | 1,699,385 B | +1.050 % | 11.59 | 1.28× |
| 11 | 1,720,920 B | +2.331 % | 10.77 | 1.37× |
| 13 | 1,824,140 B | +8.468 % | 10.35 | **1.43×** |
| 15 | 1,933,065 B | +14.945 % | 10.73 | 1.38× |

Three things follow. The group costs about what its parts cost — six kinds
priced at 0.503 % apart cost 0.667 % together — so the price table predicts
the group, which is what makes it a design tool rather than a curiosity. The
curve has a knee at five or six kinds, and past eight the exchange rate
collapses: the last five kinds buy 0.15× for 13 % of ratio. And the lever
saturates at **1.43×** with every context slot gone, so the remaining 70 % of
the time is the coder, the claims, the estimation stages, the runs and recall
— not the context ensemble.

At the knee this beats the published exchange rate of the reference codec:
`mcm` gives up 1.3 % of its ratio for 23 % of its time, and six kinds out
gives up 0.67 % for 19 %. It is also the only lever measured in this session
that pays at the locked memory — the tables it removes make the model
*smaller*, not larger.

### The set that pays on text costs 5 % on XML: it has to be chosen per file

A rung is accepted on the mixed corpus, never on enwik8 alone (`DLV-3`), so
the six kinds the enwik8 table named were applied to three Silesia rows that
did not choose them — `mozilla` (executables), `osdb` (database records) and
`xml` (structured markup), the first 4 MiB of each:

| Row | Baseline | Six kinds out | Ratio | Seconds | Speedup |
|---|---|---|---|---|---|
| enwik8, 8 MiB | 1,681,725 B | 1,692,951 B | +0.668 % | 15.02 → 11.84 | 1.27× |
| mozilla, 4 MiB | 3,192,182 B | 3,196,305 B | **+0.129 %** | 10.72 → 8.43 | 1.27× |
| osdb, 4 MiB | 924,430 B | 937,986 B | +1.466 % | 11.08 → 8.88 | 1.25× |
| xml, 4 MiB | 187,919 B | 198,020 B | **+5.375 %** | 8.03 → 7.23 | 1.11× |

**The speedup is nearly constant and the price is not.** Three of four rows
return 1.25× to 1.27×, because the work removed is the same three lanes per
kind per bit whatever the input; the ratio cost ranges over a factor of forty,
from 0.129 % on executables to 5.375 % on XML. Each row's own price table
explains it — `nibbles` and `o5` are worth 0.02 % and 0.09 % on enwik8 and
0.96 % and 1.91 % on XML — and each row's own cheapest-first frontier reaches
1.2× at its own price: +0.454 % on mozilla, +1.018 % on osdb, +14.528 % on
xml. The kinds a file does not need are a property of the file.

A fixed leaner set is therefore not a rung this codec can ship: it would break
the XML row by 5 % to save a tenth of the time. What the measurement supports
is the shape the codec already has — `--contexts auto` picks one of eight sets
from the first block — with a wider table of sets and a rule that picks from
it. `bench/slot_price.py` is what builds that table; this session built the
tool and four of its rows.

## The experiments

Each is independent: it can be built, measured and accepted or rejected on its
own, against the locked floors. Complexity is XS to XL, never time.

| # | Experiment | Status | Measured or expected | Ratio | Cost |
|---|---|---|---|---|---|
| **A1** | Segment size from input length and worker count | **refuted** | slower *and* bigger at fixed memory | +6.1 % | XS |
| **A2** | Worker cap from measured memory bandwidth | open | protects whatever A-group work survives | 0 | S |
| **A3** | Workers on distinct physical cores, `SCHED_BATCH` verifier | open | host impact | 0 | XS |
| **A4** | Budget per worker vs. total budget | **answered by A1** | the split is the dominant parallel cost | — | S |
| **A5** | Decode-side segment sizing | **moot** | A1 is refuted | — | XS |
| **B1** | Two first-layer arbiters instead of four | open | 1.25–1.4× expected | small | M |
| **B2** | A marginal price table for all twenty context slots | **run; pays** | **1.24× at +0.67 %; 1.43× ceiling** | see the curve | M |
| **B3** | Line-aligned arbiter and estimation strides | **refuted** | 1 % slower, +7.9 MB | 0 (bytes identical) | XS |
| **B4** | int8 lane inputs, narrower context cells | open | 1.1–1.2× expected | small | L |
| **B5** | Update skipping on saturated cells | open | 1.1–1.25× expected | small | M |
| **B6** | Arbiter weights refreshed per nibble, not per bit | open | 1.2–1.4× expected | measured | L |
| **B7** | Profile-guided layout of the per-bit path | open | 1.0–1.05× expected | 0 | S |
| **C1** | LZP bypass as an input-detected path | **refuted on text** | 1.4× ceiling at a 32 % error rate | gated | M |
| **C2** | Run-length and zero-run transforms ahead of the model | open | large on records | 0 (reversible) | M |
| **C3** | Fixed-stride delta detection | open | large on tables | 0 (reversible) | M |
| **C4** | Small-alphabet packing | open | large on 2–16 symbol data | 0 (reversible) | M |
| **C5** | Per-block model choice from a measured cost estimate | open | 1.2–2× on mixed input | ~0 | M |
| **D1** | A `--high` rung between `--fast` and `--max` | **priced by B2** | 1.20× at +0.45 %, 1.28× at +1.05 % | its own lock | L |

### Group A — parallel encoding, which trades memory for time

**A1. Derive the segment size from the input length and the worker count —
refuted.** The proposal was to replace the fixed 16 MiB `ADAPT_SEGMENT_BYTES`
(`adapt.h:84`) with `max(ceil(length / workers), floor)` for a named file,
on the reasoning that restarts, not parallelism, carried the audit's 4.1 %.
`--segment-bytes` already exists, so it was run rather than built, and the
enwik8 table above is the answer: 25 MiB segments removed two of five restarts
and produced a **larger** container than 16 MiB segments, because each worker
reserves an output buffer bounded by its whole uncompressed segment
(`adapt_segment_out_bound`, `adapt_encode.c:349`) and takes that memory from
its own model. At 48 MiB only one worker fits the budget at all. Segment size
and worker count compete for one budget; the restart term A1 attacks is the
smaller of the two. Nothing to land.
*Origin:* `kanzi`'s block-parallel design (its README), repriced and refuted
here.

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

**B2. A marginal price table for all twenty context slots — run, and it
pays.** `zmix` deletes slots at compile time — code, tables and mixer inputs
together — rather than skipping them at run time, choosing the set from a
measured per-slot loss. This tree had the same lever in `--contexts lean`,
locked at −0.09 % of bytes for −3.4 % of instructions, but the set was chosen
by hand. `bench/slot_price.py` now prices every kind and walks them cheapest
first; the table and the curve are above and the data is
[slot-price.json](slot-price.json), which also holds the
`mozilla`, `osdb` and `xml` tables. On enwik8 the knee is at five or six kinds
— 1.20× for 0.448 % or 1.24× for 0.667 % — and the ceiling is 1.43×. The
mixed-corpus rows then settled the shape: the speedup carries (1.25× to 1.27×
on three rows of four) and the price does not (0.129 % to 5.375 %), so the set
belongs to the file, not to the profile. What remains is to price more rows —
CJK, sorted data, already-compressed input — and to derive the sets and the
choice rule D1 will carry.
*Origin:* `zmix`'s compile-time slot masking (its §5).

**B3. Pad the arbiter and estimation strides to whole cache lines —
refuted.** Built, measured bytes-identical, and 1 % slower in every
interleaved pair; see the measurement above. The audit's 1–3 % estimate was
made against the int32 layout that the second max model replaced with int16
rows and twice as many sets, where the footprint cost of padding was half what
it is now. Reverted.
*Origin:* this tree's audit, item 5; run and closed.

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

**D1. A `--high` rung between `--fast` and `--max`, priced by B2's curve.**
This is what `mcm` actually sells, and its ladder shows why: a rung at +1.3 %
of bytes for −23 % of time is a rung many callers want. B2 supplies this
rung's content and its price, measured rather than argued — five kinds out for
1.20× at +0.448 %, six for 1.24× at +0.667 %, eight for 1.28× at +1.050 % —
and the B-group experiments that miss the `--max` floors by a small margin
join it later.

Landing it takes a new format version: `DLV-1` holds a published version's
model fixed, all eight `CONTEXT_SETS` codes are used and every bit of the
sizing byte is spoken for, so the leaner set cannot ride an existing number.
Steps, in order — confirm the knee on the mixed corpus and on enwik9, since
the set that dominates 8 MiB of English Wikipedia is not yet known to dominate
records, binaries or CJK; choose the point; take the next free version pair
(76/77) with its decoder side and its vectors; lock its own floors so the
`--max` floors are untouched; and record the choice in `LOCKS.md`.

The decision it needs from the operator is the one number this note cannot
supply: how much ratio the rung may spend. A version number is permanent —
every container it writes must decode forever — so it is worth spending once,
on a point chosen from the curve above.

## Position

Build D1 as a **wider set table with a per-file choice**, not as one leaner
set. That is the one lever these codebases offer that was measured to pay
here — 1.25× to 1.27× on three of four acceptance rows, at the locked memory,
with the model smaller than the one it replaces — and the fourth row is why it
cannot be a constant: the same six kinds cost 0.129 % on `mozilla` and 5.375 %
on `xml`. The codec already has the mechanism, in the eight `CONTEXT_SETS` a
file chooses from in its first block; what the price tables add is evidence
for which sets belong in that table and a rule for picking one.

It needs a new format version, because `DLV-1` holds a published version's
model fixed and all eight set codes and all eight sizing bits are spoken for.
That version number is a permanent commitment, so the sets it carries should
be chosen from more rows than the four priced here, and the ratio the rung may
spend on any single row is the operator's number, not this note's. The price
tables are here so those choices are made against evidence rather than
guessed.

Take B1 next — two first-layer arbiters instead of four — because B2 bounds
what is left and because, unlike the set choice, it is one decision for every
input. Slot deletion saturates at 1.43× with every context gone, so
roughly 70 % of the per-bit time is outside the context ensemble: the dot
products and trains that B1 halves, the claims, the estimation stages, the
runs and recall. B1 is the only untried experiment whose target is that 70 %,
and `zmix`'s finding that a better mixer is worth about one byte in 10⁹ says
its ratio cost should be small. Everything else in group B is a guess until
someone measures it, and two of this session's four refutations were of
estimates that looked at least as sound.

The honest limit, now measured rather than argued: **threefold at the locked
ratio and the locked memory is not reachable from anything these three
codebases do.** Slot deletion tops out at 1.43× and costs 8.5 % of ratio to
get there. Parallel encoding reaches 2.1–2.3× and costs 6 % of ratio at fixed
memory, or four times the memory at fixed ratio. The front-end bypasses that
make `mcm` fast are worth 1.08× to 1.4× on this text, at error rates that
would cost more ratio than they save. What is genuinely available is a
per-file rung at 1.25×, costing a tenth of a percent on some inputs and a few
percent on others, and a threaded mode for callers who have the memory to
spend. A threefold single-core codec at this ratio is a different codec, not a
tuning of this one.
