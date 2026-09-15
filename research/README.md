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
| [ltcb-tables.md](ltcb-tables.md) | 2026-09-14 | The Large Text Compression Benchmark tables as CSV, with this codec's rows; `make ltcb` builds the charts |

The CSV and JSON files beside the notes are their data. The renderings
`make ltcb` writes here are not tracked.
