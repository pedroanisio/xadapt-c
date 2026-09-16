# Locked decisions

Decisions that are settled. A change to any of them is a new entry, not an edit.

## 2026-09-15 — The class-history width proved on the first block, the recall index's cap under --max, and the -7 objective measured

**Decision.** Two encoder policies, no format change (every sizing bit
and every model unchanged; a file written before decodes as before):

1. *The class-history width is proved on the whole first block when
   the sample cannot rank it.* `choose_model` ranked the three widths
   on a 32 KiB sample with a 1/1024 margin toward none. A class history
   earns its keep slowly — its eight-byte key spans 2^24 histories — so
   the sample put it within a thousandth of none whichever way the file
   went: on the first 8 MiB of enwik8 the sample had eight bytes 14 in
   12,037 ahead of none, the first 256 KiB block 282 in 82,357, the file
   0.34 %. Now, on an input the sizer gave a table of 2^21 blocks or
   more (4 MiB and up at the tiers that hold it, `CLASS_PROOF_TABLE_LOG`)
   whose sample puts a width within 1/256 of none (`CLASS_BAND`), that
   width — the sample's favorite, or the narrower of the two when it
   favored none — is coded against none on the whole block and taken
   only where it codes the block smaller; the block coding is kept for
   the set proof, which no longer repeats it. Measured with every width
   forced on every corpus row (`--max -6` and the default command): the
   block's verdict is the file's on every row it decides, and the
   eight-byte width beats the sample's choice on dickens (−0.12 %),
   samba (−0.19 %), mr (−0.26 % default) with no row worse. The proof
   costs two block codings on encode when it runs: +4.3 % instructions
   on the 8 MiB proxy, +0.4 % on enwik8, nothing on decode.
2. *The recall index grows to the format's 2^24 entries under `--max`*
   when the tier has room (`RECALL_LOG_SPARE` 24; the normal and fast
   profiles keep 22, `RECALL_LOG_SPARE_NORMAL`): on the first 32 MiB of
   enwik8, 2^22 → 2^24 entries 6,297,375 → 6,287,528 B (−0.16 %); at
   `-9` enwik8 gets 2^23.

Together on the 8 MiB proxy 1,686,110 → **1,680,015 B (−0.36 %)**, on
the 32 MiB slice 6,297,375 → **6,269,551 B (−0.44 %)**, the bytes
identical in a clang build and on the baseline instruction set. The
landing runs (`tools/check_ratio_floor.py --verify`, quiet host, every
round trip verified byte for byte): enwik8-16MiB 3,266,940 →
**3,254,114 B** (−0.39 %), enwik8 17,978,253 → **17,883,525 B**
(−0.53 %, 120.4 CPU s), enwik9 147,896,517 → **146,899,997 B**
(−0.67 %, 1,183.6 CPU s); the CPU floors stand (the proof's two block
codings cost 2–5 % on encode, within the 10 % tolerance).

**What was found on the way.** The round's first candidate, an int16
arbiter rate normalized by each set's running |error| (the NLMS step),
measured −0.36 % on the proxy — and every byte of it was the sample's
class-history verdict moving to eight bytes; the rate alone was worth
−0.04 % at +2.4 % decode cycles, and it cost every binary corpus row
0.05–0.2 %. Dropped. Rule from it: when a lever moves bytes, diff
`cadapt info` (class history, contexts, recall, table) between the two
binaries before crediting the model.

**The `-7` objective** (operator, 2026-09-15: enwik9 must reach the
`-11` container, 143,307,006 B, at the `-7` tier, 186 MiB). Where it
stands: enwik9 at `-7` on the previous binary is **156,680,572 B**
(table 2^23 blocks / 128 MiB, window 16 MiB, recall 2^20, compact side
tables; 1,259 wall s; 181 MiB peak) — 8.5 % to go; on this binary,
with the width's proof taking the eight-byte class history there,
**156,316,451 B** (−0.23 %; 1,170 s) — 8.3 % to go. The gap decomposed
on the first 32 MiB of enwik8 under the `-7` shape (table 2^18, window
2^19, recall 2^15, compact — the same bytes per block, window per byte
and recall per byte as enwik9 at `-7`; `ADAPT_SIZES` in a build with
`-DADAPT_SIZES_OVERRIDE`) against the `-11` shape (2^21 / 2^24 / 2^19):
7,027,692 vs 6,557,825 B, 7.2 %. The table eight times larger alone:
−5.7 %; the recall index sixteen times: −1.0 %, with the window to use
it −1.7 %; the window alone: 0 (the index's reach, not the window,
bounds recall — at `-7` the index turns over every 2^20 positions of a
16 MiB window; at `-11` every 2^24 of 512 MiB); the full side tables:
−0.43 %. The table's exchange rate at this pressure: −1.55 % (2×),
−3.3 % (4×), −5.7 % (8×). Claims at the `-7` shape: 59 % hit, 41 % miss,
68 % of evictions taking a block claimed once and never re-hit.

Measured at the `-7` shape, each alone (bytes vs 7,027,692):
- the lean set: −0.09 % at −3.4 % instructions;
- a pressure set dropping the eight kinds of non-positive marginal
  value there (o6, o8, word3, field, word, sparse2, record, indirect):
  −0.58 % at −15 % instructions, and +0.01 % at the `-11` shape — text
  hardly needs them at either pressure; set 4 with the lean bit, which
  the format has today: −0.22 %;
- window and index rebalanced at equal memory (2^17 / 2^17): −0.19 %;
- recall insertion at one position in 4 / 8 / 16 (content-defined):
  −0.06 / +0.02 / +0.16 %; dense recent ways beside sparse far ways:
  −0.06 %;
- admission control (a block on a context's second claim, an aged bit
  sketch): +0.17 to +0.30 % — the first observation is worth more than
  the eviction it causes;
- a hashed order-5 indirect context (what followed, two bytes) in o6's
  slot, at equal memory (half the table + the successors): +0.87 % —
  worth −0.42 % against the half table, a third of what the table's own
  doubling buys;
- PPMd-style information inheritance (a fresh block starts with the
  next lower order's states): +3.8 %.
Kind ablation at the `-7` shape (dropping one; + is worse): o1 +0.28,
o2 +0.20, o3 +0.08, o4 +0.05, o6 −0.10, word −0.04, field −0.05,
sparse +0.08, record −0.01, class +0.40, word2 +0.04, sparse2 −0.02,
indirect −0.01, nibbles +0.11, nest +0.64, o5 +0.02, o8 −0.09, wordlc
+0.03, word3 −0.07, wordc +0.09 %.

Also measured and not landed: a `-12` tier (4 GiB) with a 2^27-block
table, enwik9 at `-11` 143,307,006 → 141,532,476 B (−1.24 %, 2.7 GiB
peak, 1,381 wall s) — the opposite of the objective, so reverted
(`TABLE_LOG_MAX` 27, a `-12` preset of 4,096 MiB, a size_t guard in
`model_new`; a reader before it would refuse such a file). The normal profile's
21-byte class history wins at file level on webster (−1.4 %), reymont
(−2.1 %) and samba (−0.35 %) and loses on dickens (+0.44 %), and no
256 KiB sample ranks it (the block prefers eight bytes on all four):
a proof longer than a block is what it needs, and the throughput floors
do not hold one.

*Correction, 2026-09-16.* The `-7` shape above ran with the second max
model's pressure-gated parts off (`MODEL2_PRESSURE_LOG` reads the
table's size, 2^18 there); enwik9 at `-7` runs them (2^23). With
`ADAPT_MODEL2_SLOTS=1` the faithful `-7` shape is **6,974,653 B**
(the parts are worth −0.75 % at this pressure), the gap to the `-11`
shape 6.4 %, the table alone (2^21) −5.0 %, 2^19 −1.51 %. Re-measured
there, each alone: the first-nibble claim skipped for o6/word3/wordc/
wordlc −0.09 %; o4 case-folded −0.03 %; a mixer set keyed by the count
of fresh blocks +0.29 %; the pressure sets +0.14 to +0.36 % (the kinds
of negative marginal value cover one another — o6 −0.09, indirect
−0.06, field −0.05, sparse2 −0.05, word −0.04, word3 −0.04, record
−0.02, o5 0.00 %; nest +0.60, class +0.30, o1 +0.18, o2 +0.15 %);
half-size blocks for every kind but o1–o3, emulated as an upper bound
(those three confined to half of a doubled table): −1.48 % of the
doubling's −1.51 %; a victim store restoring nodes 1–3 of evicted
blocks that had two observations: −0.57 % for half the table's memory
again (the table's own +50 %: about −0.9 %); a four-byte entry taking a
context's first claims, a block only at its second or third root
observation: −0.20 % for the table's memory again (the doubling:
−1.51 %), +1.1 % at equal memory. Every scheme that keeps less than a
block for a context's first visits loses to spending the bytes on
blocks: the first observation is worth its block.

**Consequences.**
- The floors are re-locked below on this binary; the `-6` and default
  corpus floors are lowered where the gate found smaller containers.
- Development builds (`-DADAPT_SIZES_OVERRIDE`) read `ADAPT_SIZES`
  (table:window:recall:compact:lean), `ADAPT_CLASS_CODE`,
  `ADAPT_DROP_MASK` and `ADAPT_MODEL2_SLOTS`: the pressure proxy for the
  `-7` objective is the 32 MiB slice at `18:19:15:1:0`, class 1, slots
  on, against `21:24:19:0:0`.
- What the `-7` objective can reach with this model family: the levers
  measured sum to about 2 % (half-size blocks at most −1.5 %, the
  window and index rebalanced −0.2 %, the rest ±0.1 %), enwik9 at `-7`
  about 152–153 M; 143,307,006 B is the `-11` container and, with that
  2 %, would be reached from `-10` (1 GiB). An eight-fold table
  economy at 186 MiB has no measured path: the table is a cache under a
  compulsory-miss workload, and no admission, replacement, inheritance
  or successor structure recovers what the missing blocks would hold.
- The `-7` objective is 8.5 % away and 85 % of it is table pressure; the
  levers above sum to about 1 %. What remains is structural: denser
  blocks for the kinds that cannot recur at this pressure, or a model
  that spends its 128 MiB on something other than nibble blocks.

**Status.** Landed 2026-09-15 on branch `ratio-speed`.

## 2026-09-15 — The second max model: int16 arbiters, four re-purposed context slots, four-way recall, a fifth arbiter

**Decision.** Bit 7 of the sized max profile's sizing byte
(`adapt_sizes.model2`, `ADAPT_SIZING_MODEL2`) names a second max model,
and every `--max` encode that is neither adaptive nor BMP writes it, so
the max profile is always version 58 (61) now and 21, 24, 53, 54, 56 and
57 are read and no longer written. The model keeps every table, context
kind, run memory, estimation stage and the 1/32 start of the first and
changes four things, each measured on its own on the first 8 MiB of
enwik8 with `--max --allocation context` at the default tier (the CI
proxy, 1,700,354 B and 125.4 G instructions before; bytes by `cmp`,
instructions by `perf stat -e instructions:u`, round trips verified):

1. *The first-layer arbiters' weights are int16* in units of 1/32768,
   saturating at ±1, with the train step rounded to nearest —
   `vpmulhrsw` of the input times sixteen by the error is
   round(x·err / 2048) in one instruction for sixteen lanes — and the dot
   products by `vpmaddwd`, sixteen lanes an instruction, their eight
   int32 sums widened to 64 bits before they meet. −0.11 % bytes against
   the int32 arbiters at −10.4 % instructions, −9.5 % cycles interleaved,
   decode −9 %; the two side tables halve (36.5 → 18.3 MiB). The
   floor-shifted int16 forms lost bytes (1/16384: +0.92 %; 1/32768
   without rounding: +0.24 %; 1/65536 saturating at ±0.5: +0.52 %, its
   arbiters over-confident), so the rounding is what makes int16 pay.
2. *Four context slots carry other contexts.* Dropping a kind at a time
   showed three that cost more than they gave at this table load (80
   claims per block): without o5 −427 B, without o8 −999 B, without
   word3 −1,131 B, and sparse2 worth only +161 B. In their slots, at no
   change in claims or lanes: o5 the 21-byte class history (−2,390 B
   alone), o8 the order-1 indirect context — what followed the previous
   byte the last two times (−2,711 B), word3 recall's expected byte with
   its length bucket and the previous byte (−3,184 B), sparse2 recall's
   next two expected bytes (−542 B); −7,863 B (−0.46 %) for the first
   three together, the first-block trial then declining the 8-byte class
   history the 21-byte one makes redundant.
3. *A recall bucket holds the four most recent positions* of its key and
   the longest verified candidate wins, in the same table: −960 B for
   two ways, −1,794 B for four.
4. *A fifth first-layer arbiter* keyed by the bytes since the last space
   or newline (at most 15) and the previous byte's class, with the
   partial byte, 128 × 256 sets: −2,171 B (−0.13 %) for +4.6 %
   instructions. Two other keys lost or added nothing beside it
   (recall's length and expected bit; eight bits of the order-3 hash).

Together, this binary against the tree before it:

| Input | Before | After | Change |
|---|---:|---:|---:|
| enwik8-8MiB (`--max --allocation context`, the CI proxy) | 1,700,354 B, 125.4 G instr. | 1,686,110 B, 120.0 G instr. | −0.84 % bytes, −4.3 % instructions, decode 11.8 → 11.5 s |
| enwik8, first 32 MiB (the same command) | 6,351,878 B, 428.9 G instr. | 6,297,375 B, 410.4 G instr. | −0.86 % bytes, −4.3 % instructions, decode 45.9 → 44.4 s |
| codecbench `enwik8-8mib`, 3 repeats, encode + decode | 1,700,354 B, 23.8 s | 1,689,657 B*, 22.0 s | −0.63 %, 0.92× the time (*the state before the fourth change) |

The bytes are the same on the baseline and the x86-64-v3 paths and in
gcc and clang builds; a first clang build differed until the cast
intrinsic that leaves a vector's upper half undefined
(`_mm256_castsi128_si256`) was replaced by a zero-extension.

Measured on the proxy and not taken, each a format change of its own:
the estimation stages' update weighted by the interpolation (+92 to
+856 B), a 16-bit final probability into the coder (−24 B), the map
limit per kind (all worse) and a 10-bit map count with limits 511 and
1,023 (−109 B, +1 B), recall following its pointer through a miss (+220
to +344 B), the miss history as a confidence context (−17 B), the
highest order that hit as an arbiter key (+1,495 B on the first
arbiter; −2,335 B on the second and final, but +3,259 B once the slots
above were re-purposed), a 21st context kind (−2,153 B for +6.7 %
instructions: the lanes leave the sixteen-lane multiple), the last bit
in the small mixed states (+27 B: the run memories carry it), a
word-keyed third estimation stage (+524 B), the transform's codes
classed as letters (+1,224 B), a direct order-1 memory under the max
profile (−20 B), and the final blend at 1:1 or 1:7 (+1,072 B, +62 B).

**Consequences.**
- `RUN-5` holds: the scalar train and dot form the same integers as the
  vector ones (`train16_delta`, `dot16_scalar`), checked by the
  `ADAPT_ISA=baseline` cases and a clang build.
- The memory bound of a max model falls by about 13 MiB, so the tiers
  a max encode is raised to fall with it and the context policy reaches
  a table doubling sooner (the sizing tests name the new outcomes).
- The `ADAPT_SHADOW_CHECK` build exempts the two recall-derived slots:
  their lines are guessed after a byte that breaks a match, a wasted
  prefetch at match boundaries and never a bytes fault.
- `tools/ratio_floor.json` re-locked on this binary (main 371b42f,
  sha256 08330fcc…) the same day, on the reference host quiet (load
  under 1.2 throughout), `tools/check_ratio_floor.py --verify`, every
  round trip byte for byte, the enwik8 rows the median of three runs:

  | Fixture | Size before | Size after | CPU s before | CPU s after |
  |---|---:|---:|---:|---:|
  | enwik8-8MiB | 1,700,354 B | 1,686,110 B (−0.84 %) | 13.3 | 12.2 (12.1, 12.2, 12.3) |
  | enwik8-16MiB | 3,296,323 B | 3,266,940 B (−0.89 %) | 24.4 | 22.2 (22.2, 22.4, 22.1) |
  | enwik8 | 18,110,558 B | 17,978,253 B (−0.73 %) | 130.5 | 118.2 (120.0, 117.8, 118.2) |
  | enwik9 | 150,063,628 B | 147,896,517 B (−1.44 %) | 1,385.4 | 1,159.2 (one run) |

  Size and throughput tighten on every row. enwik9 gains twice what
  enwik8 does: the four-way recall buckets and the recall-derived slots
  are worth more on the long file. The enwik8 decode peaks at 653,712
  KiB of RSS (638 MiB; `cadapt info` names a ~638 MiB model) in 130.8 s
  wall on the quiet host.
- The encoder's max vectors are `text-max2.adpt` and `abc-max2.adpt`;
  `text-max.adpt` and `abc-max.adpt` stay as decode fixtures of 21.

**Amended the same day — the `DLV-3` gate.** `make corpus-floor` on the
landed binary: under the default command all 24 rows byte-identical;
under `--max --allocation context -6` nineteen rows smaller (every
Silesia row: nci −8.4 %, xml −4.4 %, reymont −3.8 %, webster −2.0 %,
samba −1.9 %, mozilla −1.5 %) and five Canterbury rows over their floors:
kennedy.xls +3.7 %, grammar.lsp +3.0 %, xargs.1 +1.9 %, fields.c +1.5 %,
alice29.txt +0.17 %. Ablated one change at a time on those rows (all at
`-6`, bytes): the fifth arbiter is the small files' loss (fields.c 2,531
→ 2,479 without it, grammar.lsp 1,162 → 1,125, xargs.1 1,625 → 1,589 —
starting its final weight at zero does not help: 2,536), the order-1
indirect context in o8's slot is kennedy.xls's (14,837 → 13,649 with o8
kept), recall in word3's slot is alice29.txt's (36,746 → 36,614 with
word3 kept); the class history in o5's slot and recall's next two bytes
in sparse2's are worth keeping on every row (without them nci +1.1 %,
xml +1.3 %). The mechanism was always table pressure — those three
parts pay when the table is oversubscribed and a 150 KB or 1 MB input
never oversubscribes it — and the header names a measure of it: the
table's size, which for a named file follows its length (150 KB → 2^20,
1 MB → 2^20, 5 MB → 2^21, 8 MiB → 2^22, enwik8 → 2^25). So the three run
from 2^21 blocks (`MODEL2_PRESSURE_LOG`), the other two slots, the
int16 arbiters and the four-way recall always. Thresholds 2^21 and 2^22
measured on the ten rows: at 2^22 xml (a 2^21 table at `-6`) gives back
0.2–0.5 %; at 2^21 every row is under its floor — kennedy.xls 13,481 B
(−5.8 %), fields.c 2,466, grammar.lsp 1,119, xargs.1 1,584, alice29.txt
36,631, sum 9,464, cp.html 6,296 — and no row at 2^22 or above changes
a byte (the proxy stays 1,686,110 B; the enwik8 and enwik9 floors, at
2^25 and 2^26, stand). The corpus floors under `-6` are lowered to the
gated binary's bytes. No container of the ungated bit-7 model was
published or pushed: the model was amended within the day it landed,
before any release, and the vectors `text-max2.adpt`/`abc-max2.adpt`
(49 KB and 40 B inputs, below the line) were regenerated.

**Status.** Landed 2026-09-15 on branch `ratio-speed` with the model,
the sizing bit, decoder, encoder, C and CLI tests, the man page, README
and CHANGELOG; the SBOM regenerated.

## 2026-09-15 — Throughput floors are CPU seconds, checked as the median of runs; a shared host gates relatively

**Decision.** The floors of `tools/ratio_floor.json` are seconds of CPU
time (user and system, as the kernel charges the encode) at the figures
the wall-clock floors held — 13.3 s, 24.4 s, 130.5 s and 1,385.4 s — and
`tools/check_ratio_floor.py` reads the measure from each floor
(`cpu_seconds` or `wall_seconds`, exactly one). `make gate` encodes the
8 MiB proxy three times (`--runs 3`) and takes the median, every run
held to the same bytes (RUN-5). With `--baseline-binary` the two binaries
take turns run by run. The 10 % tolerance stays. A floor is locked from
runs on a quiet host; a run beside other jobs never re-locks it.

**Why.** The corpus audit of 2026-09-15 (Systems Performance 2e, ord.
7797, 7812; C++ High Performance 2e, ord. 872, 994) found the gate taking
its verdict from a single wall-clock run, against floors locked beside
other agents' jobs: one hiccup could fail a sound build, and a floor
locked slow could pass a slower one. The re-lock runs that followed
measured what a CPU floor does and does not fix. It removes runqueue
wait — but at load 3–4 on this host's 12 hardware threads there was none:
CPU time equalled wall time in every run. It does not remove a
neighbour's cache: the same binary took 13.1 s alone (median of three,
10:21) and 15.3 s of both CPU and wall time beside two other encodes
(10:34; 26.1 s for 16 MiB, 137.2 s for enwik8), a 17 % cost the
scheduler cannot see. That is why the contended runs did not re-lock the
floors — a floor is only ever improved from — and why the relative gate
interleaves. Relabelling the wall figures as CPU floors can only tighten
them, since CPU time never exceeds wall time. Rejected: `perf stat`
cycles or instructions as the measure (frequency-invariant, but `perf`
and its permissions are not on every runner; the operator may still use
them by hand, as the audits do); a CPU budget for `make check` (a
parallel suite: 887 CPU seconds for 105 wall seconds on 12 CPUs).

**Consequences.**
- A build must reach 567,650 B/s of CPU time on the 8 MiB proxy in
  `make gate` (13.3 s less the tolerance), and the full floors' rates
  in `make ratio-floor`; a CPU floor on a platform without child CPU
  accounting is compared against wall time and the report says so.
- Re-locking is `tools/check_ratio_floor.py --runs 3 --verify` on the
  reference host with nothing else running; the report shows every run in
  both measures, so a re-lock taken beside another job is visible.
- The corpus floor files stay in wall seconds at their 25 % tolerance
  until they are re-locked the same way.
- `make check` keeps its 120 s wall budget (the entry below) and now
  prints the seconds it took and the load average, so a budget failure on
  a busy host is told from a code failure; `CHECK_SECONDS` widens it for
  that run, not in the Makefile.

**Status.** Landed 2026-09-15 with the corpus audit's other three fixes
(CHANGELOG: the hard-link table, whole-block writes, workers capped at
the CPUs online).
## 2026-09-15 — A window halved for a table eighth is measured and not taken

**Decision.** The context policy keeps the window whole, as the sized-table
entry below decided. A rule that halved the window once when the half it
freed bought the table at least one more eighth of its next doubling was
written, measured and rejected: it makes enwik8 smaller at every budget
where the table is short of its target, and larger three rows of the
mixed corpus at `-6`, which `DLV-3` makes the judge.

**Why.** Measured with `--max --allocation context`, this binary, each
row the rule's choice against the policy's:

| Input | Budget | Policy | Halved window | Change |
|---|---:|---:|---:|---:|
| enwik8, text transform | 128 MiB | 2^22 blocks, window 16 MiB: 18,957,171 B | 9/8 of 2^22, window 8 MiB: 18,920,972 B | −0.19 % |
| enwik8, `--transform none` | 128 MiB | 19,657,728 B | 19,632,586 B | −0.13 % |
| enwik8, text transform | 94 MiB (`-6`) | 9/8 of 2^21, window 8 MiB: 19,238,712 B | 10/8 of 2^21, window 4 MiB: 19,230,187 B | −0.04 % |
| enwik8, text transform | 160 MiB | 10/8 of 2^22, window 32 MiB: 18,857,598 B | 12/8 of 2^22, window 16 MiB: 18,784,270 B | −0.39 % |
| enwik8, text transform | 256 MiB | 9/8 of 2^23, window 32 MiB: 18,544,928 B | 10/8 of 2^23, window 16 MiB: 18,524,894 B | −0.11 % |
| the first 32 MiB of `libLLVM.so` | 128 MiB | 3,085,170 B | 3,083,929 B | −0.04 % |
| silesia/mozilla | `-6` | 12,414,028 B | 12,425,868 B | +0.10 % |
| silesia/nci | `-6` | 1,278,085 B | 1,279,544 B | +0.11 % |
| silesia/samba | `-6` | 3,012,350 B (its floor) | 3,023,223 B | +0.36 % |

The corpus rows were attributed by building the encoder both ways, with
and without the arbiters' 1/32 start of the entry below: mozilla and nci
reproduce their floors without the halving and change by nothing with the
start removed, so the halving alone is the loss. A larger recall index
for the halved window instead of the eighth (enwik8 at 128 MiB:
18,924,474 B) and both with the window quartered (18,935,762 B) were
also measured and were smaller gains on enwik8.

**Consequences.**
- `adapt_sizes_prioritize_context` is unchanged; the measurements are
  the record of what a 128 MiB text run leaves on the table (0.19 %)
  and what it would cost a mixed corpus.
- A future rule that trades the window is judged on `make corpus-floor`
  at `-6` before enwik8, per the DLV-3 entry.

**Status.** Rejected 2026-09-15.

## 2026-09-15 — The max profile under the text transform is its sized version, the arbiters starting at 1/32

**Decision.** Bit 6 of the sized max profile's sizing byte
(`adapt_sizes.low_start`, `ADAPT_SIZING_LOW_START`) starts every
arbiter's trust in a context's stretched probability at the weight 1/32
rather than 1/4, so the weights are learned from the input sooner than
trusted. The encoder sets it under the text transform and nowhere else,
so the max profile under the transform is always written as 58 (61),
with or without a fraction, and the transform's own max versions 64 to
66 (69 to 71) are read and no longer written; a decoder honours the bit
wherever the byte carries it. Bit 7 stays reserved and refused. Every
file written before decodes as written: bit 6 was zero in every 58 and
61 file.

**Why.** Measured with `--max --allocation context`, this binary against
the tree before it, round trips verified where the floor tool ran:

| Input | Budget | Before | After | Change |
|---|---:|---:|---:|---:|
| enwik8, first 16 MiB (library) | 128 MiB | 3,336,047 B | 3,330,628 B | −0.16 % |
| enwik8, first 16 MiB (library) | default tier | 3,320,062 B | 3,296,290 B | −0.72 % |
| enwik8 (library) | 128 MiB | 18,971,412 B | 18,957,171 B | −0.08 % |
| enwik8 (the locked command) | default tier | 18,207,203 B | 18,110,558 B | −0.53 % |
| enwik8-16MiB (the locked command) | default tier | 3,320,094 B | 3,296,323 B | −0.72 % |
| enwik8-8MiB (`make gate`) | default tier | 1,713,845 B | 1,700,354 B | −0.79 % |

The gain is the transform's: on the plain model the same weight was worth
0.18 % at 128 MiB and 0.11 % at the default tier on enwik8, measured in
the session that grew the recall index, and could not be carried by any
version without a new number; under the transform the model sees words
as symbols and the arbiters' first trust in each context costs more.
Gated to the transform so that `DLV-3` holds: text is an input-detected
path, and the plain model's containers are unchanged byte for byte.
Also measured under the transform at 128 MiB on the first 16 MiB and
within noise, so not taken: a 1/64 or 1/16 start, the mixer rate bounds
(4, 12, 128, 256), its decay (64, 128), the estimation stage's rate (6),
the mixer's skip (4) and count cap (1,023, 16,383); the lean set stays
worth 0.16 % there.

Rejected: a text version pair for the sized max profile (a new number
for a layout the sizing byte already describes); the 1/32 start for
every max file (unmeasured on the mixed corpus's non-text rows, and the
bit stays available for the day it is).

**Consequences.**
- `tools/ratio_floor.json` re-locked on this binary, measured on an Intel
  Xeon container: enwik8-8MiB 1,700,354 B, enwik8-16MiB 3,296,323 B,
  enwik8 18,110,558 B, every round trip verified byte for byte; the
  throughput floors are the reference host's and unchanged. enwik9 is
  unmeasured on this binary and keeps its floor.
- The mixed corpus (`make corpus-floor`, the `DLV-3` acceptance): under
  the default command all 24 rows are byte-identical (the normal profile
  never carries the bit); under `--max --allocation context -6` the two
  rows whose first block looks like text shrink, dickens 2,126,598 →
  2,124,454 B (−0.10 %) and webster 5,838,203 → 5,829,070 B (−0.16 %),
  and the other 21 are byte-identical, so those two floors are lowered.
- `cadapt info` prints the arbiters' start (`1/4` or `1/32`).
- The sized max profile's sizing byte has one reserved bit left, bit 7.

**Status.** Landed 2026-09-15 with `adapt_sizes.low_start`, the sizing
bit, decoder, encoder and CLI tests, the man page, README and CHANGELOG.

## 2026-09-15 — `make check` is held to 120 s of wall time

**Decision.** (Operator's, 2026-09-15.) The gate every landing runs,
`make check`, completes within 120 s of wall time or fails: the root
Makefile runs it under `timeout $(CHECK_SECONDS)` (default 120) and
reports the overrun as a failure. Work that needs longer — the CLI
suite's megabyte cases, the sanitizers, the corpus floors, the full
enwik8 floors — lives behind `make check-long`, `make sanitize`,
`make corpus-floor` and `make ratio-floor`, run once per merge, never
per push. The coverage floors (80 % library, 60 % CLI) stay in `make
check`, measured on the fast suites.

**Why.** The gate had drifted to 14–17 minutes: the coverage pass ran
every suite in order on an instrumented binary 5–9 times slower per
byte, and the suites had accumulated multi-megabyte encodes. A gate that
slow is not run, and a gate that is not run gates nothing. Measured on
this host after the change: `make check` 105 s wall (load average 4,
another job's corpus encode sharing the host), `make coverage` alone
115 s at load average 10 against 830 s before, branch counts identical.

**Consequences.**
- Every suite runs at once under `make -j` (the two large C suites as
  shards `k/n`, the CLI suite class by class), each process with its own
  gcov counts merged by `gcov-tool`; the coverage build is `-O1`.
- A new test that needs megabytes of input goes behind
  `ADAPT_LONG_TESTS=1`, or pins its contract on one block; a pipe's sizes
  do not depend on its length, and the C tests hold the sizer at any
  budget in milliseconds.
- Raising `CHECK_SECONDS` is a new entry here with the reason, not a
  quiet edit.

**Status.** Landed 2026-09-15 with the parallel `check`, `test` and
`coverage` targets, the shards, `check-long`, and this budget.

## 2026-09-15 — Levers are accepted on the mixed corpus; enwik8 is one row (DLV-3)

**Decision.** (Operator's, 2026-09-15: "enwik8 and enwik9 are benchmarks,
but this is a general-purpose tool".) A default, a policy or a modelling
lever is accepted only if it does not worsen the mixed corpus — the
Silesia and Canterbury corpora, fetched by `tools/fetch_corpus.py` into
`static/corpus/mixed/` and pinned by the manifest's hashes, never
committed (`DLV-4`) — under both commands `make corpus-floor` runs: the
default command and `--max --allocation context -6`, the largest tier a
128 MB machine holds. enwik8 and enwik9 stay as rows with stated goals
(17 MB; below 130 MB) and as the throughput reference; a gain on them
that costs the corpus lands, if at all, as an input-detected path, as the
x86 and BMP transforms and the text transform do.

**Why.** Every measurement before this entry was Wikipedia text (enwik8,
its prefixes, the six per-script rows); a default tuned on it overfits
one input class. The corpus spans prose, executables, images, databases,
office documents, source, XML and small files, where a text lever can
cost bytes and a small file pays the header. Rejected: vendoring the
corpus files (their terms are their sources', not this project's);
gating only on enwik8 (the failure this entry prevents); a corpus of
this project's own making (no one else can compare against it).

**The floors at 15d0cbe** (`cadapt` built from main, host shared with
other agents' jobs, every file decoded byte for byte):

| Command | Corpus | Bytes |
|---|---|---:|
| `encode` (default) | Silesia + Canterbury, 214,749,364 B | 40,478,327 B (18.85 %) at 15d0cbe; 40,426,753 B once PR #2 (the recall index grown) merged |
| `encode --max --allocation context -6` | the same | 40,330,336 B (18.78 %); 40,311,762 B once the text bit below landed (dickens −0.17 %, webster −0.26 %, the rest identical); 40,309,136 B on the merged binary at 87d50ec, which the floor files lock |

**Consequences.**
- `make corpus-floor` joins `make gate` and `make ratio-floor` as the
  gates a landing runs; a lever that lowers enwik8 and raises any corpus
  row is not landed unless it becomes input-detected.
- The floor files are raised (never lowered) by a new entry with the runs
  that justify it, as the enwik floors are.
- The tier decision above means "128 MiB" is measured at `-6` (94 MiB):
  enwik8 there is 19,306,669 B at 15d0cbe (text transform, compact, lean,
  table 2^21, no fraction).

**Status.** Landed 2026-09-15 with the fetcher and its tests, the
`--corpus` option of `check_ratio_floor.py`, the two floor files and the
Makefile targets.
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

## 2026-09-15 — The sizing byte carries the text transform (bit 5), so a fraction and the transform compose

**Decision.** On the sized max versions 58/61 bit 5 of the sizing byte is
the text transform; the encoder writes 58/61 with the bit when it takes
both a fraction and the transform, and the text versions 62–71 (a
power-of-two table) when there is no fraction. Bits 6 and 7 stay
reserved. A sized header without the bit reads with the transform off.

**Why.** At `-6` the encoder took the transform and dropped the fraction
(the text versions have no sizing byte), losing the −0.39 % the fraction
was worth there. Measured on enwik8, `--max --allocation context -6`,
this binary against main at 15d0cbe: 19,306,669 → 19,252,902 B
(−0.28 %), version 61, table 9/8 of 2^21 (36 MiB), peak RSS 89 MB,
decoded byte for byte. Rejected: a text bit in the fixed header's flag
byte (its bits are the window's), and a sized text version pair (four
more numbers for one bit).

**Consequences.** The corpus rows whose first block looks like text and
whose tier leaves slack change under `--max -6`; the default command's
bytes are unchanged (the normal profile takes no fraction).

**Status.** Landed 2026-09-15 with `ADAPT_SIZING_TEXT`, the encoder and
decoder rules, and C tests (the bit with every table, set and fraction; a
sized text container written, read back and decoded; the same sizes
without a fraction still the text version).

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
the frozen baseline decoder (`research/baseline-2026-09-14.md`, then named current-baseline.md); the enwik9
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
