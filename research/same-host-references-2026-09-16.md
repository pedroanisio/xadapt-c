<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-16, from the operator's
question of that day: how far behind the reference codecs this one is. Every
number is from a run on the host named under Method; nothing is normalised
across machines. -->
# The reference codecs on one host

Part of [adapt](../README.md).

> **Superseded in part, 2026-09-17.** This note measured bytes and seconds
> only. With peak memory measured too, and with `--mid` run at every tier
> instead of interpolated, two claims below do not survive:
> "the deficit is one profile, not the codec" and "about 1.3× at matched
> ratio". The deficit is **every** profile and it is **1.60× to 3.91×**,
> measured. See [three-axis-strategy-2026-09-17.md](three-axis-strategy-2026-09-17.md),
> which replaces this note's conclusions; the table below stands as measured.

This codec reaches ratios no reference reaches, and is slower than the
reference at every point where they overlap: `kanzi -l9` produces
20,035,687 B in 35.7 s where `--fast` produces 21,663,531 B in 35.6 s, so at
equal time the reference is **8.1 % smaller**, and `kanzi -l8` is both smaller
and 1.6× faster than `--fast`. Below 20 MB the reference has no entry at all,
which is territory rather than a win: the strategy note tests whether any
configuration of ours beats a reference point on all three axes at once, and
none does.

## Method

Host: Intel Xeon @ 2.10 GHz, 4 cores, 15 GiB, a shared cloud container — not
the reference host of `tools/ratio_floor.json`, so no time here may be
compared to a locked floor. All times are wall seconds of a single encode or
decode on this one host, which is the point: every comparison in
[speed-3x-experiments-2026-09-16.md](speed-3x-experiments-2026-09-16.md)
until now was cross-machine and therefore arguable.

Input is enwik8 (100,000,000 B). `kanzi` 2.5.3 was built from its own
`CMakeLists.txt` with no source change and run with `-j 1`, one job, so the
comparison is per core; its levels are its own presets. This codec's rows use
the profile named. `--max --allocation context --contexts lean` reproduces the
locked container byte for byte, which is what makes the ladder trustworthy.

`mcm` could not be built: it is a 2017 tree that does not compile under
g++ 13 (four distinct C++ conformance errors), and patching a GPL-3 source is
outside `DLV-4`. Its published enwik8 figures stand as ratio evidence only —
`-x9` at 18,379,121 B, which this codec beats by 2.6 %.

## enwik8, one host, one core

| Codec | Container | bits/byte | Encode | Decode |
|---|---|---|---|---|
| `kanzi -l7` (LZP+BWT&CM) | 22,817,366 B | 1.825 | 8.1 s | 5.8 s |
| **`cadapt --fast`** | 21,663,531 B | 1.733 | 35.6 s | — |
| `kanzi -l8` (TPAQ) | 21,181,998 B | 1.695 | 22.8 s | 23.5 s |
| `kanzi -l9` (TPAQX) | 20,035,687 B | 1.603 | 35.7 s | 35.3 s |
| **`cadapt --mid`** | 18,761,302 B | 1.501 | 56.3 s | — |
| **`cadapt` (default)** | 18,500,473 B | 1.480 | 86.5 s | — |
| **`cadapt --max`** | 18,008,117 B | 1.441 | 150.7 s | — |
| **`cadapt --max --allocation context --contexts lean`** | 17,902,645 B | 1.432 | 131.7 s | — |

Every kanzi round trip was compared byte for byte against the source.

## What the table says

**`--fast` is strictly dominated and should not exist in its present form.**
`kanzi -l8` beats it on both axes at once — 2.2 % fewer bytes in 64 % of the
time — and `kanzi -l9` matches its time while giving up 8.1 % fewer bytes.
A profile that is beaten on both axes by a general-purpose block compressor is
not a fast profile; it is 192 cycles per bit spent on three context slots and
no estimation stage, which is the worst of both designs. The honest target for
anything occupying that slot is `kanzi -l9`'s point: **20.0 MB in 35.7 s on
this host.**

**Below 20 MB the comparison ends, and that is where this codec lives.**
`--mid` at 18,761,302 B has no reference counterpart: kanzi's strongest preset
stops 6.8 % above it, and `mcm -x9`'s published 18,379,121 B is 2.6 % above
`--max --contexts lean`. The ratio leadership is real and it is not close.

**The gap is not threefold — but the 1.3× figure this note first gave was an
interpolation, and it was wrong.** `--mid` reaches 18.76 MB in 56.3 s, a point
kanzi cannot reach at any level. Estimating this codec's time at kanzi `-l9`'s
20.04 MB by interpolating between `--fast` and `--mid` gave about 47 s. Run
instead of interpolated, `--mid -5` produces 20,041,401 B — kanzi `-l9`'s size
to within 0.03 % — in **58.9 s**, because `--mid`'s time barely moves with the
memory tier. The measured deficit at matched ratio is therefore **1.60× to
1.72×**, not 1.3×. Threefold was never the gap; 1.6× is.

## Measured and rejected on the way

*Per-byte state gated on its consumer.* Every byte maintains `word_lc`, the
`word_prev2` chain, the markup-nesting state machine and two random-access
table stores (`followed`, `followed1`), each of which has exactly one consumer
in `hash_kinds()`. Under `--mid` — seven kinds, none of them `wordlc`, `nest`,
`indirect`, `o8` or `word3` — all five are dead work. Gating each on a flag
set once in `model_new` was built, verified bytes-identical on `--fast`,
`--mid` and `--max --contexts lean`, and measured interleaved on the 16 MiB
fixture: **`--fast` +2.3 %, `--mid` +0.1 %, `--max` 0.0 %.** The arithmetic
explains it — the gated work is roughly thirty cycles a byte against ten
thousand — and the change was reverted. The per-byte machinery is not where
the time is, whatever share of it is dead.
