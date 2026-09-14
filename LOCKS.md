# Locked decisions

Decisions that are settled. A change to any of them is a new entry, not an edit.

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
