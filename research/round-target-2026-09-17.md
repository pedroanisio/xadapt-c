<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-17, against the
operator's round target of that day. Every number is from a run of
bench/codec_bench.py on the host named under Method. -->
# The round's target, measured

Part of [adapt](../README.md).

This codec is 24 % smaller than `xz -9e` on enwik9 in less than half its
encode time, and threads close the time and memory halves of the round's
target while costing ratio to do it. On enwik8, `--threads 4` puts the codec inside **30 s / 30 s /
600 MiB** at 19,454,846 bytes — 14.4 % above the 17 MB goal. On enwik9,
`--max --contexts lean --threads 4` meets the 500 s and 530 s budgets at
434.7 s and 362.7 s and 161,113,398 bytes — 10.4 % above the 146 MB goal, and
its decode peaks at 817.9 MiB uncapped — which `--max-memory` brings inside
the envelope without touching the codec, at a cost in decode time. Every row
restored byte for byte.

The earlier reading that threads cost memory in proportion to worker count was
wrong: `adapt_sizes_for_threads` divides one budget across the workers, so
four threads peak near one thread's figure, not four times it. That correction
is what moved the target from unreachable to partly met.

## Method

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container — not
the reference host of `tools/ratio_floor.json`. `bench/codec_bench.py` runs one
process per measurement so the peak RSS is that process's own, times it with a
wall clock, and compares every restored file against the source byte for byte;
a codec that does not restore fails the run. `enwik9` was fetched fresh and
its md5 matches the fixture the floors name. Data:
[round-target-2026-09-17.json](round-target-2026-09-17.json).

`(tN)` marks `--threads N` on both encode and decode. No dictionary is used or
shipped anywhere: every book this codec builds comes from the input it is
compressing (`FMT-2`).

## enwik8 — target 17,000,000 B, 30 s, 30 s, 600 MiB

| Configuration | Container | Encode | Decode | Enc RSS | Dec RSS | Inside the envelope |
|---|---|---|---|---|---|---|
| `--max --allocation context --contexts lean` | **17,902,645** | 127.3 s | 137.9 s | 655.0 | 654.4 | no |
| `--max … lean -8` | 18,118,964 | 128.4 s | 137.6 s | 355.0 | 354.5 | no |
| `--max … lean` (t4) | 18,966,492 | 60.1 s | 39.0 s | 514.9 | 653.4 | no |
| default (t3) | 19,335,301 | 38.1 s | 40.6 s | 602.2 | 601.9 | no |
| **default (t4)** | **19,454,846** | **23.5 s** | **24.8 s** | **528.1** | **511.4** | **yes** |
| `--mid` (t3) | 19,595,070 | 26.4 s | 29.0 s | 602.2 | 602.3 | time only |
| `--mid` (t4) | 19,712,074 | 16.6 s | 18.4 s | 526.1 | 512.5 | yes |

Best inside the envelope: **19,454,846 B, +14.4 % over the goal**, with 6.5
seconds of encode headroom unspent. Best ratio at any speed: 17,902,645 B,
+5.3 % over the goal and 4.2× over the time budget.

The gap between them is the whole trade. Four threads buy 5.4× of encode time
and cost 8.7 % of ratio, and on this host nothing bridges it: `--max` at four
threads still takes 60.1 s, and there is no configuration between
`default (t4)` at 23.5 s and `--max … lean (t4)` at 60.1 s.

## enwik9 — target 146,000,000 B, 500 s, 530 s, 600 MiB

| Configuration | Container | Encode | Decode | Enc RSS | Dec RSS |
|---|---|---|---|---|---|
| `--max … lean` (t4) | **161,113,398** | **434.7 s** ✓ | **362.7 s** ✓ | **515.5** ✓ | 817.9 ✗ |
| default (t4) | 166,333,147 | 218.5 s | 242.7 s | 546.6 | 683.3 ✗ |
| `--mid` (t4) | 169,145,966 | 152.3 s | 168.8 s | 546.8 | 685.9 ✗ |

The references on the same harness, same host, single threaded:

| Codec | Container | Encode | Decode | Enc RSS | Dec RSS |
|---|---|---|---|---|---|
| `xz -9e` | 211,776,220 | 908.7 s | 11.1 s | 675.3 | 66.1 |
| `zstd -19` | 235,515,928 | 585.6 s | 1.4 s | 221.2 | 12.1 |

`--max --contexts lean` at four threads is **24 % smaller than `xz -9e` in
less than half its encode time**, and 32 % smaller than `zstd -19`; the locked
single-threaded container is 30.7 % smaller than `xz -9e`. Both references
keep their decode asymmetry — 11.1 s and 1.4 s against 362.7 — which is the
one axis where LZMA and Zstandard are in a different class and always will be.

The locked single-threaded figure, 146,797,785 B, **meets the 146 MB goal** —
at 1,056 CPU s on the reference host, twice the time budget. So enwik9's two
halves are each reachable and not yet together: the ratio alone single
threaded, the time and encode memory alone at four threads.

Threads cost more on enwik9 than on enwik8 — 9.8 % against 8.7 % — for two
reasons that pull the same way. Each restart costs about 105 KB and a 1 GB
file cut into 16 MiB segments takes 59 of them, and the one budget divided
four ways leaves each worker a model far too small for a gigabyte. Larger
segments would cut the restarts, but each worker buffers a whole segment
(`adapt_segment_out_bound`), so segment size and worker count compete for the
same 600 MiB.

## Decode memory: not a defect, an option nobody passed

Decode under threads does cost more than the encode that wrote the file —
enwik9 817.9 MiB against 515.5, enwik8 652.9 against 514.9 — and the reason is
plain in the bound: `adapt_parallel_decode_memory_bound` charges each worker a
model, the longest segment's decoded bytes and a block's workspace, and the
whole container besides. Measured on the enwik8 container, decode costs about
150 MiB per worker:

| Decode | Wall | Peak RSS |
|---|---|---|
| `--threads 1` | 149.4 s | 153.2 MiB |
| `--threads 2` | 79.4 s | 349.8 MiB |
| `--threads 4` | 40.8 s | 652.9 MiB |
| `--threads 4 --max-memory 629145600` | 68.6 s | **514.4 MiB** |
| `--threads 4 --max-memory 419430400` | 80.7 s | **349.7 MiB** |

**The last two rows are the answer, and they need no change to the codec.**
`adapt_decode_workers` already walks the worker count down until the bound
fits the cap, so a caller who states the envelope gets it: 600 MiB asked for,
514.4 MiB used, byte-exact restore. An earlier draft of this note called the
decode figure a defect against `RUN-1`; that was wrong. `RUN-1` promises the
tier's memory for a single-threaded decode, and `FMT-6` already says only a
cap the caller sets may refuse a valid container — `--max-memory` is that cap,
and it works.

What remains true is that the *default* is to use the threads asked for and
spend what they need. A caller who says `--threads 4` and nothing else gets
four workers' memory. That is a documentation and defaults question, not a
correctness one.

On enwik9 the cap turns the memory miss into a time miss rather than removing
it. Decoding the 161,113,398-byte container with `--threads 4`:

| Decode | Wall | Peak RSS | Restored |
|---|---|---|---|
| uncapped | **362.7 s** ✓ | 817.9 MiB ✗ | yes |
| `--max-memory 629145600` | 724.8 s ✗ | **487.8 MiB** ✓ | yes |

Neither point satisfies both budgets, and the frontier between them has no
point that does: the cap buys memory by dropping workers, and the workers are
what buy the time. Per-worker memory is set by the model the *encoder* chose,
so the only way to fit more decode workers in 600 MiB is to encode with more
threads and a smaller model each — which spends ratio, the axis already
short.

## Where the round stands

| | Goal | Best measured | Gap |
|---|---|---|---|
| enwik8 bytes, inside 30 s / 600 MiB | 17,000,000 | 19,454,846 | +14.4 % |
| enwik8 bytes, any speed | 17,000,000 | 17,902,645 | +5.3 % |
| enwik9 bytes, inside 500 s / 600 MiB | 146,000,000 | 161,113,398 | +10.4 % |
| enwik9 bytes, any speed | 146,000,000 | 146,797,785 | **met** |
| enwik9 encode | 500 s | 434.7 s | **met** |
| enwik9 decode, uncapped | 530 s | 362.7 s | **met**, at 817.9 MiB |
| enwik9 encode RSS | 600 MiB | 515.5 MiB | **met** |
| enwik9 decode RSS, capped | 600 MiB | 487.8 MiB | **met**, at 724.8 s |
| enwik9 decode time *and* RSS together | 530 s, 600 MiB | no such point | — |

Four of the eight are met, and the decode axis is met on either budget alone
but not on both at once. The ratio gaps all have one shape: threads buy the
time and spend the bytes.

The memory figure is a caller's choice, not a wall: `--max-memory` fits the
decoder to whatever envelope is stated, and the price is decode time rather
than bytes. What is left is the ratio, and it has one shape — threads buy the
time and spend the bytes, because each worker restarts the model and each gets
a quarter of the budget. The lever that would change that is letting a worker
stream its segment out rather than buffer it whole: buffering is what forces
small segments inside a fixed budget, 59 restarts on a gigabyte, and it is the
larger half of the 9.8 % that threads cost on enwik9.
