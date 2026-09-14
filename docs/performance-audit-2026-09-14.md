# Performance audit — cadapt (format 13–15), 2026-09-14

<!-- Authored with Claude Opus 5 (Claude Code). Every number below is from a
run on the host named in Baseline; corpus citations name the doc-ray document
and sentence ordinal they were read from. -->

The current bottleneck is instruction count in the per-bit model, not memory: at
IPC 2.48 the encoder retires ~1,300 instructions per coded bit, 75 % of them in
`predict` and `update`, and the only measurable memory stall is the first touch of
each context's cache line in `start_byte` (14 % of cycles). The single
highest-leverage change within a 1 % ratio floor is the byte-boundary prefetch
(item 2, bytes-identical, an estimated 4–7 %); the only multiplicative step —
parallel segment encoding, which the format already supports and the CLI runs
sequentially — was measured at a **4.1 % ratio cost** on 100 MB of enwik8 with
16 MiB segments (7.0 % when six workers share the default budget), so it lands
only if the operator's floor admits that, and buys nothing below 16 MiB.

## Baseline

**Hardware.** AMD Ryzen 5 5600X: 6 cores / 12 threads (SMT siblings 0+6, 1+7 …),
one CCX; L1d 32 KB and L2 512 KB per core, L3 32 MB shared; 125 GB RAM; NVMe,
ext4; ISA: AVX2, FMA, BMI2, ADX — no AVX-512; transparent huge pages `madvise`
(the codec's `MADV_HUGEPAGE` is honoured); Linux 7.0.11; perf 7.1.5.

**Workload.** Eight files, 22.6 MB (prose 1.1 MB, word list 1.0 MB, CSV 3.0 MB,
XML 2.5 MB, JSON 0.9 MB, libc 2.1 MB, python 4.2 MB, enwik8 first 8 MiB); the
full 100 MB enwik8 for the segment measurement. Order-0 entropies range from
0.37 bpB coded (JSON) to 2.53 (x86). One process; `--verify` adds one thread.
Assumption A1: the operator's workload resembles this mix; every "MB/s" figure is
the enwik8 8 MiB slice unless named otherwise.

**Current measurements** (encode unless stated; medians of 2–3 runs, spread ≤ 1.5 %).

| Mode | Throughput | Wall | Peak RSS | CPU | Ratio (enwik8 8 MiB) |
|---|---|---|---|---|---|
| normal | 1.06 MB/s | 7.92 s | 52.8 MB | 99 % | 1.712 bpB (1,794,864 B) |
| normal decode | 1.05 MB/s | 7.96 s | 52.3 MB | 99 % | — |
| fast | 2.78 MB/s | 3.02 s | 21.8 MB | 99 % | 1.891 bpB |
| max | 0.52 MB/s | 16.02 s | 55.1 MB | 99 % | 1.704 bpB |
| normal `--verify` (proof alongside) | 0.92 MB/s | 9.07 s | 104.2 MB | 184 % | same bytes |

Hardware counters, normal encode (`perf stat`, user only): 35.6 G cycles,
88.3 G instructions → **IPC 2.48**; 531 cycles and 1,315 instructions per
coded bit; branch misses 3.1 % (2.8 per bit, spread across functions in
proportion to their time); dTLB load misses 537 K for 67 M bits (huge pages
work); L1d misses 13.8 per bit; LLC references 15.7 and misses 3.2 per bit;
181 syscalls for 8 MiB (34 reads, 66 writes).

Profile (`perf record`, normal encode): `predict` 43.4 %, `update` 31.7 %,
`start_byte` 14.4 %, `end_byte` 6.5 %, `code_block` (arithmetic coder) 3.4 %,
everything else < 0.5 %. Hottest instructions: in `start_byte`, 57 % of samples
on the branch after the tag load in `claim()` (a cache miss attributed to the
next instruction); in `predict`, the 16-bit APM row load (8.7 %) then the
dot product's `vpermq`/`vpaddq` (int64 lanes); in `update`, the compare/clamp
and multiply of `map_update`.

Fixed costs: an empty input encodes in 0.00 s (model set-up is free); a 50 KB
input runs at 0.28 MB/s, 200 KB at 0.26, 1 MB at 0.62, 4 MB at 0.89 — the
first block's model trials cost ≈ 0.5 s per file.

Noise floor: 1.5 % on wall time across repeats; anything under 2 % is treated as
noise below.

Ratio floor: X is not given; assumption A2 sets it at **1 %** (the gap between the
normal and max profiles is 0.5 %, so anything looser makes the profiles
meaningless). Items marked (A2) depend on it.

Corpus note: doc-ray holds no C-language reference; its relevant sources are
*Systems Performance, 2nd ed.* (Gregg), *C++ High Performance, 2nd ed.*, and the
*Lossless Compression Handbook* (Said's arithmetic-coding chapter). Only passages
actually retrieved are cited, by ordinal.

## Ranked opportunities

Rank = expected gain ÷ (effort × risk). "Bytes-identical" means the container is
unchanged, so the ratio floor is trivially met.

### Land now — if the ratio floor admits 4 %; otherwise see "requires architectural change"

**1. Parallel segment encoding on threads.**
*Current:* `adapt_encode.c:encode_blocks` codes segments in order on one thread;
`cadapt.c:encode_command` sets `sizes.segmented` for `--threads n` and notes
"this port cuts them in order either way". Each segment already starts a fresh
model and the table at the end records every segment's lengths, so the output
is a concatenation.
*Mechanism:* one worker per segment with its own model arena and block buffers,
outputs written in order; no shared mutable state, so no lock is on the per-bit
path (C++ HP ord. 3338–3345 on contention; 3357 "no point in using more threads
than cores").
*Expected effect:* throughput ≈ min(n, segments) × for inputs ≥ 32 MiB: on this
host 5–6× (0.92–1.06 → ~5.5 MB/s) for a 100 MB file. **Measured ratio cost on
enwik8 100 MB, 16 MiB segments:** 20,078,327 → 20,897,572 bytes (+4.1 %) with
every worker at the single model's sizes (300 MB each), and → 21,488,897
(+7.0 %) when six workers share one 300 MB budget, because
`adapt_sizes_for_threads` fits each to a sixth (table 2^20, window 2^22 instead
of 2^23/2^27). Both exceed the A2 floor of 1 %; larger segments (32–64 MiB)
would cut the restart share but were not measured. This item therefore ranks
first only under a floor of ≥ 4 % (or ≥ 7 % at fixed total memory).
*Cost:* M; POSIX threads already optional in the build; Windows threads absent.
Peak RSS × n (each worker its own 45–55 MB) — priority 2 gives way to priority 1;
the per-worker figure is unchanged.
*Risk:* segments below 16 MiB lose more ratio; the verifier design (one thread,
one queue) has to become per-segment; memory budget accounting must divide by n
(already modelled by `adapt_sizes_for_threads`).
*Measurement:* `cadapt encode enwik8 --threads 6` wall time and bytes vs `--threads 1`;
`perf stat` for cycles per bit unchanged.
*Evidence:* `adapt_encode.c:encode_blocks`, `adapt_encode.c:sizes_for_threads_profile`;
C++ HP ord. 3351–3357.

**2. Byte-boundary two-candidate prefetch.**
*Current:* `adapt_model.h:update_impl` prefetches both nibble candidates' lines at
bit 3 (comment: "worth 3 %"), but the byte boundary has no equivalent:
`start_byte → claim_chain → claim` takes ten random 64-byte lines (one per hashed
context), plus `end_byte` one 4 MB recall-table slot and two run-table slots,
all first-touched with nothing in flight.
*Mechanism:* at bit 7 the byte is known but for its last bit; compute the ten
context hashes, the recall key and run slots for both candidate bytes and issue
prefetches — 26 lines, ≈ 60 cycles of hashing against ~300 cycles of exposed
miss latency per byte (57 % of `start_byte` samples sit on the tag-load branch).
*Expected effect:* 4–7 % of encode and decode wall (start_byte is 14 %, ~60 % of it
stall; the existing nibble prefetch bought 3 % on half as many lines).
*Cost:* S (≈ 60 lines); bytes identical.
*Risk:* one candidate's lines are wasted bandwidth (≈ 800 B/byte, 1 GB/s at
1 MB/s — negligible); on a host with hardware prefetch throttling none.
*Measurement:* `perf record` share of `start_byte` before/after; harness sizes
byte-identical; wall on the 8-file corpus.
*Evidence:* `adapt_model.h:update_impl` (bit-3 prefetch), `claim`, `start_byte`,
`end_byte`; C++ HP ord. 1052 (a line is fetched whole) and 1075–1076 (hardware
prefetchers handle small strides only — these are random).

**3. Scale the start-up trials to the input.**
*Current:* `adapt_encode.c:choose_model` codes the first 32 KiB three times for
the class width, then up to seven context sets, then proves a lean choice on the
whole first block against the full set: ≈ 320 KiB + 256 KiB of coding before
byte one — the measured 0.5 s that makes a 200 KB file run at 0.26 MB/s.
*Mechanism:* fewer coded bytes: a sample of min(32 KiB, n/8), the family search
stopped after the first head that fails to beat the full set (already partly
done), and the whole-block proof only when the block is longer than four
samples.
*Expected effect:* small files 2–3× (200 KB: 0.26 → ~0.7 MB/s); 8 MiB inputs
≈ 5 %; 100 MB inputs < 1 %. Ratio: the choice can worsen on files whose first
32 KiB misrepresent them — measure (A2).
*Cost:* S; bytes change only where the choice changes.
*Risk:* a worse set on mixed files; the trial sample size is a format-neutral
encoder policy, so no decoder impact.
*Measurement:* throughput vs size curve (50 KB … 8 MB) and corpus bytes.
*Evidence:* `adapt_encode.c:choose_model`, `trial`, `CLASS_SAMPLE`; measured
size curve above.

**4. APM row prefetch at the byte boundary.**
*Current:* `adapt_model.h:predict_impl` reads `apm[(c1 << 8) | c0]`, a 66-byte row
in a 4.3 MB table; the first row of each new previous byte `c1` is a miss (the
hottest single instruction in `predict`, 8.7 % of its samples).
*Mechanism:* in `end_byte`, once the byte is known, prefetch the row for
`c0 = 1` (and its neighbour line: rows straddle lines); later bits stay inside
the same 17 KB `c1` block and hit L2.
*Expected effect:* 1–3 % (the load is 3.7 % of runtime; only the first of eight
lookups per byte is cold).
*Cost:* XS; bytes identical.
*Risk:* none beyond one extra line per byte.
*Measurement:* `perf annotate predict`: the `movzwl` share; wall.
*Evidence:* `adapt_model.h:predict_impl` (APM lookup), `end_byte`.

**5. `posix_fadvise` on input and output.**
*Current:* `cadapt.c:read_stream`/`write_stream` use stdio; a one-pass tool leaves
every input and output page in the page cache.
*Mechanism:* `POSIX_FADV_SEQUENTIAL` on the input (doubles read-ahead) and
`POSIX_FADV_DONTNEED` behind each 256 KB block on both files; Gregg names the
call and its flags as the hint that "improve[s] performance, including making
better caching decisions" (ord. 5027–5028, 5040).
*Expected effect:* priority 3 only: page-cache growth during a 1 GB encode drops
from ~1.2 GB to ~0; throughput unchanged (I/O is < 1 % of time; 181 syscalls per
8 MiB).
*Cost:* XS, POSIX-only under `#ifndef _WIN32`.
*Risk:* a caller that re-reads the input immediately (the `--verify` second pass)
pays a disk read; skip DONTNEED on the input when that pass is coming.
*Measurement:* `free`/`vmstat` cache columns during a 1 GB encode; wall unchanged.
*Evidence:* `cadapt.c:read_stream`, `encode_command`; Gregg ord. 5027–5028.

### Land after measurement

**6. Fused per-profile bit loop (direct calls, lanes in registers).**
*Current:* `adapt_model.h:ADAPT_PROFILE` selects `predict`/`update` through function
pointers per block; `predict` is a `target_clones` function reached via the
ifunc resolver (`predict.resolver@plt` appears in the profile), so each of the
16 calls per byte is an indirect call, and every intermediate (`st[]`, `cell[]`,
`wset`, `pmix` …) is stored to the model struct by `predict` and reloaded by
`update`.
*Mechanism:* one `code_bit(model*, bit-or-decoder)` per profile instantiation
holding the lanes in registers across predict and update; removes ~16 indirect
calls and ~50 store/load pairs per byte from ~10,500 instructions.
*Expected effect:* 2–4 %; bytes identical.
*Cost:* M (the encoder and decoder loops both change; the multiversion attribute
moves to the fused function).
*Risk:* code size in the hot loop grows; a wrong split hurts inlining.
*Measurement:* instructions per bit (`perf stat`), wall.
*Evidence:* `adapt_model.h:ADAPT_PROFILE`, `encode_byte`, `decode_bytes`;
`perf report` line `predict.resolver@plt`.

**7. APM rows on a 128-byte stride.**
*Current:* `adapt_model.h:model_layout` places `apm` as `uint16_t[65536][33]` —
66-byte rows, so a lookup of `row[j]`, `row[j+1]` crosses a line boundary for
about half the `j` values and every byte's eight lookups touch up to 16 lines.
*Mechanism:* stride 128 B (33 entries, 62 B pad): every lookup inside one line;
memory 4.3 → 8.4 MB (priority 2 gives up 4 MB per model for priority 1).
*Expected effect:* 1–2 %; bytes identical.
*Cost:* XS; RSS +4 MB per model; the memory bound changes (fits tests).
*Risk:* below noise on hosts where the APM already sits in L2; measure first.
*Measurement:* L1d misses per bit and wall.
*Evidence:* `adapt_model.h:model_layout`, `APM_BUCKETS`; C++ HP ord. 1051–1053.

**8. Parallel segment decoding.**
*Current:* `adapt_decode.c` decodes segments in order; the header comment in
`adapt.h` says a decoder "holding a file may read the table first and decode the
segments at once", which the crate does on threads and this port does not.
*Mechanism:* read the segment table from the tail, seek each worker to its
segment, decode in parallel, write in order.
*Expected effect:* decode throughput × min(n, segments) on segmented files;
nothing on unsegmented ones.
*Cost:* M; needs a seekable input (a pipe stays sequential).
*Risk:* RSS × n; the streaming decoder's "bytes written before an error are not
to be trusted" contract now spans workers.
*Measurement:* decode wall of a `--threads 6` file.
*Evidence:* `adapt.h` (segment flag comment), `adapt_decode.c:adapt_decode_stream`.

**9. Fit the model to two copies when `--verify` is asked for.**
*Current:* `cadapt.c:encode_command` runs the proof alongside only if
`adapt_verified_memory_bound(sizes) <= max_memory`; otherwise it re-reads the
input and decodes the staged output in a second, sequential pass. With the
2^21-block table now chosen from 8 MiB up, a default-budget `--verify` of an
8 MiB file takes the sequential path: 7.9 s of coding plus a full decode, versus
9.07 s wall threaded.
*Mechanism:* when verification is requested and two fitted models do not fit,
shrink the context table one step so they do — the `!length_known` branch of
`adapt_encode.c:sizes_for_input_profile` already fits pipes to the verified
bound; extend the rule to files.
*Expected effect:* verified encodes of 8–32 MiB files at the default budget:
wall −40 % (≈ 16 s → 9 s); ratio −1.0 % to −1.6 % from the smaller table
(measured in the format-13 work) — needs A2.
*Cost:* XS.
*Risk:* trades ratio for wall only when the user asked for the proof; document it.
*Measurement:* `--verify` wall and bytes at the default budget.
*Evidence:* `cadapt.c:encode_command` (`alongside`), `adapt_encode.c:sizes_for_input_profile`.

**10. Verifier thread placement on a loaded host.**
*Current:* `adapt_encode.c:verifier_thread` is an unbound pthread; on this host the
scheduler places it on a separate physical core (unbound: 9.07 s wall), but
confined to SMT siblings (`taskset -c 0,6`) the same run takes 12.48 s (+39 %,
23.6 s user vs 16.7 s).
*Mechanism:* the two models each need a full core's L1/L2 and execution width;
siblings share both (Gregg §6.5.10 "binding processes and threads to individual
CPUs", ord. 3393; C++ HP ord. 3371–3380 on affinity and warm caches).
*Expected effect:* 0 on an idle host; up to 39 % of `--verify` wall on a host
where the scheduler cannot spread threads (containers pinned to sibling pairs).
*Cost:* S: a `--cpu` option or an advisory affinity that picks distinct cores
from `sched_getaffinity`; Linux-specific.
*Risk:* a hard pin fights the scheduler on a busy host and can make it worse.
*Measurement:* the `taskset` pair above under a synthetic load.
*Evidence:* measured; Gregg ord. 3393; C++ HP ord. 3371–3380.

**11. `SCHED_BATCH` for the verifier thread.**
*Current:* the proof thread competes at normal priority with everything on the
host (`--verify` costs a second core: 184 % CPU).
*Mechanism:* `pthread_setschedparam(SCHED_BATCH)` on the verifier (Gregg ord.
3792: `chrt -b` runs in SCHED_BATCH): the kernel treats it as throughput work,
lowering its interference with interactive tasks; priority 3.
*Expected effect:* host impact only; encode wall unchanged on an idle host, and
the queue of two blocks (`VERIFY_QUEUE`) absorbs scheduling jitter.
*Cost:* XS, Linux-only.
*Risk:* on an oversubscribed host the encoder stalls when the queue fills.
*Measurement:* latency of a foreground task while encoding with `--verify`.
*Evidence:* `adapt_encode.c:verifier_thread`, `VERIFY_QUEUE`; Gregg ord. 3792.

**12. Explicit huge pages when THP is off.**
*Current:* `adapt_model.h:model_arena` maps the arena with `MADV_HUGEPAGE`, which is
advisory; on this host THP is `madvise` and dTLB misses are 537 K per 67 M bits.
The code's own note records 0.77 dTLB misses per bit without huge pages.
*Mechanism:* fall back to `MAP_HUGETLB` when `/sys/kernel/mm/transparent_hugepage/enabled`
is `never` and the pool has pages (Gregg ord. 4419 names
`MAP_ANONYMOUS|MAP_HUGETLB`; ord. 3539 shows TLB walks taking half the cycles in
a worst case).
*Expected effect:* 0 here; 10–30 % on a THP-`never` host (0.77 misses/bit × ~30
cycles ≈ 23 of 531 cycles at the least, more with page walks).
*Cost:* S; needs a pre-allocated pool, so it is a fallback, not a default.
*Risk:* `MAP_HUGETLB` failure must fall back to normal pages silently.
*Measurement:* `perf stat -e dTLB-load-misses` on a host with THP disabled.
*Evidence:* `adapt_model.h:model_arena` and its comment; Gregg ord. 4045, 4419, 3539.

### Requires architectural change

**13. 16-bit mixer weights with packed multiply-add.**
*Current:* `adapt_model.h:dot` accumulates `int32 × int32` into `int64`, so the
AVX2 clone works four lanes per vector (`vpaddq`, `vpermq` in the hot list);
`train` updates 16–54 int32 weights three times per bit; the first arbiter's
weight sets are 418 KB (6,144 × 68 B), on the edge of L2.
*Mechanism:* int16 weights and inputs (inputs already fit: ±2047), `vpmaddwd`
style 16×16→32 pairs summed — 16 lanes per vector, half the weight bytes (209 KB,
L2-resident). Rounding changes → a new format version, and the ratio floor must
be re-measured (A2).
*Expected effect:* 3–5 % (the dot/train code is ~10 % of `predict` and a similar
share of `update`).
*Cost:* M plus a format bump and vector regeneration.
*Risk:* weight saturation at ±32767 changes learning on long inputs; ratio
measured on the corpus before landing. Corpus note: doc-ray has no text on packed
multiply-add (searched *The Art of 64-Bit Assembly*: no hit), so the evidence is
the codebase and `perf annotate` only.
*Measurement:* instructions per bit; corpus bytes within A2.
*Evidence:* `adapt_model.h:dot`, `train`, `MIXER_SETS`; `perf annotate predict`.

**14. Interleaved trial models for the start-up phase.**
*Current:* `adapt_encode.c:choose_model` runs each trial to completion in turn:
up to ten fresh models over the same 32 KiB, each stalling on its own cold
tables.
*Mechanism:* run the trials of one family in lockstep — one loop advancing k
models a bit at a time — so their independent table misses overlap
(memory-level parallelism) instead of serialising; distinct from item 3, which
codes fewer bytes.
*Expected effect:* the trial phase 1.3–1.5× faster; on an 8 MiB input ≈ 2 %, on a
200 KB input ≈ 20–30 %.
*Cost:* L: `predict`/`update` take one model; the trial loop needs a k-model
driver, and the ratio must be bit-identical (it is, by construction).
*Risk:* code complexity in the encoder only; no format impact.
*Measurement:* small-file throughput curve.
*Evidence:* `adapt_encode.c:trial`, `choose_model`; C++ HP ord. 994 ("the limiting
factor … could be due to cache misses").

**15. Window aliasing for the in-memory API.**
*Current:* `adapt_encode.c:encode_in_memory` streams from the caller's buffer while
`model_layout` allocates a separate `hist` ring of `2^window_log` bytes that
receives the same bytes again.
*Mechanism:* when the window covers the whole input, point `hist` at the caller's
buffer and skip the copy: −8 MB of 52 MB RSS for an 8 MiB input, −1 store per
byte.
*Expected effect:* priority 2: RSS −15 % for the library's in-memory path; wall
< 1 %.
*Cost:* M; the ring indexing assumes a power-of-two mask, the CLI streams and
gains nothing.
*Risk:* an aliasing invariant across `end_byte`, `skip_bytes`, the transform
(which rewrites blocks in place — the alias must see the transformed bytes).
*Measurement:* `maxRSS` of `adapt_encode()` on 8 MiB.
*Evidence:* `adapt_encode.c:encode_in_memory`, `adapt_model.h:model_layout`.

## Top three, in order

Under the assumed 1 % floor (A2):

1. **Item 2, byte-boundary prefetch** — bytes-identical, ~60 lines, attacks the
   one measured memory stall; land before item 6 because item 6 restructures the
   same functions and would otherwise need re-measuring twice.
2. **Item 3, start-up trials** — the largest win for small-file workloads and
   ≈ 5 % on 8 MiB inputs; second because it changes encoder policy (bytes may
   change where the choice changes) and its ratio check should run on a settled
   hot path.
3. **Item 6, fused per-profile bit loop** — bytes-identical, 2–4 %, but a
   restructuring of the same functions as item 2, so it goes last of the three
   and is measured against item 2's baseline.

If the operator's floor is 4 % or looser, **item 1 moves to first**: it is
independent of the other three and the only one that changes the order of
magnitude; items 2, 3 and 6 then follow in the order above.

## Rejected candidates

| Candidate | Reason |
|---|---|
| Profile-guided optimisation (`-fprofile-use`) | Measured: encode 7.83–8.04 s vs 7.92–7.99 s — within noise; decode −5 % on one run only. Below the floor for encode; decode needs three runs before it is anything. |
| Single-multiply `arith_split` (Handbook §5.2.5.5, ord. 2425–2447: inexact multiplication is an inexact model) | The coder is 3.4 % of time; the saving is < 0.5 % and it changes bytes. |
| Verifier queue `memcpy` of source and framed block (`adapt_encode.c:verifier_submit`, 896–898) | 2 × 256 KB per block ≈ 2 MB/s of copying at 1 MB/s coding: < 0.2 %. |
| Larger stdio buffers, `mmap`/`O_DIRECT` input, async read thread (Gregg §8.3.8, §8.3.10 retrieved by title only) | 181 syscalls and 0.04 s sys per 8 MiB: I/O is < 1 % of wall. |
| Skipping the `followed`/`word_prev` updates under the normal profile (`adapt_model.h:end_byte`) | One L2-resident read-modify-write per byte ≈ 0.3 %. |
| Vectorising the eleven `map_update` calls per bit | Independent cells but gather/scatter on Zen 3 costs more than the scalar chain; no bytes-identical formulation found. |
| Removing the `MIXER_SKIP` branch in `train` | Branch misses are 2.8 per bit spread evenly across `predict`/`update`; no single site to fix, and the skip saves a 16-lane update. |
| Earlier/more nibble-boundary prefetch (bit 2, four candidates) | `perf annotate update` shows the nibble claim is not where update stalls; the existing bit-3 prefetch already works. |
| Model set-up per file (`model_new`: 4.3 MB APM init, arena mmap) | An empty input encodes in 0.00 s. |
| Sharing read-only tables between the encoder's and the verifier's models | Only ~50 KB of a 52 MB model is read-only. |
| Match-verify loop with 8-byte compares (`end_byte`) | Runs once per match start; `end_byte` is 6.5 % in total. |
| CPU binding on this host for the single-thread case | One CCX, one L3: nothing to bind toward. (The SMT case is item 10.) |
| Multi-symbol coder structures from the Handbook (bisection / tree-based cumulative updates, ord. 2874–2893, 3018) | Apply to M-ary coders; this codec is binary, where the Handbook itself notes the update problem disappears (ord. 2796, 2811). |
| PPM / BWT / dictionary variants from the Handbook | Different ratio class; out of scope under a fixed-ratio objective. |
| Two-thread pipelining of coding and I/O | I/O share < 1 %. |
| `khugepaged`/THP compaction cost (Gregg ord. 4420) | sys time 0.04 s per 8 MiB; not measurable here. |

## The measurement that would invalidate this ranking

Item 2 leads on an estimate, not a measurement: `start_byte` holds 14 % of
cycles and 57 % of its samples sit on the tag-load branch, but an out-of-order
core already overlaps some of those ten independent misses. If a `perf record`
after implementing the prefetch shows `start_byte` still above ~10 % of cycles —
a gain under 2 %, the noise floor — the stall was already hidden, item 2 drops
to the rejection list, and the bytes-identical ceiling for this codec falls to
the 4–8 % of items 3, 4, 6 and 7 combined. At that point the only remaining
step of any size is item 1, and the operator has to price its 4 % of ratio
against a 5–6× throughput multiple.

Two runs would also move the order without invalidating it: the operator's
floor (X) above 4 % promotes item 1 to first; a workload of mostly sub-1 MB
files promotes item 3 to first (0.26 → ~0.7 MB/s on 200 KB inputs).
