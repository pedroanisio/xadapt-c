# Research notes

<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-15. -->

Part of [adapt](../README.md). Dated records of the research behind the
codec: each was true of the code on its date, and none is kept current —
the decisions that came of them are in [LOCKS.md](../LOCKS.md), and the
numbers that stand are in [tools/ratio_floor.json](../tools/ratio_floor.json).

| Note | Date | What it holds |
|---|---|---|
| [arxiv-compression-review.md](arxiv-compression-review.md) | 2026-09-14 | The compression literature (arXiv) read against this codec; the papers in [arxiv-compression-papers.json](arxiv-compression-papers.json) |
| [byte-only-compression-strategy.md](byte-only-compression-strategy.md) | 2026-09-14 | The byte-only strategy that became `FMT-8` in [docs/CONSTRAINTS.md](../docs/CONSTRAINTS.md) |
| [byte-only-mathematical-model.md](byte-only-mathematical-model.md) | 2026-09-14 | A theoretical bound for enwik8 (13,182,140 bytes), verified by [verify_byte_only_math.py](verify_byte_only_math.py) |
| [causal-byte-implementation.md](causal-byte-implementation.md) | 2026-09-14 | The adaptive predictors and the BMP transform (container versions 25–52), still in the code |
| [self-contained-experiment-spec.md](self-contained-experiment-spec.md) | 2026-09-14 | The self-contained experiment protocol for those profiles, with its source audit |
| [scripts-measurement.md](scripts-measurement.md) | 2026-09-14 | The six per-script corpus rows (Cyrillic, CJK, diacritics) at that day's binary |
| [baseline-2026-09-14.md](baseline-2026-09-14.md) | 2026-09-14 | The measurements of 2026-09-14 afternoon: 18,564,510 bytes on enwik8 with `--allocation context`, since improved |
| [ltcb-tables.md](ltcb-tables.md) | 2026-09-14, snapshot of 2026-09-15 | The Large Text Compression Benchmark tables as CSV — the main table of the day cadapt 0.12.0 was listed (rank 30 of 226), transcribed by `tools/ltcb/transcribe_ltcb_table.py`; `make ltcb` builds the charts |
| [speed-3x-experiments-2026-09-16.md](speed-3x-experiments-2026-09-16.md) | 2026-09-16 | What `kanzi`, `mcm` and `zmix` are worth for a threefold speedup at the locked ratio: five experiments run — four refuted, one paying — and thirteen more ranked; the context-slot price tables in [slot-price.json](slot-price.json) from [bench/slot_price.py](../bench/slot_price.py), and the front-end bypasses priced out on text by [bench/redundancy_probe.c](../bench/redundancy_probe.c) |
| [same-host-references-2026-09-16.md](same-host-references-2026-09-16.md) | 2026-09-16 | `kanzi` 2.5.3 and this codec measured on one host, one core: the ladder from `kanzi -l7` to `--max --contexts lean`, and what it says about `--fast` |
| [three-axis-strategy-2026-09-17.md](three-axis-strategy-2026-09-17.md) | 2026-09-17 | The path to winning ratio, memory and speed together: the profile x tier x reference surface in [three-axis-surface.json](three-axis-surface.json), the domination test, and the phased plan |

The CSV and JSON files beside the notes are their data. The renderings
`make ltcb` writes here are not tracked.
