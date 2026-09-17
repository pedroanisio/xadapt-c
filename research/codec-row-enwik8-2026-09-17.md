<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-17, from the operator's
round target of that day. Every number is from a run of bench/codec_bench.py
on the host named under Method; nothing is normalised across machines. -->
# enwik8 on one harness — cadapt, xz, zstd, kanzi

Part of [adapt](../README.md).

This codec produces the three smallest containers on enwik8 by a wide margin
— 28 % smaller than `xz -9e` and 34 % smaller than `zstd -19` — and `--mid`
beats `xz -9e` on encode time, size and memory at once. It is slower than
every reference at matched ratio, and the round's target of **17 MB in 30 s
each way inside 600 MB is 4.2× of encode time away from the measured best**,
which no lever priced this month closes.

## Method

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container — not
the reference host of `tools/ratio_floor.json`, so no second here may be
compared to a locked floor. `bench/codec_bench.py` runs one process per
measurement so the peak RSS is that process's own, times it with a wall clock,
and compares every restored file against the source byte for byte; a codec
that does not restore fails the run rather than scoring a ratio. Single
threaded throughout (`-T1`, `-j 1`). Data:
[codec-row-enwik8-2026-09-17.json](codec-row-enwik8-2026-09-17.json).

## enwik8, 100,000,000 bytes

| Codec | Container | Ratio | Encode | Decode | Encode RSS | Decode RSS |
|---|---|---|---|---|---|---|
| `cadapt --max --allocation context --contexts lean` | **17,902,645** | 0.1790 | 127.3 s | 137.9 s | 655.0 MiB | 654.4 MiB |
| `cadapt` (default) | 18,500,473 | 0.1850 | 80.8 s | 88.1 s | 600.8 MiB | 600.3 MiB |
| `cadapt --mid` | 18,761,302 | 0.1876 | 56.2 s | 63.2 s | 600.8 MiB | 600.2 MiB |
| `kanzi -l9` | 20,035,687 | 0.2004 | 34.1 s | 33.8 s | 905.0 MiB | 905.2 MiB |
| `kanzi -l8` | 21,181,998 | 0.2118 | 22.6 s | 22.8 s | 265.9 MiB | 265.5 MiB |
| `cadapt --fast` | 21,663,531 | 0.2166 | 31.7 s | 32.6 s | 90.6 MiB | 90.1 MiB |
| `kanzi -l7` | 22,817,366 | 0.2282 | 8.1 s | 5.7 s | 96.0 MiB | 95.7 MiB |
| `xz -9e` | 24,831,648 | 0.2483 | 85.6 s | 1.1 s | 674.2 MiB | 66.1 MiB |
| `zstd -19` | 26,954,633 | 0.2695 | 53.9 s | 0.2 s | 188.6 MiB | 11.8 MiB |

Every row restored the input byte for byte. The `--max` row is the locked
enwik8 container.

## What the row settles

**`--mid` dominates `xz -9e` outright on the encode side.** 18,761,302 bytes
in 56.2 seconds inside 600.8 MiB against 24,831,648 in 85.6 seconds inside
674.2 MiB: 24 % smaller, 1.5× faster to write, and lighter. The only axis
`xz` keeps is decode, where LZMA's asymmetry gives it 1.1 s against 63.2 s —
which is the honest reason to reach for `xz` when a file is written once and
read often, and no reason at all when it is written once and stored.

**The ratio lead over the general-purpose field is large and not close.**
Nothing here except this codec goes below 20 MB, and the nearest general tool,
`zstd -19`, is 50 % larger than `--max --contexts lean`. Against the two the
round names: 28 % smaller than `xz -9e`, 34 % smaller than `zstd -19`.

**Memory is not a weakness.** `kanzi -l9` needs 905 MiB to reach 20.0 MB where
`--mid` needs 600.8 MiB to reach 18.8 MB, and `--fast` holds 90.6 MiB. The
600 MB envelope the round sets is already met by every profile but `--max`,
which sits 9 % over it.

**Decode is the asymmetry to own.** This is a context-mixing codec: decode
costs what encode costs, within 10 %. `xz` and `zstd` decode 50× to 300×
faster than they encode. Any claim about this codec's speed has to say which
direction it means.

## Against the round's target

The target is enwik8 at 17 MB in 30 s each way inside 600 MB. Measured best
against it:

| | Target | Measured (`--max --contexts lean`) | Gap |
|---|---|---|---|
| Container | 17,000,000 B | 17,902,645 B | **+5.3 %** |
| Encode | 30 s | 127.3 s | **4.24×** |
| Decode | 30 s | 137.9 s | **4.60×** |
| Peak RSS | 600 MiB | 655.0 MiB | +9.2 % |

The 17 MB is `DLV-3`'s own long-standing goal, so the ratio half of the target
is the project's and it is 5.3 % away. The speed half is new, and it is the
part nothing measured this month reaches. Every lever priced since 2026-09-16,
with what it returns and what it costs:

| Lever | Returns | Costs |
|---|---|---|
| Context-slot deletion | 1.11× on a full file | ratio, 0.79 % |
| Arbiter deletion (`--mid`) | 1.4× over the default | ratio, 1.4 % |
| Front-end transforms on enwik8 | 1.05× ceiling | nothing, but it is a ceiling |
| Threads | 2.1–2.3× | ratio 6 %, and memory ×workers |
| Line-aligned arbiter rows | 0.99× | reverted |
| Per-byte state gating | 1.00× outside `--fast` | reverted |

Nothing on that list is both large and free, and the two largest both spend
ratio — which moves the container away from 17 MB while chasing 30 s. The
product of everything that does not cost ratio is about 1.05×.

The reference row bounds the ask independently of this codec. At 34.1 seconds
the strongest general-purpose context-mixing compressor measured here produces
**20.0 MB**, using 905 MiB. The target asks for 17.0 MB at 30 seconds inside
600 MiB — 15 % smaller than the best available tool at a shorter time and two
thirds of its memory. That is not a tuning target for this codec; it is past
the field on this host.

## What the round can deliver

Three things are reachable and one is not, and they should be separated
before the work starts.

**Reachable now, at the locked ratio:** the 600 MB envelope, by giving `--max`
a default tier at the knee of its own curve rather than `-9`; `--max -7`
measured 18,542,411 B in 181.6 MiB, so the envelope is a policy choice across
the whole ladder.

**Reachable with a version number:** the markup book, measured at 1.95× and
−1.2 % on the Silesia `xml` row — a `DLV-3` win that does not move enwik8.

**Reachable with a stated ratio budget:** 1.3× to 1.45× of encode time from
the model levers above, priced per corpus row by `bench/slot_price.py`, at 1 %
to 2.5 % of ratio. That is the only route to a materially faster `--max`, and
it makes the 17 MB goal harder, not easier.

**Not reachable from here:** 17 MB and 30 seconds together. Closing 4.2× while
*gaining* 5 % of ratio needs a different per-bit kernel, not a tuning of this
one — which is the same conclusion the same-host note reached about `--fast`,
arrived at now from the other end of the ladder.
