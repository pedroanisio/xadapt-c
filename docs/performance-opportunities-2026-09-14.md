# Performance opportunities — cadapt at HEAD `085dff0`, 2026-09-14

> **Status, 2026-09-18 (9109c3f, cadapt 0.12.0).** A dated record: true of
> the code at 085dff0, its status brought forward here and not promised
> beyond it; the decisions are in `LOCKS.md`. Landed since 2026-09-15, each
> bytes-identical or measured for bytes in the changelog's `[0.12.0]`
> Performance entries: item 8 as the second max model's int16 arbiter
> weights with `vpmaddwd` dot products and a rounded `vpmulhrsw` train
> step (`LOCKS.md` 2026-09-15: −10.4 % instructions, decode −9 %, at
> −0.11 % bytes — the earlier 16-bit form that grew the proxy 8.2 % is the
> one the 2026-09-15 status below rejects); the max profile's vector lane
> and learn paths running only the slot groups a file's context set fills
> (the lean baseline 8–9 % faster on encode, 5 % on decode: enwik8 117.1 →
> 107.4 CPU s, the floors re-locked, 2026-09-16); item 11's family — the
> first block's width trials stopping after the first when the sample
> declines the class context, and a model's arbiter weights initialized by
> one row copied per set instead of a lane-by-lane call (45 % of a small
> file's encode); and the mid profile (`--mid`, 2026-09-16, opt-in: a
> tier below `--max` built anew as its own format versions, the route
> `docs/throughput-study-2026-09-16.md` names for a 2–3× at the tiers
> below `--max`).
> Items 3, 4, 6, 7, 9, 10, 12, 13, 15, 16 and 17 stand as written.
>
> **Status, 2026-09-15.** Landed since: item 1 (segments on threads, 510aa16),
> item 14 (parallel segment decoding, the same commit). Measured and rejected:
> item 2 (branch-free run and recall lanes: bytes identical, branch misses
> −8.6 %, instructions +3.9 %, no cycle win in interleaved runs). Since then
> the text transform (e72f3e8) cut the bytes the model codes on text by about
> 38 %, which moved every time floor (`tools/ratio_floor.json`); the
> per-bit numbers below are the state of 085dff0.

<!-- Authored with Claude Opus 5 (Claude Code), the LTCB session. Every
number is from a run on the host in Baseline unless it names the earlier audit
(docs/performance-audit-2026-09-14.md); corpus citations name the doc-ray
document and the sentence ordinal they were read from. No code was changed
by this audit. -->

The current bottleneck is the instruction count of the per-bit model: the
max-profile encoder retires **2,676 instructions per coded bit at IPC 2.27**
(1,181 cycles a bit), 78 % of cycles in `predict_max_v3` and `update_max_v3`,
with the memory system already mostly hidden behind the prefetch plan on one
core — and *not* hidden once cores are added: six concurrent encoders reach
only **3.46×** on this host because each one's IPC falls to 1.39 at unchanged
instruction and miss counts. The single highest-leverage change is
**16-bit arbiter weights with packed multiply-add** (item 8): it cuts the
dominant instruction stream (the four 72-lane dot products and their four
trains) by roughly half and the per-bit DRAM traffic by a third, which is
also what lifts the parallel ceiling — but it changes the bytes, so under the
operator's zero-degradation lock it lands only after it shows an equal or
smaller container on both locked inputs.

## Baseline

**Hardware.** AMD Ryzen 5 5600X (Zen 3): 6 cores / 12 SMT threads, one CCX;
L1d 32 KiB and L2 512 KiB per core, L3 32 MiB shared; 125 GiB DDR4 (dual
channel); NVMe (ext4); ISA x86-64-v3 (AVX2, FMA, BMI2), **no AVX-512**;
transparent huge pages `madvise` (the codec's `MADV_HUGEPAGE` is honoured:
0.58 dTLB misses per *byte*); Linux 7.0.11; GCC 13.3; `perf` with
`perf_event_paranoid = 2` (user-space counters only).

**Workload.** The locked command `cadapt encode --max --allocation context`
on enwik8 (100 MB, English Wikipedia XML, ~1.5 bits/byte coded) and enwik9
(1 GB); the CI proxy is the first 8 MiB of enwik8 (`tools/ratio_floor.json`).
Single process, one thread; `--verify` adds one decoder thread. The
compressed-data mix beyond Wikipedia is **not** characterised here
(assumption **A1**: text-like inputs; the other audit's eight-file mix showed
the same profile shape).

**Ratio floor.** X was left blank in the brief. LOCKS.md (2026-09-14) locks
the containers at **18,564,510 B (enwik8) and 155,906,798 B (enwik9)** as
floors that may only improve, so for the locked command **X = 0 %**. Any item
below that changes bytes must show ≤ the locked size on both inputs, or land
as a separate mode (`--threads`, a new profile) with its own lock
(assumption **A2**: the operator will accept a ratio cost only in a mode
that is not the locked command).

**Current measurements** (HEAD `085dff0` built from its own sources with
`-O3 -g`, bytes identical to the locked binary's on the proxy, 1,716,715 B).

| Measure | Value | Source |
|---|---|---|
| Throughput, max, 8 MiB proxy | 0.47 MB/s (17.8 s, quiet host) | `perf stat`, this audit |
| Throughput, max, enwik8 | 380 KB/s locked (263 s); 452–506 KB/s on today's builds (221 s verified round trip, 197 s dev build) | floor file; `tools/check_ratio_floor.py`; `_tmp` run |
| Throughput, max, enwik9 | 320 KB/s encode (3,127 s), 292 KB/s decode (3,419 s) | operator run, decode verified by this session |
| Peak RSS | 659 MB (674 MiB budget: 512 MiB table, 128 MiB window, 2 × 19 MiB weight tables, 17 MiB APM2, …) | `/usr/bin/time` |
| CPU | 99 % of one core; `sys` 1 % (0.18 s / 17.8 s) | `perf stat` |
| Ratio | 18.56 % enwik8, 15.59 % enwik9 | locked |
| Cycles / instructions per bit | 1,181 / 2,676, IPC 2.27 | `perf stat`, 8 MiB |
| Branch misses | 6.6 per bit (3.2 %), ≈ 9 % of cycles at ~17 cycles each | `perf stat` |
| L1d load misses | 78 per bit (7.9 % of 990 loads/bit) | `perf stat` |
| LLC misses | 29 per bit ≈ 15 KB of DRAM traffic per coded byte ≈ **7 GB/s** at 0.47 MB/s | `perf stat` (`cache-misses:u`) |
| Profile (self) | `predict_max_v3` 42.9 %, `update_max_v3` 35.0 %, `start_byte` 10.9 %, `prefetch_byte` 4.2 %, `end_byte` 3.7 %, `code_block` (coder) 2.0 %, `model_new` 0.6 %, `memmove` 0.4 % | `perf record`, 18,146 samples |
| Hottest lines | predict: `dot4_v3` (`vpaddq`/`vpmuldq`/`vpsrlq`, ~27 % of predict), `context_lanes_v3` cell inserts (9.7 %), APM row loads (~11 %); update: `memories_learn_v3` (8.7 %), `claim` tag compare (5.8 %), `claim_chain` (5.0 %); start_byte: `claim` (16.7 %) and `claim_chain` (16.3 %) | `perf annotate -l` |
| **Concurrency ceiling** | 1 encoder 0.45 MB/s; 3 concurrent 1.20 MB/s aggregate (2.67×); **6 concurrent 1.56 MB/s (3.46×)**, per-process cycles +63 % with instructions and LLC-miss *counts* unchanged (IPC 2.27 → 1.39) | 8 MiB proxy, host at load ≈ 2 with a paq8px job on one core |
| Noise floor | 1.5 % on wall time (earlier audit, repeats); this host is shared, so anything under 2 % is treated as noise below | — |

The profiler ran; the ranking below is by measured hotspots, and the
expected effects are derived from the counters above, not from inspection.
Items whose effect depends on A1 or A2 say so.

## What is already landed, rejected or measured (do not redo)

`docs/performance-audit-2026-09-14.md` (the other session, same day) landed,
bytes-identical: the branch-free `claim`, the x86-64-v3 instantiations with
the hand-written even/odd `vpmuldq` dot products, the planar lanes and
vector learning, the next-bit arbiter/APM prefetch in `update`, the byte-level
plan (`prefetch_byte`/`nibble_plan`/`issue_planned`), the stamp-table alias
filter. Together: 143.5 → ≈ 80 G cycles on the proxy (this audit's count)
and 316.7 → 194 s on enwik8. It measured and rejected: a fused per-bit body
(+7 %), a scout thread, `prefetcht1`, gathers for stretch/rate, fused trains,
earlier claim prefetches (L2-bound), per-cell lane words, PGO (noise). It
measured **parallel segments at +4.1 % ratio** (16 MiB segments, per-worker
budget) and **+7.0 %** (six workers sharing one budget). Those results are
taken as given here and cited as *PA-2026-09-14*.

## Ranked opportunities

Rank = expected gain ÷ (effort × risk), against the objective order:
throughput at fixed ratio, then RSS per worker, then host impact, then the
ratio floor. "Bytes-identical" means the container is unchanged, so the floor
is met by construction. Effort: XS < S < M < L < XL.

### Land now

**1. Parallel segment encoding, one model per worker, ordered writes, worker count capped by measured memory bandwidth.**
- *Current:* `adapt_encode.c:encode_blocks` codes segments in order on the calling thread; `--threads n` only sizes memory (`adapt_sizes_for_threads`) and cuts `ADAPT_SEGMENT_BYTES` = 16 MiB segments (`adapt.h:81`), each starting a fresh model (`model_free`/`model_new` at the marker). The only worker thread today is the verifier (`verifier_thread`).
- *Mechanism:* shared-nothing workers — each with its own arena, block buffers and coder — encoding segment *k* into a private buffer; the writer drains buffers in order; no lock on the per-bit path. Cores, not caches, are the resource: the encoder's working set is private.
- *Expected effect:* on this host, measured, **2.67× at 3 workers and 3.46× at 6** (aggregate 1.56 MB/s) on the proxy: the per-worker IPC drop is the memory system saturating (≈ 7 GB/s per worker, six of them exceed what dual-channel DDR4 delivers with an L3 shared six ways). A 12-worker SMT run would be worse per worker (Systems Performance, 2nd ed., ord. 2985–2986: a hardware thread "is not the same as a separate CPU core"). The cap must therefore be `min(cores, bandwidth ÷ per-worker traffic)`, measured at start-up from a 2-second calibration, not `nproc`. Items 5, 7 and 8 raise the ceiling by cutting traffic.
- *Cost:* M. POSIX threads are already in the build (`ADAPT_THREADS`); Windows threads absent. RSS ×n (each worker 659 MB at `--max`): priority 2 gives way to priority 1 per the brief.
- *Risk:* **ratio −4.1 % at 16 MiB segments** (PA-2026-09-14), which is a different command from the locked one but exceeds any small X; 64 MiB segments should cut the restart share to ≈ 1–1.5 % (six restarts on enwik8 cost 4.1 %, so ≈ 0.7 % per restart — an estimate, unmeasured, A2). Decoding stays sequential until item 14. Verified mode needs a verifier per worker.
- *Measurement:* `cadapt encode --threads {1,3,6} --segment-bytes {16,64}MiB enwik8`: wall, aggregate MB/s, container bytes vs 18,564,510, `cmp` of the decode; the 6-worker aggregate is the number that decides whether items 7/8 must precede it.
- *Evidence:* `adapt_encode.c:encode_blocks` (1133–1340), `adapt.h:81`; this audit's 1/3/6-worker measurement; PA-2026-09-14 item 1; Systems Performance ord. 2985–2986.

**2. Branch-free run and recall lanes.**
- *Current:* `adapt_model.h:predict_impl` (1870–1909) tests, per bit and per run model (six under max) plus the recall model, `expected_prefix == (c0 ^ (1 << bitpos))` and `rcnt > 0` / `mlen > 0 && !mmiss` with data-dependent branches; `update_impl` (2127–2135) repeats the tests. The counters show 6.6 mispredictions per bit — 3.2 % of branches, ≈ 112 cycles a bit, ≈ 9 % of all cycles — and the earlier audit's rejection of a single `MIXER_SKIP` fix noted the misses are "spread evenly". PA-2026-09-14 names "the runs without their data-dependent branches" as an open bytes-identical candidate.
- *Mechanism:* compute every run lane unconditionally with masks (`-(prefix == expected)` AND the stretched value), and fold the eight run/recall compares into one vector compare over a 16-lane `int16` (`vpcmpeqw`, `vpand`), so the per-bit path has no data-dependent branch between `context_lanes` and the dot products.
- *Expected effect:* bounded by the 9 % branch-miss share; the run/recall sites are seven of the roughly a dozen data-dependent branches per bit, so **2–4 %** of cycles if their miss share is proportional (estimate; `perf annotate` on `predict_max_v3` after the change decides).
- *Cost:* S. Bytes-identical (the lane values are unchanged; only control flow moves to data flow).
- *Risk:* more instructions on bytes where every run is inactive (they were skipped); the fast profile's `RUN_ON` gating must stay compile-time.
- *Measurement:* `perf stat -e branch-misses:u,cycles:u` on the proxy, interleaved with HEAD; expect ≥ 1.5 fewer misses per bit and ≥ 2 % fewer cycles, `cmp` against 1,716,715 B.
- *Evidence:* `adapt_model.h:predict_impl` 1870–1909, `update_impl` 2127–2135; `perf stat` branch counters above; PA-2026-09-14 "remaining candidates".

**3. Worker and verifier placement: one worker per physical core, `SCHED_BATCH` for the proof thread.**
- *Current:* pthreads are unbound; the verifier (`adapt_encode.c:verifier_thread`) competes at normal priority; nothing prevents two workers on SMT siblings (0+6, 1+7, …).
- *Mechanism:* `pthread_setaffinity_np` to distinct physical cores for workers (siblings share L1/L2 and the front end, and this codec's working set already misses L1 78 times a bit), `SCHED_BATCH` on the verifier so an interactive host is not stolen from; both are host-impact controls (priority 3).
- *Expected effect:* throughput: no change on an idle host; on a loaded host it prevents the 1.74× per-process slowdown seen at six workers from being worsened by sibling placement (estimate; the SMT-pair configuration was not run). Host impact: the verifier stops preempting foreground work.
- *Cost:* XS; Linux-only under `#ifdef __linux__`.
- *Risk:* pinning fights the kernel scheduler on a heterogeneous or busy host; expose as `--pin`, off by default.
- *Measurement:* 6 workers pinned to 6 cores vs pinned to 3 sibling pairs vs unpinned: aggregate MB/s; `perf stat` IPC per worker.
- *Evidence:* `adapt_encode.c:verifier_thread`, `cadapt.c` (no affinity calls); Systems Performance ord. 2985–2986 (hardware threads) and 2438 (padding per-line state — relevant once workers share a writer queue).

**4. `posix_fadvise` on input and output.**
- *Current:* `cadapt.c` reads with `fread` (684) and writes through a staged `FILE` (695, `staged_open`); every input and output page stays in the page cache after a one-pass encode — ≈ 1.2 GB for an enwik9 run.
- *Mechanism:* `POSIX_FADV_SEQUENTIAL` on the input and `POSIX_FADV_DONTNEED` behind each finished 256 KiB block on both files: the kernel drops clean pages instead of aging them out of other processes' working sets.
- *Expected effect:* priority 3 only: page-cache growth during an enwik9 encode from ≈ 1.2 GB to ≈ 0; throughput unchanged (I/O is 1 % of time; the earlier audit counted 181 syscalls per 8 MiB, 34 reads and 66 writes).
- *Cost:* XS; POSIX-only.
- *Risk:* `--verify`'s second pass re-reads the input; skip DONTNEED on the input when a second pass is coming. Item 10 (mmap-backed history) wants the input *kept* — the two are alternatives on the input side.
- *Measurement:* `vmstat`/`free` cache column during an enwik9 encode; wall unchanged within noise.
- *Evidence:* `cadapt.c:684, 695, 763–818`; Systems Performance ord. 4557 and AI Systems Performance Engineering ord. 1256 (page-cache stalls and the hints that avoid them); PA-2026-09-14 item 5.

### Land after measurement

**5. Line-aligned strides for the arbiter weight rows and the estimation rows.**
- *Current:* `adapt_model.h:model_layout` (1088–1112) packs weight rows at `ARBITER_STRIDE = 73` int32 = 292 B and APM rows at 33 × uint16 = 66 B. A 292-B row starting at an arbitrary 4-byte offset covers 5 lines in 45 % of positions and 6 in 55 % (mean 5.55); the four rows read and written per bit (`dot4_v3`, then four `train`s) are the bulk of the 29 LLC misses a bit.
- *Mechanism:* pad the stride to 320 B (5 lines) with the row 64-byte aligned, and the APM/APM2 rows to 128 B, so every row is a whole number of lines: fewer lines fetched, fewer fill buffers per prefetch burst (`update_impl` 2054–2071 already prefetches a row's lines one by one), and no partial-line write-backs.
- *Expected effect:* weight lines per bit 22 → 20 (−10 %), APM 4 → 4; ≈ 2 fewer LLC misses a bit of 29 → **1–3 %** single-thread, more under six-way contention where those misses cost +63 % (estimate from the line arithmetic; below noise is possible, hence this group).
- *Cost:* XS. Bytes-identical (layout only). RSS: weight tables +10 % (2 × 19 → 2 × 21 MiB), APM2 +6 MiB — a priority-2 cost that priority 1 outranks.
- *Risk:* none functional; the memory-bound check `adapt_model_bytes` must include the padding.
- *Measurement:* `perf stat -e cache-misses:u` per bit before/after on the proxy; the 6-worker aggregate.
- *Evidence:* `adapt_model.h:model_layout`, `ARBITER_STRIDE` (766), `APM_BUCKETS` (254); the LLC-miss count above; PA-2026-09-14 item 7 (APM rows only).

**6. Drop the 16 KiB `stretch32` copy: gather from the `int16` stretch table with scale 2.**
- *Current:* `model` carries `stretch[4096]` (int16, 8 KiB) *and* `stretch32[4096]` (int32, 16 KiB) because `context_lanes_v3` gathers 32-bit words (`_mm256_i32gather_epi32(m->stretch32, p, 4)`); with `squashed` (8 KiB), `maps` (21 KiB), `rate`, `mixer_rate` and the per-bit lane arrays, the hot working set is over the 32 KiB L1d, and the counters show 78 L1d misses a bit.
- *Mechanism:* gather with scale 2 from the int16 table (`_mm256_i32gather_epi32(stretch, p, 2)` reads two adjacent entries; `vpslld 16` + `vpsrad 16` keeps the low one), removing 16 KiB from the working set; the same trick applies to `squashed`.
- *Expected effect:* L1d misses are the mechanism, not instructions: **0–2 %** (unquantifiable without trying; the earlier audit found gathers vs inserts a wash, so the gather itself is not the cost).
- *Cost:* XS. Bytes-identical.
- *Risk:* the extra shift pair adds two instructions per gather (three per bit); if L1 misses do not fall, it is a loss.
- *Measurement:* `perf stat -e L1-dcache-load-misses:u` per bit; keep only if misses fall ≥ 5 % and cycles ≥ 2 %.
- *Evidence:* `adapt_model.h` model struct (`stretch`, `stretch32`), `context_lanes_v3` (1749–1795); L1 counters above.

**7. Direct-indexed order-1 and order-2 memories under the max profile.**
- *Current:* under `--max` all twenty contexts, including o1 and o2, are hashed into the 512 MiB table (`hash_kinds`, `claim_chain`): two of the ~20 random DRAM lines per nibble belong to contexts whose whole space fits in L2 (o1: 256 × 16 nibble blocks = 64 KiB) or L3 (o2: 65,536 × 16 × 16 B = 16 MiB). The fast profile already has a direct `o1[O1_BLOCKS][16]` (`o1_blk`).
- *Mechanism:* give o1 and o2 their own direct tables (no tag, no eviction), so their claims never miss to DRAM and never evict a higher-order context's block from the shared table.
- *Expected effect:* 2 of ~40 claim lines per byte leave DRAM (−5 % of claim traffic, −1 % of LLC misses) and the shared table gains capacity — **1–2 % throughput, ratio likely better** (no collisions for the two most-hit contexts; estimate).
- *Cost:* S–M; bytes change (different collision history), so the floor check runs on both locked inputs. RSS +16 MiB.
- *Risk:* the ratio might not improve on non-text inputs (A1); the vector path assumes all slots share one block layout — a direct table is still 16-byte blocks, so `cbase` slots still work.
- *Measurement:* enwik8/enwik9 container vs floor; `perf stat` cache-misses per bit.
- *Evidence:* `adapt_model.h:hash_kinds` (1398), `claim_chain` (1224), `o1_blk` (1253); Designing Data-Intensive Applications ord. 2254 (sequential over random access, tight loops).

**8. 16-bit arbiter weights and inputs with packed multiply-add (`vpmaddwd`).**
- *Current:* `adapt_model.h:dot_v3`/`dot4_v3` (804–876) accumulate `int32 × int32 → int64` with `vpmuldq`, which multiplies **4 lanes per 256-bit instruction** and needs the even/odd shift split; the four 72-lane dots are the top of the profile (`vpaddq`/`vpmuldq`/`vpsrlq` ≈ 27 % of `predict`), and `train` (891) updates 4 × 73 int32 weights a bit with `vpmulld` (2 µops, 10-cycle latency on Zen 3). Weight rows are 292 B.
- *Mechanism:* int16 weights and stretched inputs (12-bit stretch fits; weights scaled to 16 bits as PAQ8/lpaq do) so `vpmaddwd` does **16 products and 8 adds per instruction** (4× the MAC density), `vpmullw`/`vpmulhw` (1 µop) trains, and every weight row halves to 146 B (3 lines aligned, item 5) — the same lever the corpus states for lower precision: "each drop in precision doubles or quadruples the number of operations per byte" (AI Systems Performance Engineering ord. 5337) and "lower precision reduces memory and increases throughput" (ord. 12248, 3619).
- *Expected effect:* dot products ≈ 12–15 % of cycles → ≈ 4 %; trains ≈ 5 % → ≈ 2 %; weight lines per bit 20 → 12, LLC misses 29 → ≈ 21 (−28 %), so the memory-stall share and the six-worker ceiling both improve: **10–16 % single-thread** (estimate from the shares above) and a higher parallel aggregate (the concurrency test is the confirming measurement).
- *Cost:* L. **Bytes change** (16-bit rounding differs), so under X = 0 it lands only if enwik8 *and* enwik9 come out ≤ the locked sizes — PAQ-class codecs run 16-bit mixers at no measured loss, but this codec's fixed-point scaling (`>> 10` in `train`, `dot_clamp`) has to be re-derived and the floor re-checked; a new format version and decoder support.
- *Risk:* saturation in 16-bit accumulators (use `vpmaddwd` into 32-bit lanes, 8 accumulators as now); ratio drift on adversarial inputs; the portable scalar path must produce the same bytes as the vector path (the existing `ADAPT_ISA=baseline` test covers it).
- *Measurement:* `perf stat` instructions and cycles per bit on the proxy; the two locked-input sizes; the 1/3/6-worker aggregate.
- *Evidence:* `adapt_model.h:dot_v3`, `dot4_v3`, `train`, `ARBITER_STRIDE`; `perf annotate` lines `avx2intrin.h:128/526/789`; AI Systems Performance Engineering ord. 3619, 5337, 12248; PA-2026-09-14 item 13.

**9. Co-locate a byte's two nibble blocks in one 256-byte region.**
- *Current:* at the nibble boundary `update_impl` (2141–2148) re-hashes every context (`h[i] = mix(h[i] + salt(hi))`) to a fresh random line, so a byte costs ~20 first-nibble lines *and* ~20 second-nibble lines; the encoder plans the exact second-nibble lines because it knows the byte (`nibble_plan`), the decoder does not and can only prefetch both candidates at bit 3 — its `start_byte`/claim share is the reason it runs ≈ 9 % slower than the encoder (3,419 vs 3,127 s on enwik9).
- *Mechanism:* map each context's second-nibble block into a 256-B region (four lines) adjacent to its first-nibble line, addressed by the high nibble as a 16-way set within the region; then the decoder prefetches the whole region at byte start (four sequential lines, which the L2 adjacent-line prefetcher also helps) without knowing the byte.
- *Expected effect:* decoder: the second-nibble claim stalls vanish, **≈ 4–6 %** of decode cycles (bounded by the 9 % decoder/encoder gap; estimate); encoder ≈ 0.
- *Cost:* M; bytes change (different placement and associativity), floor check on both inputs, new format version.
- *Risk:* the 16-way-within-256-B set trades global associativity for locality; collision behaviour changes and could cost ratio on inputs with many high-order contexts (A1).
- *Measurement:* decode wall on the enwik8 container vs HEAD (`cmp` of the restored file); `perf stat` on `cadapt decode`.
- *Evidence:* `adapt_model.h:update_impl` nibble re-hash (2141–2148), `nibble_plan`/`issue_planned` (2024–2043), `prefetch_byte` (1548); the enwik9 encode/decode timings above.

**10. History window backed by the input file (encoder), not a 128 MiB ring.**
- *Current:* `model_layout` places `hist` as `1 << window_log` = 128 MiB at `--max` (`WINDOW_LOG_MAX` 30 allows 1 GiB); the encoder copies every byte into it (`end_byte`: `m->hist[n & hmask] = b`) although the same bytes sit in the input file the CLI just read.
- *Mechanism:* when the input is a seekable file, `mmap` it `PROT_READ | MAP_PRIVATE` and point the recall and record contexts at it (the window then costs page-cache pages, shared with the reader, not private RSS); keep the ring for pipes and for the decoder.
- *Expected effect:* peak RSS per worker **659 → ≈ 531 MB (−19 %)**, bytes-identical; six workers −768 MB. Throughput unchanged (the window reads are the recall model's, prefetched already).
- *Cost:* M; `mmap`/`CreateFileMapping` paths; the `--verify` reader and `x86`/BMP transforms read from `orig`, which stays.
- *Risk:* a file modified during the encode changes the history under the model (the CRC in the trailer catches it only at decode); pipes fall back to the ring; page-cache pressure grows if item 4 evicts the input pages the window still needs — order the two.
- *Measurement:* `/usr/bin/time -v` peak RSS; container `cmp`-identical.
- *Evidence:* `adapt_model.h:model_layout` (`hist`), `end_byte` (1603–), `cadapt.c:684`; PA-2026-09-14 item 15 (the in-memory API variant).

**11. Start-up model trials scaled to the input length.**
- *Current:* `adapt_encode.c:choose_model` (637) codes the first 32 KiB up to three times to pick a context set before the real encode; `model_new` is 2.8 % of a 1 MiB run and 0.6 % of 8 MiB; the earlier audit measured 50 KB at 0.28 MB/s against 1 MB at 0.62 — ≈ 0.5 s fixed per file.
- *Mechanism:* size the trial sample to the input (skip trials under a threshold; the default set for tiny inputs), or run the trials on the first block only as far as the choice is decided.
- *Expected effect:* small-file workloads only: up to **2× on 50–200 KB files**; nothing on the locked inputs (their choice is the same).
- *Cost:* S; bytes may change for small inputs (a different chosen set), so a small-file ratio check is part of landing it.
- *Risk:* a wrong set on a short input costs ratio where it is most visible; A1.
- *Measurement:* wall and bytes for a 50 KB / 200 KB / 1 MB file mix.
- *Evidence:* `adapt_encode.c:choose_model`, `trial` (605); the earlier audit's fixed-cost measurements; PA-2026-09-14 item 3.

**12. Verified encode with both models inside the budget.**
- *Current:* `adapt_encode_stream_verified` runs the decoder thread alongside only when two models fit `max_memory`; at `-9` on large inputs they do not, so the proof becomes a second pass and a verified enwik8 takes ≈ 2× wall (LOCKS.md, memory-profile entry).
- *Mechanism:* either count the verifier's arena outside the user's budget (policy: `--verify` doubles RSS, said plainly) or fit both models to half each (ratio cost, A2). The first keeps bytes identical.
- *Expected effect:* verified-mode throughput **≈ 2×** (one pass instead of two); RSS ×2 during the run.
- *Cost:* S (policy and CLI text).
- *Risk:* RSS doubling on a small host; make it opt-in (`--verify=alongside`).
- *Measurement:* `cadapt encode --verify` wall on enwik8 vs unverified; peak RSS.
- *Evidence:* `adapt_encode.c:adapt_encode_stream_verified` (1445), `verifier_init` (868); LOCKS.md 2026-09-14 memory-profile entry; PA-2026-09-14 item 9.

**13. Detect missing huge pages and fall back to `MAP_HUGETLB`.**
- *Current:* `model_arena` (1114) maps the arena and calls `madvise(MADV_HUGEPAGE)`, which the kernel may decline (`transparent_hugepage = never`, or fragmentation with `defrag` off); the code's own note records **0.77 dTLB misses per coded bit** without huge pages — today's run shows 0.58 per *byte*, i.e. the hint is honoured here.
- *Mechanism:* after mapping, read `AnonHugePages` for the arena from `/proc/self/smaps`; if under half the arena, retry with `MAP_HUGETLB` when the pool allows, else print a one-line warning naming the expected slowdown.
- *Expected effect:* on a host where THP is off, avoids a **≈ 15–25 %** slowdown (0.77 misses a bit × ~30 cycles ≈ 23 cycles of 1,181 with page walks partly overlapped — an estimate from the code's own figure; zero on this host). Systems Performance, ord. 4047: larger pages increase TLB reach "which reduces TLB misses and improves system performance".
- *Cost:* XS; Linux-only.
- *Risk:* `MAP_HUGETLB` needs a reserved pool; the fallback must be silent when unavailable.
- *Measurement:* `perf stat -e dTLB-load-misses:u` with THP set to `never` (needs root) vs `madvise`.
- *Evidence:* `adapt_model.h:model_arena`, header note (79–90); `perf stat` dTLB counters; Systems Performance ord. 4047; PA-2026-09-14 item 12.

**14. Parallel segment decoding.**
- *Current:* `adapt_decode.c` decodes segments in order; the segment table at the end of the container (`counter_table`) already records every segment's coded length, so the offsets are known before decoding starts.
- *Mechanism:* the mirror of item 1: one decoder model per worker, outputs assembled in order (or written with `pwrite` at known offsets, since raw lengths are in the table too).
- *Expected effect:* decode wall ÷ ≈ 3.4 at six workers on this host (same memory ceiling as encode); nothing on single-segment containers.
- *Cost:* M; depends on item 1's container layout; RSS ×n.
- *Risk:* the CRC and the byte-exact compare must cover the assembled output, not each segment.
- *Measurement:* `cadapt decode --threads 6` wall and `cmp` on the enwik8 container from item 1.
- *Evidence:* `adapt_decode.c`, `adapt_model.h:counter_table` (2753); PA-2026-09-14 item 8.

**15. Prediction-guided decoder prefetch from the recall and run models.**
- *Current:* the decoder's byte-level plan needs the byte (`coding_known`); without it `update_impl` issues both candidates' lines at bit 3 and both next bytes' at bit 7 (2183–2197), forty-plus lines in the last bit, more than the core's fill buffers carry (the code's own comment at 1563–1565). But when the recall model has a match (`mlen > 0`) or a run model has a byte (`rcnt > 0`), the model already holds an *expected* byte (`mbyte`, `rbyte`).
- *Mechanism:* at byte start, when `mbyte`/`rbyte` is available, plan that byte's lines exactly as the encoder does (`prefetch_byte(m, mbyte, …)`); a wrong guess costs nothing but bandwidth, a right one gives the decoder the encoder's plan.
- *Expected effect:* on text, the recall model is active on a large share of bytes and right most of the time when active (that is why it earns its lanes); **2–5 % of decode cycles** (estimate; bounded by the decoder's 9 % gap less item 9's share).
- *Cost:* S. Bytes-identical (prefetch only; the shadow-check build proves no model effect).
- *Risk:* extra traffic on inputs where the guess is usually wrong (binary, A1); gate it on the match length.
- *Measurement:* decode wall on enwik8, `perf stat` cache-misses on `cadapt decode`.
- *Evidence:* `adapt_model.h:update_impl` (2172–2197), `prefetch_byte` (1548–1597), the `mbyte`/`rbyte` state in `model`; Advanced Compiler Design and Implementation ord. 6684 ("prefetching does not reduce the latency … it hides it by overlapping it").

**16. Whole-program x86-64-v3 build for the reference host.**
- *Current:* only `predict`/`update` are instantiated for v3 (`ADAPT_TARGET_V3`); `start_byte`, `end_byte`, `prefetch_byte`, `hash_kinds`, `claim_chain` and the coder — 21 % of cycles — compile for baseline x86-64 (no BMI2 shifts, no AVX2 auto-vectorisation of the small loops). The header says a `-march=x86-64-v3` build "gets the same code without the dispatch".
- *Mechanism:* a second build variant (`CFLAGS += -march=x86-64-v3`) for hosts that have it, selected by the CLI wrapper or the package.
- *Expected effect:* **1–2 %** (a fraction of the 21 % outside the instantiated pair; estimate) — at the noise floor, hence this group.
- *Cost:* XS. Bytes-identical (integer code).
- *Risk:* a binary that traps on pre-Haswell hosts; keep the dispatching build as the default.
- *Measurement:* interleaved `perf stat` cycles on the proxy, ≥ 3 repeats each; keep only if ≥ 2 %.
- *Evidence:* `adapt_model.h` 52–71; `perf report` shares of the non-instantiated functions.

### Requires architectural change

**17. Two independent streams interleaved bit-by-bit on one core.**
- *Current:* one model, one dependency chain per bit (`predict → code → update`); each bit's claims and rows must arrive before the next bit can proceed, and six-way contention showed the same misses costing +63 % cycles — latency is exposed whenever the plan is late.
- *Mechanism:* software pipelining across two segments (Advanced Compiler Design and Implementation §17.4.3 / §20, ord. 5379–5412 and 6703–6710, cited in PA-2026-09-14): interleave two models' bit steps so one stream's misses overlap the other's arithmetic, doubling memory-level parallelism without threads.
- *Expected effect:* bounded by the stalled share, which at IPC 2.27 is small on an idle core — **≈ 5–15 %** single-core (estimate), most of it under contention; costs the same ratio as item 1 (two segments).
- *Cost:* XL: the whole per-bit path rewritten for two contexts, doubled L1/L2 working set, register pressure the earlier fused-body attempt already lost to (+7 %).
- *Risk:* high; likely a loss on an idle core.
- *Measurement:* only worth attempting if item 8's traffic cut fails to raise the six-worker aggregate.
- *Evidence:* the concurrency counters above; Muchnick ord. 6684, 6703–6710.

## Top three, in order

1. **Item 2 (branch-free run/recall lanes)** — bytes-identical, S, attacks the one measured cost the landed work left untouched (6.6 mispredictions a bit); land first because it changes `predict_impl` and everything after it must be measured on the settled path.
2. **Item 1 (parallel segments) with item 3 (placement)** — the only multiplicative lever; second, not first, because on this host it is worth 3.46×, not 6×, until the per-worker traffic drops, and because it needs the operator's A2 decision (a `--threads` mode with its own ratio lock at 64 MiB segments).
3. **Item 8 (16-bit weights)** — the largest single-thread gain and the change that raises item 1's ceiling; last of the three because it changes bytes and therefore needs the floor re-run on enwik9 (52 min + 57 min verify) before it can be called a gain at all.

Items 5 and 6 ride along with 8 (they share the row layout); 9 and 15 are the decoder's pair; 10 is the RSS item when priority 2 comes up.

## Rejected candidates

| Candidate | Reason |
|---|---|
| Table-lookup or single-multiply binary coder (Sayood, *Introduction to Data Compression*, ord. 2673: the M coder "replaced [the multiply] with a table lookup") | `code_block` is 2.0 % of cycles; a perfect coder saves under the 1.5 % noise floor. |
| 64-bit range coder renormalising four bytes at once (`arith_encode` 672–686) | Same 2 % budget. |
| PPM / BWT / dictionary front ends (Sayood ord. 3739 ppma; corpus) | Different ratio class; out of scope at X = 0. |
| Fused per-profile bit body | Measured +7 % by PA-2026-09-14. |
| Scout / helper thread prefetching ahead | Measured no gain by PA-2026-09-14. |
| Earlier or more nibble-boundary prefetches | Measured no gain (L2-bound) by PA-2026-09-14; the residual claim cost is bandwidth, which items 5/7/8 address. |
| Vectorised `map_update` via gather/scatter | Rejected with measurement by PA-2026-09-14 (gather cost on Zen 3). |
| PGO (`-fprofile-use`) | Measured within noise by PA-2026-09-14. |
| Reusing the encoder's planned hashes in `start_byte` instead of recomputing `hash_kinds` | ≈ 600 of 21,410 instructions a byte ≈ 1.4 % — below the noise floor; XS but unmeasurable. |
| Vector path for aliased nibbles | Aliasing needs two of ≤ 20 hashes in one of 2^23 lines *and* both missing: ≈ 2 × 10⁻⁵ per nibble at the locked table size; the scalar fallback is never the cost. |
| `mmap`/`O_DIRECT`/`io_uring` input, larger stdio buffers, a read-ahead thread | I/O is 1 % of time, 181 syscalls per 8 MiB (PA-2026-09-14); the corpus recommends io_uring for *random* reads (AI Systems Performance Engineering ord. 2658), which this is not. |
| Larger `ADAPT_BLOCK_BYTES` | Per-block work (`entropy_256`, framing, verifier hand-off) is under 0.5 %. |
| Reusing the arena across segments instead of `model_free`/`model_new` | 674 MiB re-faulted per 16 MiB segment ≈ 0.1 s of a 35 s segment ≈ 0.3 %. |
| `model_new` / APM initialisation | 0.6 % of an 8 MiB run, 0 of a 100 MB one. |
| Twelve SMT workers | Six already saturate the memory system (measured); siblings share L1/L2 and the front end (Systems Performance ord. 2985). |
| `prefetchw` for claim lines | Lines are single-owner; no RFO to avoid. |
| Carrying arbiter weights across segments, or shipping a primed model | Serialises decoding (breaks item 14) or adds tens of MB to the decompressor and violates the repository's no-corpus-dictionary rule (`FMT-2`). |
| Shrinking the window, table or APM2 for RSS | Parameter changes with a ratio cost; the lock forbids them for the locked command. |
| khugepaged/THP compaction cost | `sys` is 1 % of run time on this host. |

## The measurement that would invalidate this ranking

The ranking rests on the six-worker experiment reading as a memory-system
limit — equal instruction and miss *counts*, IPC 2.27 → 1.39 — which is what
makes traffic reduction (items 5, 7, 8) the enabler of the multiplicative
lever. The test: run six concurrent encoders with the arbiter tables pinned
to an L3-resident size (2 MiB, a throw-away container) and compare the
per-process cycles with the 512 MiB run. If the six-way slowdown stays near
+63 % with the tables cache-resident, the ceiling is not DRAM bandwidth but
L3 capacity or the fabric, and cutting weight traffic will not raise the
aggregate: item 8 drops to a single-thread gain of 10–16 %, item 17 becomes
the only lever for the parallel case, and item 1 should be run at three
workers, where it already delivers 2.67×.
