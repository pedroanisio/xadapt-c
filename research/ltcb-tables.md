# Large Text Compression Benchmark tables as CSV

Two CSV snapshots of Matt Mahoney's Large Text Compression Benchmark (LTCB),
transcribed from the published HTML so the numbers can be joined against this
repository's own measurements. The values are the published ones, unmodified
except for removing thousands separators. Nothing here was measured by this
repository. Additional codec measurements are identified separately in
[ltcb-local-entries.csv](ltcb-local-entries.csv).

| File | Source page | Page "Last update" | Rows |
| --- | --- | --- | ---: |
| [ltcb-main-table.csv](ltcb-main-table.csv) | <https://mattmahoney.net/dc/text.html> | Sept 15, 2026 | 226 |
| [ltcb-textdata-text8-fil9.csv](ltcb-textdata-text8-fil9.csv) | <https://mattmahoney.net/dc/textdata.html> | Sept. 1, 2011 | 27 |

What is tracked and what is not: the two transcribed CSVs, the local
entries and this note are in git. The tooling that reads them lives in
[tools/ltcb/](../tools/ltcb/) with its tests
(`python3 -B -m unittest discover -s tools/ltcb -p 'test_*.py'`). The
renderings it writes into this directory — `ltcb-dashboard.{svg,png}`,
`ltcb-phases.{svg,png,csv}`, `ltcb-analysis.{json,md}`,
`ltcb-interactive.html` — are ignored by git; `make ltcb` at the repository
root builds all of them, and each section below gives its own command;
nothing quotes a number from them that the CSVs do not carry.

## Snapshot provenance

The main source page was captured on 2026-09-15 at 23:25 UTC, SHA-256
`7ea13d26b7c8e67427361a333dfe0eae82981f4e24cf1b7fddeb6affaaf76866`.
The textdata page was captured on 2026-09-14 at 20:02 UTC, SHA-256
`b91ee875eb9d9b17ba825dc59ad8789b65f6d12661df0e5dffcb578668ac6822`.
These are dated snapshots. Fetch and validate a new source before presenting
rankings as current; review the CSV diff and refresh derived comparisons together.

```sh
snapshot_dir=$(mktemp -d)
curl --fail --show-error --silent https://mattmahoney.net/dc/text.html -o "$snapshot_dir/text.html"
python3 -B tools/ltcb/transcribe_ltcb_table.py --html "$snapshot_dir/text.html" --output "$snapshot_dir/ltcb-main-table.csv"
```

## Snapshot comparison

In the 2026-09-15 snapshot, cadapt 0.12.0 --max --allocation context has
150,063,628 B of enwik9 data plus a 209,893 B decompressor ZIP:
**150,273,521 B total, rank 30 of 226**, with reported memory of 661 MB.
The least reported memory among higher-ranked entries is 1,542 MB for drt|lpaq9m.
These statistics describe that submission, not the current development build.

`tools/ltcb/ltcb_goal.py` computes comparison thresholds from the CSV. For
place 20, paq8px_v206fix1 totals 125,099,359 B. A 209,893 B decompressor
would require at most 124,889,465 B of enwik9 data to overtake it. This is
arithmetic on the snapshot, not a product acceptance target.

## How the tables were transcribed

The main table is transcribed by
[tools/ltcb/transcribe_ltcb_table.py](../tools/ltcb/transcribe_ltcb_table.py)
(tests in [test_transcribe_ltcb_table.py](../tools/ltcb/test_transcribe_ltcb_table.py)),
which writes down the rules below; the textdata table was transcribed by
hand under the same rules on 2026-09-14. Each page carries its table in a
`<pre>` block. The block is reduced to text (tags removed, entities decoded)
and every row is matched against a single pattern anchored on the
right-hand numeric columns. A row that does not match is an error, never a
skipped row. Two places need a rule beyond the pattern:

- **Single time value.** Twelve main-table rows publish only one of Comp and
  Decomp. Numbers in the table are right-aligned, so the value was assigned by
  its starting column: a value beginning left of the `Decomp` header is Comp,
  otherwise Decomp. Two figures the page runs together (`108960~110000`) are
  split at the `~`, and a row without flag letters (NTFS) keeps them empty.
- **Program versus options.** The page separates the two by alignment, not by
  a delimiter. `program` is the text before the first run of two or more
  spaces; when a long name ran into the options column with single spaces,
  the split is before the first `-`-prefixed token. Three rows
  (`m03exp 2005-02-15 32MB blocks`, `libzling 20160107 e4`,
  `KuaiZip 2.3.2 x86`) are not separable on the page and are kept whole in
  `program`. `published_label` holds the verbatim page text (whitespace
  collapsed) so the split never loses information; for every row
  `program + " " + options` equals `published_label`.

## `ltcb-main-table.csv`

Column meanings follow the page's own definitions.

| Column | Meaning |
| --- | --- |
| `rank` | Position in the published table. The page states the table is ranked by `enwik9_plus_prog_bytes`; one adjacent inversion exists on the page (positions 95–96). |
| `published_label` | Program and options exactly as printed. |
| `program`, `options` | Heuristic split of `published_label` (see above). |
| `enwik8_bytes` | Compressed size of the first 10^8 bytes. Empty for 9 rows the page lists without an enwik8 result. Not part of the ranking. |
| `enwik9_bytes` | Compressed size of the first 10^9 bytes. |
| `decompressor_zip_bytes` | Size of a zip archive holding the decompressor and everything it needs (dictionaries, model files). |
| `decompressor_flags` | Page letters: `x` executable size, `s` source size (when smaller), `d` separate decompression program; `0` with `xd` means a self-extracting archive. |
| `enwik9_plus_prog_bytes` | `enwik9_bytes + decompressor_zip_bytes` as printed. The `glza 0.12` row prints the total equal to `enwik9_bytes` despite a nonzero decompressor size; the printed value is kept. |
| `comp_ns_per_byte`, `decomp_ns_per_byte` | Time in nanoseconds per byte on the largest file tested (numerically, seconds for enwik9). Approximate, on assorted machines, no effect on ranking. Empty Decomp means decompression was not tested and the entry is pending verification. |
| `comp_approximate`, `decomp_approximate`, `mem_approximate` | `1` when the page prefixes the value with `~` ("very approximate"). |
| `mem_mb` | Peak compression memory in MB as measured by the tester. |
| `alg` | Algorithm family as classified on the page (`CM`, `PPM`, `BWT`, `LZ77`, `Tr`, `LSTM`, …). Empty for four old rows. |
| `note` | Footnote number on the page, usually naming the tester and hardware. |
| `comp_pareto`, `decomp_pareto`, `mem_pareto` | `1` when the page underlines the value, meaning no better-ranked compressor is faster (or uses less memory). |

## `ltcb-textdata-text8-fil9.csv`

The June 2006 experiment on the "About the Test Data" page comparing raw
Wikipedia text with cleaned text. The page notes that for this table
"Decompression was not verified. Speed was not measured." and that the
options were tuned for enwik9.

| Column | Meaning |
| --- | --- |
| `program_options` | Program, version and options as printed. |
| `text8_bytes` | Compressed size of text8 (first 10^8 bytes of fil9). |
| `enwik8_bytes` | Compressed size of enwik8, taken by the page from its main table at the time. |
| `t8_over_e8` | `text8_bytes / enwik8_bytes` as printed. |
| `alg`, `mem_mb` | As on the main table of the time. |
| `fil9_bytes`, `fil9_ratio` | Compressed size and ratio for fil9 (713,069,767 bytes). Empty for the 10 programs not run on fil9. |
| `f9_over_e9` | fil9 compression ratio divided by the enwik9 ratio. |
| `error_t8e8_minus_f9e9` | `t8_over_e8 − f9_over_e9`, the page's estimate of how far a text8 test under-predicts fil9. |
| `note` | Footnote number on the main table of the time. |

## Dashboard

`ltcb-dashboard.svg` (`make ltcb` writes it; git ignores it) (and the same figure as
`ltcb-dashboard.png`) is rendered from the main-table CSV
by [plot_ltcb_tables.py](../tools/ltcb/plot_ltcb_tables.py); its tests are in
[test_plot_ltcb_tables.py](../tools/ltcb/test_plot_ltcb_tables.py). Matplotlib is the only
dependency beyond the standard library (`bench/requirements.txt`). To
regenerate after refreshing the CSV:

```sh
python3 -B tools/ltcb/plot_ltcb_tables.py --output research/ltcb-dashboard.svg
python3 -B -m unittest discover -s tools/ltcb -p 'test_*.py'
```

Within the same plotting environment, the same inputs produce deterministic
SVG and PNG output.
Every number on the figure is computed from the published columns; the only
derived quantities are bits per byte (8 × compressed ÷ 10^9 or 10^8), the
speed/ratio frontier (entries no other entry beats on both axes), medians,
and six algorithm families that fold the page's `alg` labels: context mixing
(`CM`, `CM1`), neural (`Tr`, `LSTM`), `PPM`, BWT (`BWT`, `BWTS`, `ST`),
LZ / dictionary (`LZ77`, `LZ`, `LZSS`, `LZW`, `ROLZ`, `LZP`, `Dict`, `Gram`,
`BPE`) and Other (order-n, `SR`, `DMC`, `MTF`, and the four rows with no
label). Panels that need a timing or an enwik8 result use only the rows that
publish one and say how many that is.

## Phase map of the speed/ratio frontier

`ltcb-phases.svg` (PNG twin `ltcb-phases.png`)
is rendered by [plot_ltcb_phases.py](../tools/ltcb/plot_ltcb_phases.py), tested in
[test_plot_ltcb_phases.py](../tools/ltcb/test_plot_ltcb_phases.py), and the numbers behind
it are written to `ltcb-phases.csv`. The frontier is the set
of timed entries no other entry beats on both speed and enwik9 ratio. A phase
is a maximal run of frontier points held by one algorithm family; it holds
from its first point until the next family's first point. A transition is that
next point, with the ratio it gains over the previous phase's last point and
the time it costs. The lower strip shows the marginal gain of each frontier
step in bits per byte per tenfold time: a cliff is a spike, a plateau a trough.

Timing comparisons use the reported hardware-dependent values. The phase map
summarizes this snapshot; it does not establish a controlled speed advantage
between algorithms. Regenerate the figure and underlying CSV after updating
the input table rather than copying their results into this guide.

```sh
python3 -B tools/ltcb/plot_ltcb_phases.py --local research/ltcb-local-entries.csv \
    --output research/ltcb-phases.svg
```

## Statistical analysis

`ltcb-analysis.md` is the report written by
[analyze_ltcb.py](../tools/ltcb/analyze_ltcb.py) (tests in
[test_analyze_ltcb.py](../tools/ltcb/test_analyze_ltcb.py)); `ltcb-analysis.json`
holds the same numbers for programs. Standard library only: average-rank
Spearman and Pearson correlations, ordinary least squares by normal equations,
inclusive-method quartiles, and the frontier from `plot_ltcb_phases.py`. It
covers family descriptives, correlations with the enwik9 ratio, the
enwik8 → enwik9 scaling law (a power law fitted across the published pairs,
used to *predict* a local entry's enwik9 result before it is measured, with
an interval and always labelled as a prediction), memory/time elasticity
within the modelling families, the cost-of-a-bit ladder along the frontier,
distance above the frontier, decompressor-size share, decode/encode symmetry,
size removed per second of compressor time, two resource-charged indices
(MB removed per second of total compress-plus-decompress time, and the same
per gigabyte of memory), a tunable weighted cost
`bpb × (comp + decomp)^α × mem^β` with the winner mapped over a grid of α and
β, and each local entry's nearest published neighbours in (log speed, log
memory, ratio).

```sh
python3 -B tools/ltcb/analyze_ltcb.py --local research/ltcb-local-entries.csv \
    --output research/ltcb-analysis.json
```

## Interactive explorer

`ltcb-interactive.html` is a single-page explorer
assembled by [build_ltcb_interactive.py](../tools/ltcb/build_ltcb_interactive.py) from
[ltcb-interactive.template.html](../tools/ltcb/ltcb-interactive.template.html) with the
main table, the local entries, the phase table and the analysis JSON embedded
(tests in [test_build_ltcb_interactive.py](../tools/ltcb/test_build_ltcb_interactive.py),
including a syntax check of the inline script). It loads Plotly from cdnjs
and the IBM Plex faces from Google Fonts; everything else ships in the file.
One filter row scopes every chart and the table: one family emphasised at a
time in blue over grey, a name search in orange, ratio and time axis choice,
memory and time caps; the time axis can be compression, decompression or
their sum. Charts: ratio against time with the frontier, ratio against
memory, decode against encode time, the phase map with its marginal-gain
strip (computed in the page by the same rule as `plot_ltcb_phases.py`, for
whichever time axis is chosen), size removed per second, the scaling law with
the local prediction and any measurement, the weighted-cost ranking under
live α/β sliders beside the winner map, an analysis readout and a sortable
table that also lists the total-time and per-GB rates.
Light and dark themes follow the viewer. Rebuild after any input changes:

```sh
python3 -B tools/ltcb/build_ltcb_interactive.py
```

## Local entries beside the table

[ltcb-local-entries.csv](ltcb-local-entries.csv) holds this repository's own
measurements in LTCB's terms, kept apart from the published table so the two
are never confused. The dashboard draws them as black diamonds and states the
position each would take in the published ranking (`would rank #n`), computed
as one plus the number of published entries with a strictly smaller value.
Columns match the main table where the meaning is identical; `timed_on` says
which file the ns-per-byte figures come from, and `source` names the commit,
binary hash and measurement records behind each number. An entry without an
`enwik9_bytes` value appears only where enwik8 alone places it (the KPI tile
and the enwik8 axis); LTCB's own ranking needs enwik9.

Regenerate with the local entries included:

```sh
python3 -B tools/ltcb/plot_ltcb_tables.py --local research/ltcb-local-entries.csv \
    --output research/ltcb-dashboard.svg
```

## Relationship to this repository

The uncompressed enwik8 and text8 members of the pinned ZIP fixtures use the
checksums recorded for the benchmark (enwik8 MD5 `a1fa5ffddb56f4953e226637dabbb36a`,
text8 MD5 `3bea1919949baf155f99411df5fada7e`), so `enwik8_bytes` in either
CSV is directly comparable with a local full-enwik8 container size. LTCB ranks by enwik9 plus decompressor size. A row lacking enwik9 can
be compared only on columns for which it has measurements.
