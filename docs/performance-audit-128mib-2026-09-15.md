# Performance audit — cadapt `--max` within 128 MiB, toward 17 MB on enwik8

<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-15 local, against
HEAD 510aa16 (the per-bit path of 085dff0, threaded segments of 510aa16). Every
number is from a run on the host in Baseline unless it names its source; corpus
citations name the doc-ray document and the sentence ordinal read. Nothing was
attributed to the corpus that was not retrieved from it. No code was changed
by this audit. The throughput-only ranking at HEAD is a separate document,
docs/performance-opportunities-2026-09-14.md; items shared with it are named,
not repeated. -->

The current bottleneck, for the objective "17 MB on enwik8 within 128 MiB", is
not the per-bit path but **where the 128 MiB goes**: at that budget the model
gives 43 MiB to fixed-size tables the file never needs at full size (two
18.25 MiB arbiter weight tables, a 16.5 MiB second estimation stage) and 16 MiB
to a history window, and only **32 MiB — 28 % of the model — to the context
table that buys ratio**, so the container is 20,091,771 B against 18,564,510 B
at 674 MiB (+8.2 %) with the same code path. The single highest-leverage change
is **sizing the arbiter and estimation tables to the budget and giving the
difference to the context table** (items 1–3): measured on this file, each
doubling of the table from 32 MiB is worth −2.46 %, −2.10 %, −1.85 %, −1.42 %
and then nothing, and the first two items were run as an experiment for this
audit — keys narrowed from 8 to 6 bits (arbiters) and 10 to 8 (second stage),
nothing else — and took 128 MiB from **20,091,771 to 19,831,153 B (−1.30 %)**
with the table grown to 64 MiB by the existing sizer, 8 MB less RSS and no
speed loss. With item 3's 16 MiB as well, 128 MiB lands where ~256 MiB is
today (≈ 19.2–19.4 MB), at the same speed and with no new dependency.

**Feasibility, stated first because it bounds everything below.** The target
is 17,000,000 B: −15.4 % from today's 128 MiB container and −8.4 % from the
codec's *memory-saturated* container (18,563,312 B at 1 GiB, LOCKS.md). Memory
efficiency alone therefore cannot reach it: with every table byte in this
document reclaimed, 128 MiB reaches the 19.2–19.4 MB band, and the rest must
come from modeling gains of ≈ 12 % beyond a model that is already saturated in
memory. For calibration, the LTCB snapshot in `research/ltcb-main-table.csv`
(224 entries) lists no compressor below 19,393,460 B on enwik8 with ≤ 256 MB
and none below 20,318,653 B with ≤ 128 MB; this codec at 256 MiB (19,185,877 B,
measured below) is already past the first figure. Items 10–16 are the modeling
mechanisms with evidence behind them; each is worth a fraction of a percent to
a few percent and each changes bytes, so each is gated by a full-file run under
the floor. The honest expectation is 19.0–19.5 MB within 128 MiB from this
document's "land now" set, and a research programme, not a ranking, for the
last 12 %.

## Baseline

**Hardware.** AMD Ryzen 5 5600X: 6 cores / 12 threads, one CCX; L1d 32 KB and
L2 512 KB per core, L3 32 MB shared; 125 GB RAM; NVMe on ext4; ISA x86-64-v3
(AVX2, FMA, BMI2), no AVX-512; transparent huge pages `madvise` (the codec's
`MADV_HUGEPAGE` is honoured). Linux 7.0.11, perf 7.1.5, IBS unavailable
(`perf_event_paranoid` = 2), so stall attribution below relies on skid.

**Workload.** enwik8 (100,000,000 B, md5 `a1fa5ffd…b36a`, order-0 entropy
≈ 5.1 bits/byte, English XML text), the locked command `encode --max
--allocation context`, one file, one process. Proxies: the first 8 MiB and 32
MiB of enwik8 (the 32 MiB slice takes a 256 MiB table at the default budget and
a 32 MiB one at 128 MiB, so it reproduces the full file's table regime).
Assumption A1: the operator's workload is this file at this command; every
figure is for it unless named otherwise.

**Measurements** (this binary, `cadapt 0.12.0`, sha256 `fb4ff848…4b26c`).

| Budget | Table | Window | Container | Wall | Peak RSS | Model memory |
|---:|---:|---:|---:|---:|---:|---:|
| 128 MiB | 2^21 blocks, 32 MiB | 16 MiB | **20,091,771 B (20.09 %)** | 274.4 s † | 118 MB | ~112 MiB |
| 192 MiB | 2^22, 64 MiB | 32 MiB | 19,597,276 B (−2.46 %) | 296.7 s † | 168 MB | ~160 MiB |
| 256 MiB | 2^23, 128 MiB | 32 MiB | 19,185,877 B (−2.10 %) | 270.0 s † | 233 MB | ~224 MiB |
| 674 MiB (default) | 2^25, 512 MiB | 64 MiB | 18,564,510 B (the floor) | 189.4 s (gate run) | 659 MB | ~640 MiB |
| 1 GiB | 2^26 | — | 18,563,312 B (LOCKS.md) | — | — | — |

† Wall under other agents' jobs (load average 1–4 during these runs); the
budget itself does not cost speed: interleaved on the 8 MiB slice, 128 MiB
and the default budget took 80.1 G and 80.5 G user cycles (1,739,238 vs
1,716,715 B). Treat the three † times as *not measured*; the quiet-host number
for this path is 189 s at the default budget and, from the interleaved result,
the same at 128 MiB. Assumption A2: the 128 MiB target is a budget on the
codec's model plus workspace as `--max-memory` counts it, not on the
process's RSS; RSS exceeds it by ~6 MB (buffers, stack, arena rounding).

**Where the 128 MiB goes** (`adapt_model.h:model_layout`, max profile):

| Table | Bytes | Share | Grows with budget? |
|---|---:|---:|---|
| Third and fourth arbiter weight sets (`weights3`, `weights4`: 65,536 sets × 73 × int32 each) | 36.5 MiB | 33 % | no |
| Context table (2^21 × 16 B) | 32 MiB | 28 % | yes — the only table that buys ratio past 128 MiB |
| History window (`hist`) | 16 MiB | 14 % | yes, but worth 0.25 % per 4× (LOCKS.md) |
| Second estimation stage (`apm2`: 2^18 rows × 33 × uint16) | 16.5 MiB | 15 % | no |
| First estimation stage (`apm`: 2^16 rows) | 4.1 MiB | 4 % | no |
| Recall table (`mtab`, 2^20 × 4 B) | 4 MiB | 4 % | yes |
| First and second arbiters, runs, final, struct | ~4 MiB | 3 % | no |

**Profile at 128 MiB** (32 MiB slice, `perf record` 139,708 samples; `perf
stat`): `predict_max_v3` 44.8 %, `update_max_v3` 34.4 %, `start_byte` 10.6 %
(its claims: 31 % of its samples on the tag loads), `prefetch_byte` 4.1 %,
`end_byte` 3.6 %, coder 1.8 %. 2,477 instructions and 1,374 cycles per coded
bit (IPC 1.80 under load), 6.7 branch misses, 81 L1d misses and 24 L2 misses
per bit. The shape is the one measured at the default budget in
`docs/performance-audit-2026-09-14.md` ("Results"); a smaller table does not
move it.

**Noise floor.** 1.5–2 % on user cycles for interleaved pairs on a quiet host;
several × that on wall time on this shared host. Ratio is exact (`cmp`), so a
bytes claim has no noise floor; a ratio *gain* below 0.05 % (10 KB on enwik8)
is treated as not worth a format version.

**The exchange rate used below.** One doubling of the context table from the
128 MiB point is worth −2.46 % of bytes (2^21 → 2^22, measured), the next
−2.10 %, then −1.85 %, −1.42 %, 0. A reclaimed *fraction* of a doubling is
priced linearly on the log scale: reclaiming 19 MiB at a 32 MiB table is
log2(51/32) = 0.67 doublings ≈ −1.6 %. Every item that moves memory is priced
with this rate and marked (R); every item that changes the model is marked (M)
and needs the full-file gate (`tools/check_ratio_floor.py --verify`, ~6 min)
before its number is anything but an estimate.

## Ranked opportunities

Rank = expected gain ÷ (effort × risk), for the objective in order: ratio at
128 MiB first (it is the gap), throughput second (held at today's rate),
peak RSS third, host impact fourth.

### Land now

**1. Size the third and fourth arbiter tables to the budget.** (R)
- *Current:* `adapt_model.h:model_layout` places `weights3` and `weights4` at
  `MIXER3_SETS`/`MIXER4_SETS` = 65,536 sets × `ARBITER_STRIDE` (73 × 4 B) each,
  36.5 MiB in all, at every budget; `adapt_encode.c:sizes_for_input_profile`
  sizes only the table, window and recall table to the budget.
- *Mechanism:* the sets are keyed by 8 bits of the order-2 hash (or the word
  hash) and the partial byte (`predict_impl`: `wset3 = (h[1] >> 48 & 0xFF) *
  256 + c0`); keying by 6 or 7 bits at small budgets quarters or halves the
  tables (9–18 MiB back). The table bytes go to the context table.
- *Expected effect — measured, with item 2, for this audit:* keys 8 → 6 bits
  here and 10 → 8 bits in item 2 (a scratch build, `_tmp/exp-keys`, no other
  change): the model at 128 MiB fell from ~112 to ~105 MiB with the table
  grown 2^21 → 2^22 by `sizes_for_input_profile` unchanged, and enwik8 went
  **20,091,771 → 19,831,153 B (−1.30 %)**, RSS 118 → 110 MB, wall 211.6 s
  (the 274 s baseline ran under heavier load; not slower). The narrowing's
  own cost, at an unchanged 2^22 table on the 8 MiB slice at the default
  budget: 1,716,715 → 1,724,339 B (**+0.44 %**), which is why the key width
  must follow the budget (8 bits at the default, where the floor stands)
  rather than change outright. Speed: unchanged (fewer weight lines per bit;
  24 L2 misses a bit today, ~15 of them these sets).
- *Cost:* S; a header field (the key width) in a new format version; no
  dependency.
- *Risk:* changes bytes at small budgets (a format bump); the default budget
  keeps 8 bits and its floor.
- *Measurement:* enwik8 at 128 MiB, bytes and the gate; 8 MiB slice at 128
  MiB for the key-width sweep (6, 7, 8 bits).
- *Evidence:* `model_layout`, `predict_impl` (`wset3`, `wset4`); LOCKS.md
  format 19–24 table; C++ HP ord. 1052 (a line is fetched whole — a set is 5
  lines, so fewer sets is fewer lines).

**2. Size the second estimation stage to the budget.** (R)
- *Current:* `APM2_CONTEXTS(max)` = 2^18 rows × 33 × uint16 = 16.5 MiB under
  the max profile, 2^16 (4.1 MiB) under the normal one, regardless of budget
  (`model_layout`, `predict_impl` `ctx2`).
- *Mechanism:* the row is keyed by 10 bits of the order-2 hash and the partial
  byte; 8 bits at ≤ 256 MiB budgets is the normal profile's stage — 12.4 MiB
  back to the table.
- *Expected effect:* measured together with item 1 (above): the pair is
  −1.30 % at 128 MiB and +0.44 % at an unchanged table. The stage's own share
  is not separated; a sweep of its key width alone (8, 9, 10) at 128 MiB is
  the measurement that would apportion it.
- *Cost:* XS (a header field); format bump shared with item 1.
- *Risk:* as item 1.
- *Measurement:* as item 1, with the stage's key width swept (8, 9, 10).
- *Evidence:* `model_layout`; `APM2_CONTEXTS`; the two rows per bit are among
  the hottest loads in `predict_max_v3` (`docs/performance-audit-2026-09-14.md`).

**3. Back the history window with the input (encoder) and the output (decoder).** (R)
- *Current:* `model_layout` allocates `hist` as a 2^window_log ring (16 MiB at
  128 MiB, 64 MiB at the default) that `end_byte` writes every byte; the
  encoder streams from `block` (`adapt_encode.c:encode_blocks`) and the
  decoder writes `out` (`adapt_decode.c` via `decode_bytes`), so both hold the
  bytes the ring duplicates. The recall model reads `hist` at `mptr` and in
  the verify loop of `end_byte`; the record-period contexts read it at
  `n - period`.
- *Mechanism:* point `hist` at the caller's buffer when it covers the reach
  (the encoder's in-memory path and the decoder's output buffer), or keep a
  ring only as long as the recall reach actually used (the recall table holds
  32-bit positions checked against the ring mask, `end_byte`: `age +
  MATCH_VERIFY < hmask + 1`). At 128 MiB the 16 MiB ring goes to the table.
- *Expected effect:* 16 MiB → table 32 → 48 MiB → −1.4 % at 128 MiB; RSS −16
  MB per model. Ratio otherwise unchanged if the reach is kept (bytes identical
  when the window covers the same span); LOCKS.md measured a 4× window at
  0.25 %, so a *shorter* reach costs little where the buffer is shorter.
- *Cost:* M: the ring indexing assumes a power-of-two mask (`hmask`) in
  `end_byte`, `skip_bytes`, `start_byte` (period path) and the transform
  rewrites blocks in place, so the alias must see transformed bytes; the
  streaming CLI path reads 256 KB blocks and has no whole-file buffer — it
  needs the ring or an `mmap` of the input (Gregg ord. 3925–3926: file
  mappings page through the page cache, so an mmap'd input costs no anonymous
  memory).
- *Risk:* an aliasing invariant across four functions; the decoder's "bytes
  written before an error are not to be trusted" contract now also backs the
  model. Shared mechanism with the throughput audit's item 10.
- *Measurement:* peak RSS and bytes on enwik8 at 128 MiB with `--verify`.
- *Evidence:* `model_layout`, `end_byte` (recall), `adapt_encode.c:encode_in_memory`;
  Gregg ord. 3925–3926.

**4. Charge the memory bound for what is measured, not for 8 MiB of headroom.** (R)
- *Current:* `adapt_model.h`: `HEADROOM ((uint64_t)8 << 20)` is added to every
  memory bound ("stack, allocator slack and runtime overhead"); the measured
  overhead at 128 MiB is 118.2 MB RSS − 112 MiB model = ~6 MB including the
  block buffers, verifier slots and the arena's 2 MiB alignment slack
  (`model_arena`: `len = bytes + ARENA_ALIGN`).
- *Mechanism:* count the arena alignment, the block buffers (`ADAPT_BLOCK_BYTES`
  × 3 + `framed_cap`) and the verifier queue explicitly, and set the residual
  headroom to the measured 1–2 MiB; the freed 5–6 MiB reaches the table only
  when it tips a power of two, so pair it with items 1–3 (their reclaimed
  bytes plus this cross the 64 MiB boundary sooner).
- *Expected effect:* 0 alone at most budgets (the table is a power of two);
  with items 1–3 it moves the boundary by 5 %. Listed because the accounting
  is what decides every other (R) item's rounding.
- *Cost:* XS. *Risk:* a host with a larger per-thread stack or allocator slack
  overruns a strict budget; keep 2 MiB, not 0.
- *Measurement:* `maxrss` vs `--max-memory` across budgets, and the table_log
  chosen at 128 MiB before/after.
- *Evidence:* `HEADROOM`, `model_arena`, `adapt_encode_memory_bound`
  (`adapt_encode.c`); C++ HP ord. 2130–2134 (an arena sized once for a known
  lifetime reclaims in one step — the model is such an arena).

**5. Partition the context table by context, sized by measured utility.** (R, M)
- *Current:* `claim` hashes every context into one table and evicts the way
  whose first node has seen the least (`m->seen[b[1]]`), so at 32 MiB the
  twenty contexts compete for 2^21 blocks with no regard to what each is
  worth; orders 5, 6, 8 and the three-word contexts miss on most bytes and
  evict blocks the order-2/3/word contexts would have hit.
- *Mechanism:* a per-context block quota in the hash (a context's hash masked
  into its own region, region sizes chosen per profile and budget from a
  measured table: bytes saved per block for each kind on the first block's
  trial, `adapt_encode.c:choose_model`). At small tables the high-order
  contexts get a small region (they are worth little when they cannot
  retain), the low orders keep theirs. This is the "adapts to the RAM it has"
  mechanism the operator asked for: today only the table's *size* adapts, not
  its *use*.
- *Expected effect:* the difference between a 32 MiB table used by 20 contexts
  and one used well: bounded above by the 2^21 → 2^23 gain (−4.5 %) and
  plausibly a third of it, −1 to −1.5 % at 128 MiB, ≈ 0 at 512 MiB where the
  table saturates. Sayood on context length (ord. 3566–3569): performance
  rises sharply with the maximum context length "until some value", then
  drops "as the maximum length is further increased" — with a small table the
  turnover comes sooner, which is what a per-order quota tracks.
- *Cost:* M (the hash-to-region map, its header encoding, the trial that sets
  it); a format version.
- *Risk:* a poorly chosen partition is worse than the shared table; the
  chooser's sample (32 KiB, `CLASS_SAMPLE`) may not represent the file. Bytes
  change at every budget where a partition is chosen.
- *Measurement:* enwik8 at 128, 192, 256 MiB; the 8-file corpus for the
  non-text cases.
- *Evidence:* `claim`, `claim_chain`, `choose_model`; Sayood ord. 3566–3569,
  3440–3442 (the number of contexts grows exponentially with the length; PPM
  keeps only those that occur).

**6. Branch-free run and recall lanes.** (throughput; shared with the other
audit's item 2, kept here because it is the one bytes-identical single-thread
item left with a measured cost: 6.7 mispredictions a bit at 128 MiB, the
runs' and recall's prefix tests among them.) Mechanism, cost, risk and
measurement as stated there; not repeated.

### Land after measurement

**7. 16-bit arbiter weights with the sign-extending packed multiply-add.** (R, M)
- *Current:* `adapt_model.h:dot4_v3` multiplies int32 weights by int32 inputs
  into int64 accumulators (`_mm256_mul_epi32` on even and odd lanes); `train`
  updates int32 weights; a set is 73 × 4 B = 5 cache lines.
- *Mechanism:* int16 weights and inputs (the inputs already fit: |x| ≤ 2047,
  bias 256, recall ±2048) with `vpmaddwd` pairs summed into int32 then widened
  — 16 lanes a vector, half the bytes per set (3 lines), and the two large
  tables halve (18.25 MiB → 9.1 MiB each) *without* narrowing their key. This
  is the throughput audit's item 8 priced for memory: it is also 18 MiB back to
  the table.
- *Expected effect:* 18 MiB → −1.5 % at 128 MiB by the exchange rate, plus the
  throughput audit's estimate for the dot and train; minus rounding loss
  (weights saturate at ±32767 — learning on long inputs changes; unmeasured).
- *Cost:* M plus a format version and vector regeneration.
- *Risk:* the rounding changes bytes everywhere, so the default-budget floor
  must be re-run (enwik8 6 min; enwik9 52 min + verify) before it is a gain at
  any budget. Corpus note: doc-ray holds no text on packed multiply-add; the
  evidence is the codebase.
- *Measurement:* bytes at 128 MiB and at the default; instructions per bit.
- *Evidence:* `dot4_v3`, `train`, `model_layout`; C++ HP ord. 4234–4240 (a
  loop vectorizes when it reads several values and applies one instruction to
  them at once).

**8. Direct-indexed order-1 and order-2 memories under the max profile.** (R)
- *Current:* orders 1 and 2 are hashed into the shared table like every other
  kind (`start_byte`: `hk[H_O1]`, `hk[H_O2]`), taking two of twenty claims a
  nibble and two blocks' worth of table per context per nibble; the fast
  profile already keeps a direct order-1 memory (`m->o1[O1_BLOCKS]`, 128 KiB).
- *Mechanism:* a direct 2^16 × 32-block order-2 memory (32 MiB — too much at
  128 MiB) is the wrong size; a direct order-1 (128 KiB) and an order-2 memory
  keyed by the hash but in its own small region (item 5's partition with two
  fixed regions) remove two claims from the shared table and never evict them.
  Shared mechanism with the throughput audit's item 7; here the value is table
  room and never-evicted low orders.
- *Expected effect:* two contexts out of the shared table's competition
  (10 % of claims) and two lines fewer per nibble; ratio −0.2 to −0.5 % at
  128 MiB (upper bound: what item 5 would give those two kinds). Speed: fewer
  claims and no eviction path for them.
- *Cost:* S–M; a format version.
- *Risk:* a direct order-1 costs 128 KiB of L2 per nibble set; negligible.
- *Measurement:* enwik8 at 128 MiB; claims per byte (`perf annotate claim`).
- *Evidence:* `start_byte`, `o1_blk`, `O1_BLOCKS`; Sayood ord. 3440–3442.

**9. Retain the shorter window's reach with a two-level recall table.** (R)
- *Current:* `mtab` holds 32-bit positions at `recall_key(c8)`; a candidate
  older than the window is rejected (`end_byte`: `age + MATCH_VERIFY < hmask +
  1`), so at 128 MiB (16 MiB window) recall reaches 16 MiB back and the 4 MiB
  table is sized for a longer reach than the window allows.
- *Mechanism:* size `mtab` to the window's reach (2^19 entries at 16 MiB, 2 MiB
  back) — or, after item 3, let the reach be the whole file and keep `mtab` at
  2^20. Either way the table's bytes follow the window rather than the budget.
- *Expected effect:* 2 MiB → 0.1 % (rate); listed because it is free and
  pairs with item 3.
- *Cost:* XS. *Risk:* none (encoder policy; the recall length is in the format,
  the table size in the header).
- *Measurement:* bytes at 128 MiB. *Evidence:* `end_byte`, `recall_key`,
  `sizes_for_input_profile`.

**10. A third estimation stage keyed by the recall state.** (M)
- *Current:* two estimation stages, keyed by the previous byte and by an
  order-2 hash, each with the partial byte (`predict_impl`: `ctx`, `ctx2`),
  averaged and mixed 3:1 with the arbiters' verdict. The recall model's state
  (`mlen`, `mmiss`, expected bit) enters the arbiters as two inputs and keys
  the first arbiter's set (`mstate`) but never an estimation row.
- *Mechanism:* a 2^15-row stage keyed by (match length bucket, expected bit,
  partial byte): 2 MiB, one row load and one interpolation per bit. Sayood on
  deterministic contexts (ord. 3571–3576): "long contexts that give only a
  single prediction are seldom followed by a new symbol" — a long match is
  such a context, and a stage that learns *how* seldom, per length, is the
  binary-CM form of ppm*'s "small but significant" improvement.
- *Expected effect:* −0.3 to −0.6 % (a stage keyed on the strongest single
  predictor; bounded by the recall inputs' share of the arbiters' gain,
  unmeasured here).
- *Cost:* S; a format version; +2 MiB (counted against items 1–3's gains).
- *Risk:* bytes change; a stage that over-trusts long matches on non-text
  loses on the binary corpus rows — measure the 8-file corpus.
- *Measurement:* enwik8 at 128 MiB and default; the corpus.
- *Evidence:* `predict_impl` (recall inputs, `mstate`), `update_impl` (APM
  updates); Sayood ord. 3571–3576.

**11. Mixer selection by the highest order that hit.** (M)
- *Current:* the first arbiter's set is chosen by (recall state, three bits of
  the previous byte, partial byte); the second by (recall state, run state,
  previous byte); the third and fourth by hashes. None is keyed by *which*
  contexts found their block this nibble (`claim` returns a hit or an
  evicted block; the hit/miss is known at `claim_chain` and discarded).
- *Mechanism:* a 5-bit summary of the claims — the highest order among
  (1,2,3,4,5,6,8) that hit, and whether the word context hit — as the set
  key of one arbiter (replacing the previous byte's three bits, or as a fifth
  arbiter of 32 × 256 sets, 2.3 MiB). Sayood's exclusion principle (ord.
  3578–3582, 3589): when a longer context has the symbol, the shorter ones'
  mass is wasted; a mixer that knows the longest hit can learn to exclude —
  "small but significant savings … cumulatively significant".
- *Expected effect:* −0.5 to −1 % at every budget, larger at 128 MiB where
  misses are more frequent and more informative.
- *Cost:* S (the summary is a by-product of `claim_chain`); a format version.
- *Risk:* bytes change; a fifth arbiter is 72 more multiplies a bit (~3 % of
  cycles) — prefer re-keying the first.
- *Measurement:* enwik8 at 128 MiB and default; instructions per bit.
- *Evidence:* `claim`, `claim_chain`, `predict_impl` (`wset`); Sayood ord.
  3578–3582, 3589.

**12. An evidence input per context: the map's count, not only its probability.** (M)
- *Current:* a context feeds the arbiters its map's stretched probability, the
  probability in the linear domain and the never-contradicted lane
  (`context_lanes`); the map cell's 8-bit count (`map_update`: `n`) — how many
  times this state has been learned from — is used only to set the learning
  rate.
- *Mechanism:* fold the count into the lane word's sign or into the third
  lane's magnitude (a state seen once and a state seen 200 times both give
  ±p/4 today), so the arbiter can weight a context by its evidence. Sayood on
  escape estimation (ord. 3556–3564): Method C, which "on average seems to
  provide the best performance", sets the escape mass by the number of
  distinct symbols seen in the context — evidence, not just frequency; the
  binary-CM form is a confidence input.
- *Expected effect:* −0.2 to −0.5 %; no memory.
- *Cost:* XS in code (the count is in the cell word the vector path already
  loads); a format version.
- *Risk:* bytes change; the mode bits in the cell are the count's spare bits
  (`CELL_MODE_SHIFT`), so the count's width is capped at 8 bits.
- *Measurement:* enwik8 at 128 MiB and default.
- *Evidence:* `context_lanes`, `map_update`, `CELL_COUNT_MASK`; Sayood ord.
  3556–3564.

**13. Per-file learning-rate and estimation-rate choice in the first-block trial.** (M)
- *Current:* `adapt_encode.c:choose_model` codes the first 32 KiB under each
  class width and context set and keeps the best (`trial`, `CLASS_SAMPLE`,
  `SET_MARGIN`); the arbiters' rate schedule (`MIXER_RATE_MIN/MAX`,
  `MIXER_DECAY`), the map rate floor (`MAP_LIMIT`) and the estimation rate
  (`APM_RATE`) are constants.
- *Mechanism:* add two header codes chosen by the same trial: an arbiter rate
  floor (8 or 16) and an estimation rate (6 or 7). The Handbook on adaptive
  arithmetic coding (ord. 2913) names adaptation after every symbol as the
  cost that cannot be avoided when the model must be current; what *can* be
  chosen is how fast it adapts, and a file's stationarity is knowable from its
  first block.
- *Expected effect:* −0.1 to −0.4 % on text (the format 19–24 work found the
  floor 8 vs 20 worth part of its gain; a per-file choice recovers what one
  constant loses on other files); no memory.
- *Cost:* S; two header bits; the trial codes 32 KiB two more times (+0.3 s
  per file).
- *Risk:* bytes change; the trial's 32 KiB may not represent the file (the
  same risk the existing chooser carries).
- *Measurement:* the 8-file corpus and enwik8 at 128 MiB.
- *Evidence:* `choose_model`, `trial`, `mixer_rate` init in `model_new`;
  Handbook ord. 2913.

**14. Probability-domain blending for the two estimation stages.** (M)
- *Current:* the two stages' outputs are averaged (`papm = (papm + papm2 + 1)
  >> 1`) and then mixed 3:1 with the arbiters' verdict by a fixed rule
  (`p = (pfinal + 3 * papm + 2) >> 2`, `predict_impl`).
- *Mechanism:* replace the fixed average and 3:1 rule with a learned blend
  (a 3-input arbiter keyed by the partial byte: the two stages and the
  verdict, in the stretched domain) — the Handbook's survey of blending
  (ord. 4783–4823): performance-based blending penalizes the sub-predictors
  by their errors, and probability-domain blending "combined … rather than the
  prediction values"; the arbiters here are exactly probability-domain
  blenders, and the final stage is the only fixed blend left in the path.
- *Expected effect:* −0.2 to −0.5 %; 72 KB of weights; ~10 multiplies a bit.
- *Cost:* S; a format version.
- *Risk:* bytes change; a learned final blend can oscillate early in a file —
  start it at the fixed rule's weights.
- *Measurement:* enwik8 at 128 MiB and default; the corpus.
- *Evidence:* `predict_impl` (the fixed rule), `update_impl`; Handbook ord.
  4783–4823.

**15. Shrink the estimation rows: 17 buckets, 8-bit interpolation weight.** (R, M)
- *Current:* `APM_BUCKETS` = 33 rows of uint16 (66 B, straddling two lines),
  interpolated by `w = s & 127` (7 bits) in `predict_impl`.
- *Mechanism:* 17 buckets at 256 stretch units apart with the same
  interpolation: rows of 34 B (two per line), both stages halve (4.1 + 16.5 →
  2.1 + 8.3 MiB); one line per lookup instead of two half the time (C++ HP
  ord. 1052: a byte fetched brings its line whole).
- *Expected effect:* 10 MiB → −0.9 % at 128 MiB (rate) minus the coarser
  stage's loss (unmeasured; the interpolation is linear between neighbours, so
  the loss is the curvature of the logistic between 128-unit points).
- *Cost:* S; a format version.
- *Risk:* bytes change; the loss could exceed the gain at large budgets where
  the table is saturated and the 10 MiB buys nothing — apply below 256 MiB
  only, by header.
- *Measurement:* enwik8 at 128 MiB and default.
- *Evidence:* `APM_BUCKETS`, `SQUASH_POINTS` (33 points), `predict_impl`;
  C++ HP ord. 1052.

**16. Adopt the shadow's hashes in `start_byte` under a verified-equal contract.** (throughput)
- *Current:* `prefetch_byte` hashes the next byte's contexts from a shadow of
  `end_byte`'s state (`view_after`, `hash_kinds`) to plan prefetches, and
  `start_byte` hashes the same contexts again from the real state one byte
  later: 4.1 % of cycles at 128 MiB is the shadow, and the real hashing is a
  further ~40 instructions a bit inside `start_byte`'s 10.6 %.
- *Mechanism:* keep the shadow's `hk[]` and use it in `start_byte` when the
  byte it was computed for is the byte just coded (it always is in the
  encoder); the decoder keeps hashing. Bytes stay identical *only* because
  `view_after` equals `view_now` after `end_byte` — a property the
  `-DADAPT_SHADOW_CHECK` build verifies today but that a future edit to
  `end_byte` could silently break, turning a prefetch aim into a container
  difference.
- *Expected effect:* −2 to −3 % of encoder cycles (the real hashing's share).
- *Cost:* XS in code; the contract must be enforced: the shadow-check build in
  `make check` (today it is a scratch build), and the ISA equivalence test
  extended to compare a `prefetch_byte`-less build.
- *Risk:* the one named: an exact-by-construction property becomes a
  correctness dependency. Not to land without the check in the gate.
- *Measurement:* instructions per bit; `cmp` on the vectors and enwik8.
- *Evidence:* `prefetch_byte`, `start_byte`, `view_after`; Muchnick ord.
  6703–6710 (a prefetch placed a computed distance ahead is the same
  computation the loop does later — the same address arithmetic twice).

**17. Explicit huge pages when THP is off** — shared with the throughput
audit's item 13 (Gregg ord. 4419); 0 on this host, kept for portability of the
throughput floor.

**18. `posix_fadvise` on input and output** — shared with the throughput
audit's item 4 (Gregg ord. 5027–5040); a host-impact item (page-cache growth
during a 1 GB encode), no ratio or speed effect.

### Requires architectural change

**19. Two model sizes in one container: a dense low-order model plus a
budget-sized hashed one.** (R, M)
- *Current:* one hashed table serves every order; every context has the same
  block format (16 B, 15 nibble states) whatever its statistics.
- *Mechanism:* orders 1–2 in dense direct tables (256 and 65,536 × 32 nibble
  blocks = 128 KiB and 32 MiB — the latter too large at 128 MiB, so order 2
  keyed to 2^14 hashed rows) and orders ≥ 3 in the hashed table with the
  partition of item 5; the format then carries two table sizes. This is what
  makes the ratio a smooth function of the budget: the dense part is constant
  and the hashed part scales.
- *Expected effect:* items 5 + 8 together, plus never-evicted low orders:
  −1.5 to −2.5 % at 128 MiB; the memory-saturated container unchanged.
- *Cost:* L; a format version; the decoder's memory bound changes shape.
- *Risk:* two tables, two claim paths, two prefetch plans; the vector path's
  slot model (`cur`/`cbase`) must address both.
- *Measurement:* the budget curve (128 → 674 MiB) before and after.
- *Evidence:* `model_layout`, `claim`, `o1_blk`; Sayood ord. 3440–3442.

**20. A reversible text transform stage for the word contexts.** (M)
- *Current:* the x86 transform (`x86_forward`/`x86_inverse`,
  `adapt_model.h`) is the only byte transform; it rewrites call operands per
  block so repeated calls become identical byte strings, detected by
  `x86_detect`. Text gets no such stage: the word contexts (`H_WORD`,
  `H_WORDLC`, `H_WORD2`, `H_WORD3`, `H_WORDC`) hash the raw words, and
  capitalisation and inflection variants are different hashes.
- *Mechanism:* a per-block reversible transform in the same slot (detected on
  the first block, flagged in the header): case-flag extraction (a marker byte
  before a capitalised word, the word lowered) so `Wikipedia` and `wikipedia`
  share every context's statistics. Bounded, reversible, block-local, like the
  x86 stage.
- *Expected effect:* unquantified here — the corpus holds no measurement of a
  text transform's effect on a context-mixing coder (its "text preprocessing"
  is NLP), so this is admissible on codebase evidence only (the transform
  slot exists) and its number must come from the experiment: −0.5 to −2 % is
  the range such stages are built for, and enwik8's markup makes the
  case-flag variant the safe first one.
- *Cost:* M; a format version; a detector with a false-positive cost on binary
  data (the x86 detector's shape).
- *Risk:* bytes change; a transform that helps text hurts the corpus's binary
  rows unless gated by detection.
- *Measurement:* enwik8 at 128 MiB and default; the corpus.
- *Evidence:* `x86_forward`, `x86_detect`, `encode_blocks` (the transform
  call site); no corpus source — stated as such.

**21. Two-model coding of one file: a small model coding what a large model
cannot fit.** — rejected below as inadmissible; listed here only so the
question "why not two tiers of memory" has its answer: the decoder must hold
the same model, so a tier the encoder has and the decoder lacks is not a
codec.

## Top three, in order

1. **Items 1 + 2 + 4 together (budget-sized arbiter and estimation tables,
   honest headroom)** — one format version, S effort, **−1.30 % at 128 MiB
   measured** (items 1+2 alone, this audit's experiment), more where item 4
   tips the next power of two, speed unchanged. First because every other (R)
   item's value depends on the accounting they fix, because their risk is one
   header field, and because the experiment already exists.
2. **Item 3 (the window backed by the caller's buffers)** — the next 16 MiB,
   −1.4 %, bytes-identical where the reach is kept; second because it is an
   aliasing refactor across four functions and should land on the settled
   accounting of item 1.
3. **Item 11 (mixer selection by the highest order that hit)** — the first
   modeling change, because its input is a by-product of the claims the code
   already makes, it costs no memory, and it is worth more at small budgets
   than at large ones — the property the objective asks for. It goes third
   because it changes bytes at every budget and so needs the enwik8 and enwik9
   floors re-run before it is a gain.

Together, 1–3 put 128 MiB at ≈ 19.3–19.5 MB (19.83 measured after 1+2; item
3's 16 MiB is 0.32 of a doubling at a 64 MiB table, ≈ −0.7 %); item 5 or 19
and the modeling items 10–14 are what would take it toward 19.0. The
remaining distance to 17.0 MB is not on this list.

## Rejected candidates

| Candidate | Reason |
|---|---|
| A scout thread walking each block ahead of the coder to prefetch both nibbles' lines, the arbiter sets and estimation rows | Measured: bytes identical, wall unchanged on 8 MiB and on the 32 MiB slice with a 256 MiB table (64.8 s vs 64.7 s), +30 % CPU. The remaining time is core throughput, not memory latency (`docs/performance-audit-2026-09-14.md`, "Results"). |
| Fusing the eight predict/code/update steps of a byte into one function | Measured +7 %: the compiler's allocation across the fused body was worse than two calls. |
| Fusing the four first-layer trains into one pass over the inputs | Measured: no difference. |
| Per-cell lane words refreshed in the map update; 24-lane planes | Measured +5 G instructions / +8 %: every cell learns every bit, so work only moved; the extra lanes cost more than the forwarding stall. |
| Prefetching the arbiter sets toward L2; issuing the claim prefetches a bit later or earlier | Measured: no difference. |
| A larger history window | LOCKS.md: a 4× window alone gave 0.25 %; at 128 MiB its bytes are worth 1.4 % as table (item 3). |
| Parallel segments within 128 MiB | Each worker needs a model; two workers at 64 MiB each are below the max profile's 70 MiB minimum, and at any budget a segment restart costs ratio (+4.1 % at 16 MiB segments, `docs/performance-audit-2026-09-14.md` item 1). The objective is ratio at fixed memory; threads spend memory. C++ HP ord. 4091–4098 names memory bandwidth as the limit on parallel execution; the throughput audit measured it at 3.46× for six workers. |
| A smaller context table for speed | Measured: a 2 MiB table ran 6 % faster than 64 MiB (the earlier audit's control), and 32 MiB vs 512 MiB the same cycles interleaved on 8 MiB; memory buys ratio, not speed. |
| Two memory tiers (a model the encoder has and the decoder lacks) | Inadmissible: the decoder allocates the model the header names (`adapt_decode.c`, `--max-memory` "caps that and refuses a container needing more"); a tier the decoder cannot hold is not a codec. |
| 8-bit map cells or 6-bit states to shrink the table's block | The block is 15 states plus a tag in 16 B (`BLOCK`, `claim`); a state is one byte and the 22-bit map probability is per state, not per block, so the block cannot shrink without a coarser state machine — a modeling change with no evidence for its ratio here. Not below noise; no evidence. |
| Dropping the third and fourth arbiters or the second estimation stage outright to save 53 MiB | LOCKS.md format 19–24: the arbiters were measured at 1.5 %, the stage part of −4.2 %; 53 MiB is 1.4 doublings ≈ −3 % at 128 MiB — a coin flip against measured losses, and item 1–2's sizing keeps the mechanisms. |
| `MAP_HUGETLB`, `SCHED_BATCH`, verifier placement, `--verify` fitting | The throughput audit's items 13, 10–12; none moves ratio or memory at 128 MiB. |
| Interleaved trial models, trials scaled to the input | < 1 % on a 100 MB file (the earlier audit's item 3/14); below the objective's noise on this workload. |

## The measurement that would invalidate this ranking

The ranking rests on the exchange rate — that a byte moved from the arbiter,
estimation and window tables into the context table is worth what a byte of
table is worth on the measured curve (−2.46 % per doubling at 32 MiB). The
run that would have falsified it for items 1–2 was made: keys quartered, the
table one step larger, 20,091,771 → 19,831,153 B, so that half of the ranking
stands on a measurement. What remains falsifiable is item 3's half — that
the window's 16 MiB is worth its 0.32 of a doubling (≈ −0.7 %) rather than
the 0.25 % LOCKS.md measured for a window's *own* effect: one run at 128 MiB
with `window_log` held one step down and the table one step up (the sizer's
`--allocation context` policy already prefers this order; force it with a
budget between 128 and 160 MiB) — if the container is not smaller than
19,831,153 B by 0.5 %, the window is worth more than the table at this
budget and item 3 drops behind item 5. A second run, item 11 alone at the
default budget, tells whether modeling items are worth a format version at
all on this file: under 0.3 % (56 KB) and the modeling set collapses to items
10 and 13, which cost no memory and change the least.
