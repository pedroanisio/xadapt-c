# Throughput study — what mcm and kanzi teach about reaching 3× at fixed ratio, HEAD `150a4c6`, 2026-09-16

<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-16, for the operator (Pedro Anisio de Luna e Silva). AI-generated: every number is from a run on the host named in Baseline on this date, or names the earlier audit it comes from (docs/performance-opportunities-2026-09-14.md, cited PO-2026-09-14; docs/performance-audit-2026-09-14.md, PA-2026-09-14). No code in the tree was changed. The competitors were built from the archives under _study/; the E5 figures come from a throw-away build of HEAD with a compile-time gate, described in E5 so it can be rebuilt, and are quoted from perf output of those runs. -->

**Summary.** Two open-source context-mixing compressors were built with our
flags and run on our proxy beside HEAD. Per coded bit, kanzi's top level
retires ≈ 326 instructions, mcm's ≈ 750, our `--max` ≈ 3,000. The difference
is model width — 7 and 13 inputs into one mixer against our 72 lanes into
five arbiters — not a technique we lack; every part of our width was landed
for bytes. **The evidence therefore does not support 3× on `--max` at its
current bytes and memory.** It supports roughly 1.1–1.2× on `--max` from
bytes-identical engineering of the per-byte code (23 % of its instructions),
and it supports 3× on the tiers below `--max` — the normal profile is beaten
on both bytes and time by a 6-input design at the same memory, and the fast
profile spends a third of its time on per-byte bookkeeping — but only by
building those tiers anew as new format versions. The experiments are listed
in priority order below; the two rebuilds are the only route to the target.

## 1. Baseline and constraints, restated

**Host.** AMD Ryzen 5 5600X (Zen 3, 6 cores, L3 32 MiB), GCC 13.3, Linux 7.0,
`perf` user-space counters only (IBS unavailable), as in PO-2026-09-14. Other
agents' encodes were running during every timed run today (1–2 concurrent
`cadapt encode`, one full-enwik8 mcm run): **instruction counts are exact;
cycle and user-second figures are indicative (±5 %)** and were taken
interleaved, HEAD beside the competitor, per `tools/check_ratio_floor.py`'s
method.

**Proxy.** The first 8 MiB of enwik8, MD5 `237766a94951aff8a1cc309f4800a458`
— the codecbench `enwik8-8mib` fixture (`static/corpus/enwik8`, first 8,388,608 bytes). All
three codecs run a word→code transform first and hand their model ≈ 62 % of
the bytes (kanzi reports 5,238,808 after its TEXT stage; mcm 5,170,788; ours
is documented at 62 % on enwik8 in `adpat/c/adapt_text.h`), so per-bit
figures below divide by ≈ 41.5 M coded bits for all three.

**HEAD on the proxy today** (a clean build of `150a4c6` from `git archive`,
default `-O3` with the runtime x86-64-v3 dispatch):

| profile | bytes | instructions | user cycles | IPC | user s | peak RSS | model memory (`cadapt info`) |
|---|---|---|---|---|---|---|---|
| `--max --allocation context` | 1,680,015 | 127.24 G | 60.2–61.8 G | 2.07 | 13.83 | 152 MiB | 2^22 blocks (64 MiB) + recall 2^23 (32 MiB) + window 8 MiB, ~150 MiB |
| normal | 1,726,147 | 90.36 G | 33.6–34.6 G | 2.6 | 7.13 | 110 MiB | 2^22 blocks + recall 2^22 + window 8 MiB, ~105 MiB |
| `--fast` | 1,890,381 | 31.61 G | 12.4–13.5 G | 2.4 | 2.47 | 48 MiB | 2^17 blocks (2 MiB) + recall 2^22 + window 8 MiB, ~43 MiB |
| `--max` decode | — | — | — | — | 11.88 | 152 MiB | roundtrip `cmp` clean |

Where the cycles go (`perf record`, cycles and instructions agree within a
point, so these are work, not stalls):

| profile | predict | update | start_byte | prefetch_byte | end_byte | code_block |
|---|---|---|---|---|---|---|
| `--max` | 38.5 % | 35.0 % | 13.0 % | 4.9 % | 4.7 % | 1.9 % |
| normal | 40.1 % | 36.4 % | 9.1 % | 4.9 % | 4.1 % | 2.7 % |
| `--fast` | 27.3 % | 28.9 % | 13.6 % | — | 19.6 % | 5.3 % |

The per-byte functions therefore cost `--max` ≈ 3,475 instructions per input
byte (12.6 + 5.5 + 4.8 % of 127 G over 8.39 M bytes) and `--fast` ≈ 1,300.
Inside the per-bit path the last ablation (2026-09-15, of 126 G) put the
first-layer trains at 17 %, the dots at 21 % and the memories' learning at
22 % of instructions.

**Locked floors** (`tools/ratio_floor.json`, quiet host, 2026-09-16): enwik8
17,883,525 B at 120.4 CPU s; enwik9 146,899,997 B at 1,183.6 CPU s; the
enwik8 `--max` decode peaks at 638 MiB. Corpus rows have byte and throughput
floors (`make corpus-floor`).

**Constraints that bind every experiment here** (`docs/CONSTRAINTS.md`):

- *Ratio.* "Preserving our compression ratio" is read as: every locked row's
  bytes at or under its floor, the proxy first and the mixed corpus before
  merge (`DLV-3`); a bytes-identical change meets it by construction, a
  bytes-changing one only by measurement.
- *Memory.* The tier is a decode promise (`RUN-1`); model memory follows the
  header, and a change may not raise what a given tier needs to decode.
  Competitor figures at 845 MiB–1.6 GiB are therefore compared to our tiers
  at 48–152 MiB with that caveat stated each time.
- *Format.* A change of model or layout is a new version, and a live version
  is never re-laid (`DLV-1`). Every bytes-changing experiment below is a new
  version by definition, so it lands only bundled or as a new profile.
- *Determinism.* The same bytes on every machine and code path (`RUN-5`);
  vector and scalar paths are held to equality (`ADAPT_ISA=baseline`, the
  `-DADAPT_SHADOW_CHECK` build).
- *Provenance.* Written from a technique's description, never from GPL or
  otherwise incompatible source; the origin of each idea recorded (`DLV-4`).
  Nothing in this document copies code; §7 is the provenance register.
- *Generality.* enwik8 and enwik9 are benchmark rows, not the judge of a
  default (`DLV-3`).

**Already measured — do not redo** (PO-2026-09-14, PA-2026-09-14, and the
memory notes of 2026-09-15): fused per-bit body (+7 %), scout thread (no
gain), PGO (within noise), branch-free run/recall lanes (no cycle win),
gathers for stretch/rate (worse on Zen 3), earlier or L2-targeted prefetch
(no gain), parallel segments (3.46× at six workers, −4.1 % ratio at 16 MiB
segments), a 2 MiB cache-resident table (only 6 % faster than 64 MiB: the
single core is instruction-bound), six workers saturating the memory system
(IPC 2.27 → 1.39 at unchanged counts).

## 2. What the two codebases do per coded bit (evidence)

Both were built `-O3 -march=x86-64-v3`, the ISA our v3 path targets; kanzi
with `-j 1` so one core codes, as ours does. Sources are named by file, not
quoted.

| | kanzi 2.5.3 `-l 9` (TPAQX) | mcm 0.84 `-x9` | cadapt `--max` |
|---|---|---|---|
| licence | Apache-2.0 | GPLv3 | — |
| bytes (proxy) | 1,783,853 (+6.2 %) | 1,658,301 (−1.3 %) | 1,680,015 |
| bytes (enwik8, their README / our floor) | 20,035,144 (+12.0 %) | 18,379,121 (+2.8 %) | 17,883,525 |
| instructions | 13.65 G | 30.86 G | 127.24 G |
| user cycles / IPC | 14.1 G / 0.97 | 25.3 G / 1.22 | 61 G / 2.07 |
| instructions per coded bit | ≈ 326 | ≈ 750 | ≈ 3,000 |
| branch misses / cache misses | 24 M / 197 M | 35 M / 208 M | 415 M / 1,100 M |
| peak RSS on the proxy | 845 MiB | 1.6 GiB (`-x6`: 245 MiB, 1,670,235 B) | 152 MiB |
| context inputs | 7 (o1, o2 direct; 5 hashed) + match | 12 hashed + match | 21 (o0 + 20 hashed) × 3 lanes + 6 runs + 2 recall + bias = 72 lanes |
| state storage | 1 byte per (context, partial byte), **no tag**, one table, index = hash + partial byte | same: `table[hash ^ partial byte]`, no tag | 16-byte tagged blocks, 4 ways, a claim per context per nibble (40 per byte) |
| state → probability | **constant table** | adaptive map per input | adaptive map per context kind, count-rated |
| mixers | one, 8 lanes, scalar int32, selected by o1 + match | one, 13 lanes, selected by a small context | five, 72 lanes each, int16 SIMD, three keyed by hashes; a final 6-lane arbiter |
| training | 8 MACs, skipped when the scaled error rounds to 0 | 13 MACs, skipped with all state updates when |err| is small (39 % of bits here) | 5 × 72 + 6 MACs, every bit |
| APM / SSE | two logistic, interpolated | one (two on LZP bits) | two interpolated stages |
| per byte | one hash update, five context multiplies, a match lookup | chained hashes, prefetch per context, word model | 20 `mix64` hashes, 20 claims per nibble, run slots, recall search, a prefetch plan for the next byte |
| other levels | `-l 8` (7 inputs) 1,827,373 B / 10.81 G; `-l 7` (BWT+CM) 1,969,881 / 5.09 G | `-h9` (10) 1,665,037 / 22.88 G; `-m9` (6) 1,706,170 / 16.20 G; `-m6` 1,717,010 at 3.05 s wall | normal 1,726,147 / 90.4 G; `--fast` 1,890,381 / 31.6 G |

Two things measured today that the comparison could have suggested and did
not hold: mcm's LZP byte bypass coded 6 bytes of this file (its text profile
needs a 12-byte match), and mcm's update skip, ported to us as a gate on the
arbiters' verdict or on the coded probability, saves 1.6–2.9 % of our
instructions for −31 … +1,185 bytes across thresholds 8–64/4096 — the
memories' learning, which is where mcm saves the rest, costs us +3.8 % bytes
and +12 % instructions when frozen (the states then never leave their
slow-learning regime). Numbers in E5.

The reading that survives: **at equal memory a 6–13-input tagless model with
one mixer is 3–6× cheaper per bit than our normal profile and codes the same
or fewer bytes at 8 MiB** (mcm `-m6`/`-x6` vs normal/`--max`); at 100 MB our
width buys 2.8 % over mcm and 12 % over kanzi; and our IPC is the highest of
the three, so what we execute we execute well — the count is the problem.

## 3. Missing information (not invented)

1. The exact number of coded bit-steps for our model on the proxy (the text
   transform's output length is not reported by `cadapt info`); ≈ 62 % is
   inferred from `adapt_text.h`'s enwik8 figure. Per-bit instruction counts
   for us carry that ±3 %.
2. A quiet-host cycle baseline for today's runs (other agents were encoding).
   Every cycle ratio in this document should be re-taken before it is quoted
   as a result; instruction ratios need not.
3. The instruction-level split inside `start_byte` (1,910 instr/byte under
   `--max`) and `end_byte` (790 under `--fast`): which of claims, hashing,
   run slots, recall verification and the view dominate. `perf annotate` was
   not run; E1–E3 start by running it.
4. The bytes each width component is worth on the current model: the third
   lane per context (no figure recorded), arbiters 3 and 4 (recorded as
   "1.5 % on text" together), the fifth arbiter (0.13 %, recorded). Needed
   only for §6, the levers this document excludes.
5. The ratio cost of 64 MiB segments under `--threads` (PO-2026-09-14
   estimated 1–1.5 %, unmeasured; 16 MiB measured −4.1 %).
6. A decode-side profile; only the `--max` decode's user time (11.88 s, near
   the encoder's 13.83) was taken.
7. kanzi `-l 8`'s peak RSS (only `-l 9`'s 845 MiB was taken) and mcm `-m6`'s
   instruction count (only its wall time).
8. Whether a 7–13-input model reaches mcm's or kanzi's bytes **under our
   memory discipline** — kanzi at 845 MiB and mcm at 1.6 GiB against our 48–152
   MiB. The only matched-memory evidence is mcm `-m6`/`-x6`; it is the single
   most important unknown for E9 and E10 and their first measurement.
9. The licence and lineage of Tangelo 2.4, which kanzi's TPAQ names as its
   origin; not in the archive. Flagged in §7 for review.
10. Whether the throughput floors in `make corpus-floor` are CPU or wall
    seconds per row, and their current values; the acceptance criteria below
    say "under its floor" and defer to the file.

## 4. Experiments

Each entry: observation and source (E = evidence measured, I = inference);
hypothesis and change; preconditions; expected effect with confidence;
minimal benchmark; acceptance; effort (XS/S/M/L/XL); risks. Effects are
stated per profile and are **not additive**: E1–E3 draw on the same 23 %
per-byte budget of `--max`, and E9/E10 replace rather than add to E4.

Common preconditions, unless an entry says otherwise: a quiet host
(`pgrep -fa "cadapt encode|mcm |kanzi"` empty); a clean HEAD build as the
baseline binary; the proxy above; `perf stat -e instructions:u,cycles:u`
interleaved A/B, three runs, instructions decisive; `cmp` of containers for
bytes-identical claims; `make check`, `make sanitize`, `ADAPT_ISA=baseline`
bytes equality and the `-DADAPT_SHADOW_CHECK` build for anything that touches
the prefetch plan; `tools/check_ratio_floor.py --baseline-binary <HEAD>
--runs 3 --verify` for the floors and `make corpus-floor`, run alone, before
merge. Decode is measured wherever encode is: the per-bit path is shared.

### Tier A — bytes-identical engineering; land under existing gates

**E1. Per-profile clones of the per-byte functions.**
- *Observation.* `start_byte`, `end_byte` and `prefetch_byte` appear as
  out-of-line symbols taking `fast` (and reading `m->max`/`m->model2`) at run
  time; GCC cloned only `prefetch_byte.constprop.0`. The per-byte code is
  22.9 % of `--max`'s instructions and 34 % of `--fast`'s (E). kanzi and mcm
  specialise their whole per-byte path at compile time (template parameters
  for the input count and the variant), so no profile pays a test for another
  profile's work (E, from reading; the share it saves is I).
- *Hypothesis.* Instantiate the three functions once per profile with the
  profile constants folded (as `predict_impl`/`update_impl` already are), out
  of line — not inlined into `update`, which PA-2026-09-14 measured +6 %.
  Dead work and runtime tests fall out; bytes unchanged.
- *Preconditions.* Common set. Depends on nothing.
- *Expected.* `--fast` −3…−8 % instructions, normal/`--max` −1…−3 %
  (confidence: low–medium; the I-cache cost of three more clones is the
  unknown). Ratio: identical. Memory: none.
- *Benchmark.* Three profiles on the proxy, interleaved with HEAD;
  `perf annotate` of the per-byte symbols before and after; `cmp`.
- *Acceptance.* `cmp` identical on the proxy and the corpus; instructions
  down on every profile or the change is dropped for that profile; no
  throughput row over its floor.
- *Effort.* XS–S. *Risk.* Code size; a clone that the compiler already
  specialised (prefetch_byte) gains nothing — measure per symbol.

**E2. Hash once per byte per side: reuse the prefetch plan's hashes at byte start.**
- *Observation.* The encoder computes the byte view and `hash_kinds` twice
  per byte (`prefetch_byte` for the byte being coded, then `start_byte`); the
  decoder three times (both candidate values, then the real one).
  PO-2026-09-14 costed the recomputation at ≈ 600 instructions per byte and
  rejected it at 1.4 % of a 21,410-instruction byte (E). Since then the text
  transform and model2 cut the byte to ≈ 15,160 instructions, so the same
  600 are ≈ 4 % (I, arithmetic). kanzi computes each context hash exactly
  once per byte (E).
- *Hypothesis.* Keep the planned `hk[]`/`rh` with the byte value they were
  planned for; in `start_byte`, use them when the coded byte matches, else
  recompute. model2's two recall-derived slots, which the plan is allowed to
  aim wrong at match boundaries (documented at `start_byte`'s shadow check),
  are always recomputed.
- *Preconditions.* Common set plus the `-DADAPT_SHADOW_CHECK` build, which
  already asserts plan == real hash for every other slot: it is the test.
- *Expected.* Encoder −3…−4 % instructions, decoder −6…−8 % (confidence:
  medium; the count is from the earlier audit, the share from today's).
  Ratio: identical (the hashes are equal by the shadow assertion; when they
  are not, the recompute path runs). Memory: a few hundred bytes of state.
- *Benchmark.* Proxy encode and decode, interleaved; shadow build on the
  corpus.
- *Acceptance.* `cmp` identical; shadow build clean on the corpus; decode
  instructions down ≥ 4 %.
- *Effort.* S. *Risk.* A plan/real divergence that the shadow build does not
  cover would silently change bytes — `RUN-5`; the recompute-on-mismatch rule
  is what keeps the bytes independent of the plan, and it must stay.

**E3. Cheaper claims.**
- *Observation.* `--max` makes 40 claims per byte (`claim`, adapt_model.h
  ≈ 1437): a line address, four tag loads and compares, a hit index, or an
  eviction scan of four observation counts and a 16-byte clear; then
  `claim_chain`'s alias stamps and `slots_refresh`'s eight-state packing.
  `start_byte` is 1,910 instructions per byte (E). kanzi and mcm have no
  claims at all — a byte state is addressed directly and collisions are
  accepted (E); that principle is not available bytes-identically, so the
  engineering form is to make each claim cheaper.
- *Hypothesis.* One 16-byte vector compare for the four tags and one for the
  four counts (the four blocks of a line are contiguous), a mask-to-index
  without the three adds, and `slots_refresh` folded into the claim loop.
- *Preconditions.* Common set; after E1 so the clone is what is measured.
  Needs missing item 3 (annotate) first to confirm claims dominate
  `start_byte`.
- *Expected.* `--max`/normal −2…−4 % instructions (confidence: low until
  annotate; the arithmetic ceiling is the claims' share of 12.6 %). Ratio:
  identical (the chosen way is unchanged). Memory: none.
- *Benchmark.* As E1; `perf annotate claim` share before/after.
- *Acceptance.* `cmp` identical; `start_byte`'s instruction share down.
- *Effort.* S. *Risk.* Zen 3 vector-to-scalar moves are not free; the
  branch-free scalar form was itself a measured win (PA-2026-09-14) and may
  already be at the floor.

**E4. Vector lanes and learning for the normal profile.**
- *Observation.* `context_lanes_v3` and `memories_learn_v3` run only under
  `max && v3 && !aliased` (adapt_model.h ≈ 2230 and ≈ 2531); the normal
  profile takes the scalar per-context loops for its 11 contexts, and its
  predict + update are 76 % of its cycles (E). On `--max` the vector paths
  were part of the 143.5 → 80 G cut (PA-2026-09-14).
- *Hypothesis.* A one-plane variant of both vector paths (normal has one
  lane per context; the max code assumes three planes) removes the scalar
  per-context loop from normal's per-bit path.
- *Preconditions.* Common set; `ADAPT_ISA=baseline` equality is the
  correctness test (`RUN-5`). Independent of E1–E3. **Superseded by E10** if
  the normal tier is rebuilt — decide E10 before spending here.
- *Expected.* Normal −10…−20 % instructions (confidence: medium–low; the
  memories' learning was 22 % of `--max`'s instructions in vector form, so
  the scalar form is a larger share of normal's). Ratio: identical. Memory:
  none.
- *Benchmark.* Normal profile on the proxy and on two non-text corpus rows,
  interleaved; `cmp`; baseline-ISA `cmp`.
- *Acceptance.* `cmp` identical on both ISA paths across the corpus;
  instructions down ≥ 8 %.
- *Effort.* S–M. *Risk.* Wasted if E10 lands; the aliasing fallback must stay
  scalar, as it does for max.

### Tier B — new profiles; the only route to 3× the evidence supports

**E9. Rebuild the fast tier as a 7–8-input, one-mixer model ("kanzi-class").**
- *Observation.* kanzi `-l 8` (7 inputs + match, one 8-lane mixer, one APM,
  constant state map, tagless states) codes the proxy to 1,827,373 B —
  3.3 % under our `--fast` — in 10.81 G instructions, 34 % of ours, at
  ≈ 260 instructions per bit against our ≈ 750 for a model of similar input
  count (E). In cycles the two are close (10.0 vs 12.9 G) because kanzi's IPC
  is 1.0 at 845 MiB of tables (its `-l 9` RSS; `-l 8` not taken) while ours is
  2.4 at 48 MiB (E). Our `--fast` spends 34 % of its instructions per byte,
  not per bit (E). Inference: the instruction gap is design (lanes, claims,
  per-byte work), the byte gap may be kanzi's 17× memory.
- *Hypothesis.* A new profile: order-1 and order-2 as direct byte-state
  arrays (no hash, no tag), four or five hashed kinds as tagless byte states
  in one table sized by the tier, the recall model with a fixed
  length→confidence table, one 8-lane int16 arbiter selected by the previous
  byte and the recall state, one interpolated APM, and a per-byte path that
  computes only those hashes. Target: ≤ 1,890,381 B on the proxy at ≤ 12 G
  instructions (≥ 2.6×), at today's `--fast` memory (~43 MiB).
- *Preconditions.* A new format version (`DLV-1`); the mixed corpus with
  every row's bytes and throughput floors (`DLV-3`); README, man page,
  CHANGELOG, LOCKS entry; a decision that the fast tier may change its
  bytes (a new version, so existing files are untouched). Independent of
  Tier A; shares the per-byte design with E10.
- *Expected.* Instructions ÷ 2.5–3 (confidence: medium — the count is a
  design property); cycles ÷ 2–3 (medium; the small tables keep IPC high);
  bytes at or under today's `--fast` (**medium–low**: item 8 of §3 — the
  tagged 16-byte block may be what makes a 2 MiB table competitive, and a
  tagless table at that size could lose more than kanzi's 3.3 % margin).
  Memory: equal to today's fast tier by construction.
- *Benchmark.* Proxy plus the corpus: bytes, instructions, cycles against
  `--fast`; RSS; decode symmetric.
- *Acceptance.* On the proxy and every corpus row: bytes ≤ the current fast
  floor and instructions ≤ 40 % of the current fast profile's; RSS ≤ current
  at every tier. Fails on either → not merged.
- *Effort.* L. *Risks.* Bytes under memory discipline (above); a second
  state-storage layout in the codebase to maintain; the constant state map
  may need to stay adaptive to hold bytes on non-text rows (kanzi lands
  within 6 % of us on text with a constant map, but its value on binaries is
  unmeasured).

**E10. Rebuild the normal tier as a 6–10-input, one-arbiter model ("mcm-m-class").**
- *Observation.* At matched memory, mcm `-m6` (6 inputs, one mixer, one SSE,
  adaptive maps, 128 MiB) codes the proxy to 1,717,010 B in 3.05 s wall; our
  normal profile 1,726,147 B in 7.13 user s at 110 MiB (E); at 1 GiB, `-m9`
  is 1,706,170 B in 16.2 G instructions, 18 % of our 90.4 G (E). kanzi `-l 9`
  is +3.3 % bytes at 15 % of our instructions (E).
- *Hypothesis.* A new normal profile: 8–10 context kinds at one lane each,
  our tagged table and window unchanged, one 16-lane int16 arbiter keyed by
  the previous byte, the recall and one run model, one interpolated APM.
  Target: ≤ 1,726,147 B on the proxy at ≤ 30 G instructions (3×), at
  today's normal memory.
- *Preconditions.* As E9. Supersedes E4. Shares its arbiter and per-byte code
  with E9 — the two are best built as one profile family with a width
  parameter, which is also how kanzi and mcm expose their levels.
- *Expected.* Instructions ÷ 3 (confidence: medium); cycles ÷ 2.5–3 (medium);
  bytes ≤ current normal (**medium**: mcm's matched-memory result is the
  evidence, but its byte-state and mixer-selection contributions are
  unseparated, so our 10-kind version may land above it). Memory: equal.
- *Benchmark.* As E9, against the normal profile.
- *Acceptance.* As E9 with the normal floors; additionally the enwik8 row
  under the normal profile's floor.
- *Effort.* L. *Risks.* The current normal profile's 10 kinds were each
  chosen against the profile without them; dropping the second–fourth
  arbiters and the run lanes may cost more on records/binaries than on text
  — the corpus decides, not the proxy.

### Tier C — bytes-changing, bundle-only, or outside the ratio constraint

**E5. Confidence-gated arbiter training.** *Measured today* (a throw-away
build of HEAD with a compile-time gate in `update_impl` on `target − pfinal`
or `target − p`, skipping the five trains and the final train; proxy, `--max`): gate on the arbiters' verdict — threshold 8/4096:
−31 B, −1.6 % instructions; 16: +44 B, −2.0 %; 32: +328 B, −2.5 %; 64:
+1,185 B, −2.9 %; gate on the coded probability — 8: −19 B, −1.7 %; 16:
+125 B, −2.1 %; 32: +566 B, −2.5 %; cycles unresolvable at any threshold.
Freezing the memories' learning as well: +3.8 % bytes, +12 % instructions
(rejected). *Hypothesis now:* at threshold 8 it is bytes-neutral and worth
≈ 2 %; it changes the bytes, so it is a version change and is worth carrying
only inside the next `--max` version, never alone. *Provenance:* the
technique is visible in mcm (GPLv3); the gate here is three lines written
from its description, flagged in §7. *Effort* XS. *Confidence* high (measured).

**E6. Cache-resident side tables (multi-worker lever).** *Observation:* we
take 1,100 M cache misses on the proxy to kanzi's and mcm's ≈ 200 M at the
same bit count (E); per bit we fetch three hash-keyed 144-byte arbiter rows
(tables of 9.4 MiB each, 4.7 under the compact layout) and a 17 MiB APM2 row,
all prefetched a bit ahead, so on one core they cost bandwidth, not latency —
PO-2026-09-14 measured the single core instruction-bound and six workers
memory-bound (IPC 2.27 → 1.39). kanzi's mixer array is ≈ 2 MiB. *Hypothesis:*
keying arbiters 3/4 by 12–13 bits and APM2 by 15 puts them in L2/L3 and cuts
per-bit traffic ≈ 40 %; single-core gain ≤ 6 % (the 2 MiB-table measurement
bounds it), aggregate at six workers perhaps +20…+30 % (I, low confidence).
*Ratio:* the compact tables' −1.87 % at 128 MiB came from re-spending the
memory on the context table; at the proxy tier smaller keys likely cost
bytes (low confidence either way). *Effort* S (constants + version).
*Interaction:* only pays with `--threads`; bundle with E5.

**E7. Constant state→probability map for the hashed kinds.** *Observation:*
kanzi's top level uses a constant table and lands within 6.2 % of us with
every other simplification included (E); the map's own share is unknown (I).
Our 21 count-rated map updates are within the 22 % "memories' learning".
*Hypothesis:* freezing a kind's map once its cells have seen CELL_LIMIT
observations saves ≈ 3 % of instructions. *Ratio:* probably +0.1…+0.5 %
(low confidence) — expected to fail the ratio criterion; listed so it is not
re-proposed without this note. *Effort* XS on the scratch harness. *Kill
criterion:* any byte loss on the proxy.

**E8. Parallel segments at 64 MiB (`--threads`, existing).** Wall-clock, not
per-core, throughput: 3.46× at six workers measured, −4.1 % ratio at 16 MiB
segments (PO-2026-09-14); 64 MiB estimated −1…−1.5 %, unmeasured (§3 item
5). kanzi's README speeds rest on the same lever (32 MiB blocks). **Outside
"preserving ratio" unless the operator sets an allowed loss**; the one
measurement to run is the 64 MiB ratio on enwik8 and enwik9, then decide.
*Effort* XS. Not counted toward the 3×.

## 5. Priority, independence and interactions

| rank | experiment | tier | bytes | expected (profile) | confidence | effort | depends on | interacts with |
|---|---|---|---|---|---|---|---|---|
| 1 | E2 hash once per byte | A | identical | −3…−4 % enc, −6…−8 % dec (all) | medium | S | shadow build | E1, E3 (same budget) |
| 2 | E1 per-profile clones | A | identical | −3…−8 % fast, −1…−3 % others | low–med | XS–S | — | E2, E3 |
| 3 | E10 normal rebuild | B | new version | ÷3 instr, ≤ bytes (normal) | medium | L | corpus, docs, version | supersedes E4; shares code with E9 |
| 4 | E9 fast rebuild | B | new version | ÷2.5–3 instr, ≤ bytes (fast) | med (instr) / med–low (bytes) | L | as E10 | shares code with E10 |
| 5 | E4 normal vectors | A | identical | −10…−20 % (normal) | med–low | S–M | — | wasted if E10 |
| 6 | E3 cheaper claims | A | identical | −2…−4 % (max, normal) | low | S | E1, annotate | E1, E2 |
| 7 | E5 gated training | C | version | −2 % (max) | high | XS | next max version | bundle with E6 |
| 8 | E6 resident side tables | C | version | ≤ 6 % core, +20–30 % at 6 workers | low | S | `--threads` | E5 |
| 9 | E8 64 MiB segments | C | ratio loss | 3.4× wall | high (speed) | XS | operator's X | — |
| 10 | E7 constant map | C | version | −3 % | low | XS | — | likely killed |

- **Run independently:** E1, E2, E4 (different functions; each measured
  against HEAD alone first). E5 and E7 need only a compile-time gate in
  `update_impl` on a throw-away build, as E5's runs were made.
- **Sequence:** E3 after E1 (measure the clone); E9/E10 after the operator
  decides the tiers may be rebuilt as new versions and after §3 item 8 is
  answered by a first cut of the tagless table on our sizes.
- **Not additive:** E1 + E2 + E3 together cannot exceed the per-byte share
  (23 % of `--max`, 34 % of `--fast`); a realistic combined `--max` outcome is
  1.10–1.15×, `--fast` 1.2–1.4×. E9/E10 replace the profile, so Tier A gains
  on those profiles do not carry over.
- **What reaches 3×:** only E9 and E10, on their own tiers, and only if their
  bytes hold under our memory. Nothing here reaches 3× on `--max`.

## 6. Excluded by the ratio constraint (for the record)

| lever | instruction effect | bytes | source |
|---|---|---|---|
| drop the third lane per context (21 of 72 lanes) | −29 % of the mixer MACs, ≈ −10 % instr | unmeasured on the current model (§3 item 4) | adapt_model.h ≈ 213 |
| drop the fifth arbiter | −4.6 % | +0.13 % | model2 landing, 2026-09-15 |
| drop arbiters 3 and 4 | ≈ −25 % of mixer MACs | +1.5 % on text | adapt_model.h ≈ 253 |
| tagless states for `--max` | claims gone (≈ −8 %) | loss under table pressure expected; the tags are what the 128 MiB tier's gains rest on | §2 |
| freeze the memories' learning on confident bits | −(would be) | +3.8 %, and +12 % instr | E5 |
| mcm's LZP byte bypass | — | irrelevant here: 6 bytes took it | §2 |

## 7. Provenance and licensing register

Ideas, not code, were taken; no line of either archive was copied or
translated. Each entry names the earliest description known to this author
and what an auditor should check. **An independent implementation does not by
itself settle licence or patent questions**; the flags are for review.

| idea | where seen | earliest description known | licence of the codebase seen | flag for review |
|---|---|---|---|---|
| tagless byte-state hash tables addressed by hash + partial byte (E9, E10) | kanzi TPAQPredictor, mcm CM | Mahoney, *Data Compression Explained* (2010), and the lpaq1 design note (2007) | Apache-2.0 / GPLv3 | paq-family code is GPL; the technique is published prior art; check for patents on hashed context tables before filing anything that claims them |
| nonstationary bit-history state machine with a constant state→probability table (E7, E9) | kanzi | Mahoney, DCE §"Indirect context models"; the fixed-map variant is the initial table of the same design | Apache-2.0 | kanzi's TPAQ names Tangelo 2.4 (Jan Ondrus) as origin; Tangelo's licence and its own derivation from paq8/fp8 (GPL) are not in the archive — §3 item 9 |
| one logistic mixer selected by a small context (E9, E10) | kanzi, mcm | Mahoney 2005, *Adaptive weighing of context models for lossless data compression* (Florida Tech TR CS-2005-16) | both | prior art; none known |
| training skipped when the prediction error is small (E5) | mcm Mixer (comment states the gain); kanzi skips only at err = 0 | not found in Mahoney's descriptions; treat mcm as the source | GPLv3 | rejected here anyway; if bundled, cite mcm as the source of the observation and the three-line gate as ours |
| interpolated logistic APM / SSE (already ours) | all three | Mahoney, DCE §"SSE"; Bloom's SEE (1998) | — | none |
| match model with a length→confidence table (E9) | kanzi, mcm | Mahoney lpaq1 (2007) | both | none |
| per-byte context hashes computed once, cheap multiplicative hashing (E2) | kanzi | Knuth, TAOCP vol. 3 §6.4 | Apache-2.0 | none |
| compile-time specialisation of the per-byte path per level (E1) | kanzi templates, mcm templates | general engineering | — | none |
| block-parallel coding with cold blocks (E8) | kanzi `-j` | bzip2, zstd, ours already | — | none |
| LZP byte bypass (not proposed) | mcm | Bloom, *LZP: a new data compression algorithm* (1996) | GPLv3 | not proposed; if ever revisited, check Bloom's and later LZP patents |

kanzi's Apache-2.0 licence permits use with attribution, but `DLV-4`'s rule
is written from the description regardless of licence, and this document
follows it: kanzi is cited as a working demonstration of the principles, not
as source.

## 8. The measurement that would change this ranking

The ranking puts the rebuilds (E9, E10) above the engineering items because
matched-memory mcm beats our normal profile on both axes. That rests on one
file at 8 MiB. Before either rebuild is started, run mcm `-m6` and kanzi
`-l 8`/`-l 9` on four non-text corpus rows (an executable, a raw image, a
records file, an already-compressed file) beside our normal and fast
profiles. If the narrow models lose their byte advantage off text, E9/E10's
byte targets stand but their confidence drops to low, and the honest answer
to "3× at fixed ratio" becomes "not on any tier without a ratio decision";
Tier A then is the whole programme, worth 1.1–1.4× depending on profile.

---

Related: `docs/performance-opportunities-2026-09-14.md` (the ranked list this
extends), `docs/performance-audit-2026-09-14.md`, `docs/CONSTRAINTS.md`.
Back to the root [README](../README.md).
