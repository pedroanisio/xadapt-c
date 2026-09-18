# Codec measurement tools

Part of [adapt](../README.md). The maintained tools here measure codec behavior
and test measurement primitives. `make check` includes their regression suites.
They are not runtime dependencies of the codec. Full benchmark collection is
explicit; general benchmark orchestration is also available through codecbench.

## Requirements and output

Collection requires Linux, Python and GNU `time`. Plotting additionally uses
the packages in `requirements.txt`. `telemetry.py` supports Python 3.10+;
`adaptive_experiment.py` uses Python 3.11+ for streaming file hashes.
Build the executable with `make` before collecting measurements.

Choose output directories outside the checkout and build context. BENCH_OUTPUT
and BENCH_PLOTS below are shell variables, not implicit tool configuration.
`--output` is required for collection and plotting. No run directory is
selected inside the source checkout by default.

```sh
BENCH_OUTPUT=$(mktemp -d)
BENCH_PLOTS=$(mktemp -d)
python3 -m pip install -r bench/requirements.txt
make
python3 -B bench/telemetry.py run --output "$BENCH_OUTPUT" --repeats 3
python3 -B bench/telemetry.py plot "$BENCH_OUTPUT" --output "$BENCH_PLOTS"
```

The default input is the decompressed enwik8 member of
`static/corpus/enwik8.zip`. Runs default to normal, fast and max profiles,
one thread and a 674 MiB requested budget. `--memory` requests a byte budget
which the runner maps to an encoder tier; it does not pass a decoder cap to
encode. `--profiles`, `--threads`, `--segment-bytes`, `--repeats` and
`--max-bytes` control the matrix. A prefix run is identified as partial input.
Use `--help` for the complete command interface.

Each collection has a unique directory with executable/input identity,
configuration, per-run commands, status, sampled metrics and raw process
measurements. Failed or interrupted runs remain explicit failures. `--verify`
includes encoder round-trip proof in timing; it is disabled by default.
Do not compare measurements with different verification policies as equivalent.

## Metrics and comparison

`codec_bench.py` runs each executable in a separate process and reports complete
output bytes, encode/decode CPU and wall seconds, peak RSS, and byte-exact
restoration. A failure to restore makes the run fail. CPU time measures total
CPU work; wall time measures latency and includes scheduling effects.

```sh
unzip -p static/corpus/enwik8.zip enwik8 > "$BENCH_OUTPUT/enwik8"
python3 -B bench/codec_bench.py --input "$BENCH_OUTPUT/enwik8" --scratch "$BENCH_OUTPUT"
```

`xz`, `zstd` and `kanzi` are discovered on PATH. `--kanzi` selects an explicit
external executable. `--only` filters codec configurations. `--json` writes
the resulting measurements to an explicitly selected file.

The authoritative regression floors are under `tools/`; `make gate`,
`make ratio-floor` and `make corpus-floor` evaluate them. This directory has
no independent floor definition. Shared hosts use alternating candidate/base
runs; absolute throughput comparisons require the reference environment.

## Measurement instruments

- `adaptive_experiment.py` compares a baseline executable with adaptive
  candidates on complete files and folders. It requires an output directory
  and records verification separately from timed encoding.
- `redundancy_probe.c` measures repeated byte/context structure.
- `stream_census.c` partitions candidate-transform opportunities without
  counting a byte twice. Its estimated removable bytes are bounds, not actual
  compression gains.
- `markup_book_proto.c` tests a reversible markup transform independently of
  the codec wire format. Its output is not a cadapt container.
- `slot_price.py` measures context ablations using a development build with
  `ADAPT_SIZES_OVERRIDE`. `ADAPT_DROP_MASK` selects omitted contexts;
  `ADAPT_SIZES` and `ADAPT_CLASS_CODE` control development sizing and class
  history; ADAPT_MODEL2_SLOTS selects model2 slot experiments. These outputs require the same development configuration to decode.
- `probe_util.h` shares bounded parsing, hashing and I/O primitives between
  the probes. Tests cover inverse transforms, limits and failure behavior.
- `fetch_scripts.py` fetches the multilingual corpus with article/revision
  provenance and hashes in `static/corpus/scripts/manifest.json`.

## Interpretation

An algorithm explanation or microbenchmark does not prove a complete-container
gain. Compare verified outputs on the same inputs and account for metadata,
search work, model memory and initialization. Keep measurement records separate
from source and promote only reviewed, self-contained evidence into release
documentation or machine-readable regression definitions.
