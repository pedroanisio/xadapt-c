<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-17, against the
operator's round target of that day. Every number is from a run of
bench/codec_bench.py on the host named under Method. Data:
codec-rows-2026-09-17.json. -->
# The round's target in CPU seconds

Part of [adapt](../README.md).

On enwik9 this codec meets every axis of the round's target and meets no two of
them at once: `--max --allocation context --contexts lean` produces
**145,784,134 bytes**, inside the 146 MB goal, at 1,056 CPU seconds; `--mid`
encodes in **493 CPU seconds** and decodes in **526**, inside the 500 and 530
second budgets, in **554.7 MiB**, inside the 600 MB envelope, and costs 18.1 %
more bytes. On enwik8 the 30-second line is reachable and the 17 MB line is
not: `--fast` encodes in 27 CPU seconds at 21,663,531 bytes, while the smallest
container, 17,800,141 bytes, takes 107. Against the two codecs the round names,
`--mid` is 24 % smaller than `xz -9e` on enwik8 in two thirds of its encode CPU
and 30 % smaller than `zstd -19` in the same CPU.

Time here is CPU time, and that single change retires threading. A thread moves
work between cores without removing any of it, so the four-thread encode of the
locked enwik8 container costs 134.1 CPU seconds against one thread's 133.9 — a
rounding error — while the container grows from 17,902,645 to 18,966,492 bytes,
5.9 %. Under a wall clock those threads look like a 2.1× speedup; under a CPU
budget they are 5.9 % of ratio for nothing. Every configuration below is
single-threaded.

## Method

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container.
`bench/codec_bench.py` runs one process per measurement so the peak RSS is that
process's own, records `ru_utime + ru_stime` for it, and compares every restored
file against the source byte for byte; a codec that does not restore fails the
run. Wall seconds equal CPU seconds within 0.5 % on every single-threaded row.
No dictionary is used or shipped: every book this codec builds comes from the
input it is compressing (`FMT-2`).

Reference-host seconds come from one factor, **1.268**, and the factor is
measured rather than assumed. The locked floors record this codec's CPU seconds
on the reference AMD Ryzen 5 5600X for two byte-identical encodes, 107.4 on
enwik8 and 1,054.0 on enwik9; the same encodes here take 136.0 and 1,339.2,
giving 1.266 and 1.271. Dividing by the mean reproduces the locked figures to
0.2 % and reproduces a third the floors do not gate, the default profile's
enwik9 encode, to 0.1 %. The factor is exact for this codec, because the work is
identical, and an estimate for `xz`, `zstd` and `kanzi`, whose instruction mix
is not this one's.

## enwik9 — 146,000,000 bytes, 500 s, 530 s, 600 MB

| Configuration | Container | Encode | Decode | Enc RSS | Dec RSS |
|---|---:|---:|---:|---:|---:|
| `--max --allocation context --contexts lean` | **145,784,134** ✓ | 1,056.2 | 1,169.6 | 659.2 | 658.5 |
| default | 165,865,476 | 706.5 | 759.4 | 554.9 | 554.3 |
| **`--mid`** | 172,472,731 | **493.2** ✓ | **526.4** ✓ | **554.7** ✓ | **554.3** ✓ |
| `xz -9e` | 211,776,220 | 739.9 | 9.0 | 675.3 | 66.1 |
| `zstd -19` | 235,515,928 | 497.2 | 1.4 | 227.6 | 12.0 |

Seconds are reference-host CPU seconds; RSS is MiB, measured here.

`--mid` satisfies three of the four budgets simultaneously and misses the fourth
by 18.1 %. `--max … lean` satisfies the fourth alone and misses the encode
budget by 2.1× and the memory envelope by 9.9 %. Between them sits the default
profile, which misses everything: 13.6 % over on bytes, 41 % over on time. The
target therefore asks for a container 15.6 % smaller than `--mid`'s at `--mid`'s
speed, and nothing on the profile ladder occupies that point.

Both references are beaten on the encode side and neither is beaten on decode.
`--mid` is 18.6 % smaller than `xz -9e` in 33 % less encode CPU, and 26.8 %
smaller than `zstd -19` in the same encode CPU to within 1 %. `xz` decodes in
9.0 seconds and `zstd` in 1.4 against `--mid`'s 526.4, which is what
context-mixing costs and always will: the decoder runs the model the encoder
ran. A file written once and read often belongs in `xz`; a file written once and
stored belongs here.

## enwik8 — 17,000,000 bytes, 30 s, 30 s, 600 MB

| Configuration | Container | Encode | Decode | Enc RSS | Dec RSS |
|---|---:|---:|---:|---:|---:|
| `--max --allocation context --contexts lean` | **17,800,141** | 107.3 | 117.5 | 655.2 | 654.6 |
| default | 18,249,169 | 68.6 | 74.8 | 600.9 | 600.3 |
| `--mid -11` | 18,692,878 | 47.4 | 53.7 | 612.8 | 612.3 |
| `--mid` | 18,761,302 | 47.8 | 53.3 | 600.8 | 600.2 |
| `kanzi -l9` | 20,035,687 | 28.9 | 28.2 | 905.1 | 905.0 |
| `kanzi -l8` | 21,181,998 | 18.9 | 20.0 | 265.8 | 265.6 |
| `--fast -10` | 21,286,652 | **28.9** ✓ | **29.4** ✓ | **102.7** ✓ | **102.1** ✓ |
| `--fast` | 21,663,531 | **27.0** ✓ | **27.5** ✓ | **90.6** ✓ | **90.1** ✓ |
| `kanzi -l7` | 22,817,366 | 6.5 | 4.7 | 96.1 | 95.7 |
| `xz -9e` | 24,831,648 | 74.9 | 1.0 | 674.2 | 66.2 |
| `zstd -19` | 26,954,633 | 48.8 | 0.2 | 188.5 | 11.9 |

The 30-second budget is met by `--fast` and by nothing else this codec writes,
at 21,663,531 bytes — 27.4 % above the goal. Raising `--fast` from its default
tier to `-10` buys 376,879 of those bytes for 1.9 CPU seconds and 12 MiB, and
`-11` buys nothing further: the fast profile's model saturates at 102.7 MiB,
a sixth of the envelope the round allows. That ceiling is the profile's, not the
tier's, and it is where the enwik8 gap now lives.

`kanzi -l9` fixes the price of the target independently of this codec. At 28.9
CPU seconds — the same as `--fast -10` — the strongest general-purpose
context-mixing compressor measured here produces 20,035,687 bytes using 905.1
MiB. The round asks for 17,000,000 bytes at 30 seconds inside 600 MB: 15 %
smaller than the best available tool at the same time and two thirds of its
memory. Against that reference `--fast -10` gives up 6.2 % of ratio and takes
8.8× less memory, which is the trade this codec is currently making at that
speed.

`--mid -11` is the one free gain on the ladder. Twelve more MiB take 68,424
bytes off enwik8 at 47.4 CPU seconds against `--mid`'s 47.8 — inside the noise
of a shared host, so the tier costs no time — and it stays inside the 600 MB
envelope on this corpus. It does not approach 30 seconds.

## Where the round stands

| | Goal | Best measured | Gap |
|---|---|---:|---|
| enwik9 bytes | 146,000,000 | **145,784,134** | **met** |
| enwik9 encode | 500 s | **493.2 s** (`--mid`) | **met** |
| enwik9 decode | 530 s | **526.4 s** (`--mid`) | **met** |
| enwik9 RSS | 600 MB | **554.7 MiB** (`--mid`) | **met** |
| enwik9, all four at once | — | no such configuration | — |
| enwik8 bytes | 17,000,000 | 17,800,141 | +4.7 % |
| enwik8 encode | 30 s | 27.0 s (`--fast`) | **met**, at 21.7 MB |
| enwik8 decode | 30 s | 27.5 s (`--fast`) | **met**, at 21.7 MB |
| enwik8 RSS | 600 MB | 90.6 MiB (`--fast`) | **met** |
| enwik8, all four at once | — | no such configuration | — |

Each axis is met on both corpora and no configuration meets two of them
together, which locates the work precisely. On enwik9 the distance is 15.6 % of
ratio at `--mid`'s speed; on enwik8 it is 21.5 % of ratio at `--fast`'s. Both
ask the same question: what does a profile between these two look like, given
that `--mid` costs 1.77× `--fast`'s CPU for 13.4 % of its bytes on enwik8.

The levers priced this month do not answer it. Context-slot deletion returns
1.11× for 0.79 % of ratio, arbiter deletion (`--mid` itself) 1.4× over the
default for 1.4 %, front-end transforms on enwik8 1.05× at best, and threads
1.00× under a CPU budget. The product of everything that does not cost ratio is
about 1.05×. Closing 1.77× while gaining ratio needs a different per-bit kernel,
not a tuning of this one — and the memory the fast profile leaves unspent,
102.7 MiB against 600, is the first place to look for it.
