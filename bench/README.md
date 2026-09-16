# Historical encoding telemetry

<!-- Implementation authored with Codex (GPT-6); fetch_scripts.py with Claude Opus 5 (Claude Code). Telemetry values are measured by the executable tools. -->

Part of [adapt](../README.md). These scripts and result bundles are retained
for historical research. New general benchmark runs belong to the standalone
codecbench tool. Ordinary codec builds and tests do not load this directory.
The active codec regression gates and floor data live under `tools/`; the old
floor files here are historical snapshots. The instructions below describe the
retained collector, and do not make it a build or test dependency.

`telemetry.py` runs the C encoder against a corpus, keeps every run's evidence,
and plots all numeric totals and sampled metrics. The default fixture is
`static/corpus/enwik8.zip`, member `enwik8`: the full 100,000,000-byte input.
It benchmarks the decompressed corpus, not the ZIP bytes.

## Run

Collection requires Linux, Python 3.10+, and GNU `time` (normally `/usr/bin/time`).
Only plotting needs a Python package:

```sh
python3 -m pip install -r bench/requirements.txt
make
python3 -B bench/telemetry.py run
```

The default matrix runs normal, fast, and max once each, sequentially, with the
codec's default 674 MiB model budget (`cadapt -9`) and verification off. Each invocation creates a fresh session
under `bench/results/`. Full-corpus runs may take several minutes per profile.
No benchmark runs during `make` or `make check`.

```sh
# Repeat all three profiles on the full corpus (`make bench` no longer runs
# this collector: it prints the codecbench migration notice and exits 1).
python3 -B bench/telemetry.py run --repeats 3

# Compare budgets and segment configurations, preserving every repetition.
python3 -B bench/telemetry.py run --repeats 2 --memory 64MiB 128MiB --threads 1 2

# Include the encoder's round-trip verification in the measured time.
python3 -B bench/telemetry.py run --verify

# Quick fixture smoke test: clearly labelled as a 1 MiB prefix, not full enwik8.
python3 -B bench/telemetry.py run --max-bytes 1MiB --profiles fast

# Another raw corpus or a named ZIP member.
python3 -B bench/telemetry.py run --corpus /path/to/corpus.bin
python3 -B bench/telemetry.py run --corpus /path/to/corpus.zip --member corpus.bin

# Collect without Matplotlib; render later.
python3 -B bench/telemetry.py run --no-plots
```

`--interval` controls sampling (default 0.1 seconds); `--timeout` limits each run
(default 3,600 seconds). A failed configuration remains in the history and the
matrix continues. Ctrl-C terminates the active encoder process group, records an
interrupted run, and leaves the session incomplete. The command returns nonzero
if any run fails or the session is incomplete.

`--threads` uses the CLI's segment settings. The C codec still encodes segments
sequentially; a verifier thread can run when `--verify` is enabled. The sampled
thread count shows actual process threads.

## Locked floors and the CI gate

`ratio_floor.json` holds the floors locked in `LOCKS.md` (2026-09-14): the
largest container and lowest throughput `cadapt encode --max --allocation
context` may produce on the first 8 MiB of enwik8 (the CI proxy), the first
16 MiB (pre-merge), full enwik8 and full enwik9 (release). `check_ratio_floor.py`
encodes a fixture with the current binary, refuses inputs whose size or MD5
differ from the locked ones, gates size exactly, gates throughput on the
reference host or relative to `--baseline-binary` elsewhere (and says so when it
cannot), and with `--verify` decodes and compares byte for byte.

```sh
make gate                                   # 8 MiB proxy, encode + verify, < 120 s
make ratio-floor                            # full enwik8 (static/corpus/enwik8 unzipped)
make ratio-floor FLOOR_ARGS='--fixture enwik8=static/corpus/enwik8 --fixture enwik9=/path/enwik9'
python3 -B bench/check_ratio_floor.py --fixture enwik8-8MiB=build/enwik8-8MiB \
    --baseline-binary /path/to/locked-commit/cadapt   # any host: relative throughput
python3 -B -m unittest discover -s bench -p 'test_check_ratio_floor.py'
```

Exit 0 means every requested floor holds, 1 that a floor is broken, 2 that the
check could not run. The proxy cannot see the allocation policy (identical
bytes below 32 MiB), which is why the full floors exist.

## Plot all runs

```sh
# All recorded sessions, including failures; never reruns encoding.
make plot

# One session, or several selected sessions.
python3 -B bench/telemetry.py plot bench/results/SESSION --output /tmp/enwiki-plots
python3 -B bench/telemetry.py plot SESSION_A SESSION_B --output /tmp/combined-plots
```

Open `bench/plots/index.html` for the full-history report. Individual sessions
also get `plots/index.html` automatically unless `--no-plots` was used. The report
contains a searchable run table, a CSV export, and PNG/SVG plots. Every numeric
total and every sampled metric has a panel, split across pages of six panels.
Runs are never silently averaged together. Failed/unfinished runs remain visible;
unavailable readings are marked `n/a` rather than zero. Table filtering leaves
the plots showing all runs.

The table identifies corpus size, full/prefix extent, input hash, executable hash,
verification mode, budget and segment settings. Compare matching input hashes and
sizes when assessing speed or compression changes.

## Recorded evidence

Each session contains:

| Path | Contents |
| --- | --- |
| `session.json` | Planned matrix, expected/finished/successful counts, configuration, host and binary provenance |
| `cadapt`, `corpus.bin` | Snapshots of the executable and exact benchmark input |
| `collector.py`, `plotter.py` | Source snapshots for provenance; the executable entry point remains `bench/telemetry.py` |
| `runs.jsonl` | Append-only finished-run records, flushed after each attempt |
| `runs.csv` | Flat configuration and metric export, including failures |
| `runs/NNNN-PROFILE-rN/run.json` | Atomic checkpoint written before execution and finalized afterwards |
| `runs/NNNN-PROFILE-rN/samples.csv` | Resource samples flushed while the encoder runs |
| `runs/NNNN-PROFILE-rN/resources.txt` | Original GNU time output, including unsuccessful exit diagnostics |
| `runs/NNNN-PROFILE-rN/stderr.txt` | Encoder output and error messages |
| `runs/NNNN-PROFILE-rN/encoded.adpt` | Published encoded result when successful |
| `plots/` | Report, numeric CSV export, summary and timeline charts; `plots.json` maps every metric to its chart |

`complete` means all planned attempts finished (an interrupt is incomplete).
`all_successful` additionally requires every run to succeed. An abrupt process or
machine failure can leave a `running` checkpoint: its outcome is unknown, and the
plotter includes it as unfinished. Replotting reads these checkpoints without
changing the measurements.

The totals cover elapsed time, observed wall time, user/system CPU time, average
CPU utilization, peak resident memory, major/minor page faults, voluntary and
involuntary context switches, filesystem I/O blocks, signals and swaps. Successful
runs add input/output sizes, compression percentage, bits per input byte,
throughput, container version, context/recall/window sizes, class history,
context-set code, record period, segment count, block-method counts, payload and
framing sizes. The record also preserves CRC-32 and SHA-256 identities.

Time-series panels cover sampled CPU, user/system CPU time, RSS, virtual memory,
peak RSS, swap, thread count, page faults, main-thread context switches, logical
I/O bytes/calls, physical disk I/O bytes, cancelled writes, input-file position,
and output-file size including the staged file.

## Measurement meaning

- GNU time measures the codec child, avoiding the Python plotter's memory footprint
  in the codec's peak RSS. Its elapsed/user/system times have centisecond resolution.
  Runs below that resolution retain their observed wall time, but throughput and
  average CPU utilization are `n/a` when GNU elapsed time is zero.
- `observed_wall_seconds` includes starting and observing the timing wrapper.
  Plotting, input preparation/hashing, and container inspection happen outside the
  measured encode. The executable and input are snapshotted once per session.
- CPU utilization uses one logical CPU = 100%, so threaded verification can exceed
  100%. Samples can miss short-lived peaks; peak RSS totals come from GNU time.
- Proc context-switch samples refer to the main thread; GNU time totals cover the
  command. Physical disk I/O can be zero when data is cached. Logical I/O also
  includes executable/runtime reads and is not a count of encoded corpus bytes.
- Input-file position includes read-ahead and can reset during verification. It
  is not exact encoding progress. Output size follows the staged file until it is
  published. Samples unavailable during process exit are left missing.
- Input preparation warms the filesystem cache. These are sequential measurements
  on the current host, not cold-cache or isolated-machine claims. Host load and
  affinity are recorded; avoid other CPU-heavy work during comparisons.
- Without `--verify`, success means encoding completed and its framing/declared
  length/CRC metadata agree with the input. This does not establish decoded-byte
  equality. Enabling `--verify` invokes the codec's actual round-trip proof.
- No hardware performance counters or internal per-bit predictor events are
  exposed by the current CLI. This tool records the available process/container
  telemetry; it does not infer cache misses, energy usage or per-block CPU costs.

References: [GNU time fields](https://www.gnu.org/software/time/manual/html_node/Resource-Measurement.html),
[Linux proc fields](https://docs.kernel.org/filesystems/proc.html), and
[Matplotlib figures](https://matplotlib.org/stable/api/_as_gen/matplotlib.pyplot.subplots.html).

The small regression suite is included in `make check` and uses no plotting
dependency or full-corpus benchmark:

```sh
python3 -B bench/test_telemetry.py adpat/c/build/cadapt
```

## The per-script corpus rows

`fetch_scripts.py` builds the rows `FMT-8` measures — Latin (English),
Cyrillic (Russian), CJK (Chinese, Japanese) and diacritic-heavy Latin
(Vietnamese, Czech), about 2 MiB of Wikipedia article text each, CC BY-SA —
into `static/corpus/scripts/<row>.zip` (member `<row>.txt`) with a
`manifest.json` naming every article, its revision and the row's hash.
Selection is deterministic (the first non-redirect articles above a size
floor, in title order); content drifts with edits, and the manifest's hashes
say whether it did. Standard library only.

```sh
python3 bench/fetch_scripts.py                                   # every row
python3 bench/fetch_scripts.py --rows cjk-ja --target 1MiB
python3 bench/fetch_scripts.py --from-manifest static/corpus/scripts/manifest.json
python3 bench/telemetry.py run --corpus static/corpus/scripts/cyrillic-ru.zip --member cyrillic-ru.txt
```

The measurements the rows gave are in
[research/scripts-measurement.md](../research/scripts-measurement.md).


## The redundancy probe

`redundancy_probe.c` prices the two front-end shortcuts that make
byte-oriented context-mixing compressors fast, against an input, before
either is built into the codec: a Bloom-style LZP literal-prediction bypass,
and a flag-free gate on contexts that have settled on one following byte.
It reads the input and writes nothing, so it reports what a shortcut could
*cover*, never what it would cost in container bytes — that number comes from
an encode. Standard library only; no codec source or constant is involved.

```sh
cc -std=c99 -O2 -o /tmp/redundancy-probe bench/redundancy_probe.c
/tmp/redundancy-probe static/corpus/enwik8 --limit 8388608
/tmp/redundancy-probe static/corpus/enwik8 --limit 8388608 --min-seen 8
python3 -B bench/test_redundancy_probe.py          # builds its own copy
```

The measurements it gave, and the experiments they rank, are in
[research/speed-3x-experiments-2026-09-16.md](../research/speed-3x-experiments-2026-09-16.md).

## The slot price table

`slot_price.py` measures what each of the max profile's twenty hashed
contexts is worth. Every context costs three arbiter lanes of every coded bit
and one claim of every nibble, so a context that earns little is a context the
encoder is paying for. The tool encodes a fixture once per kind with that kind
dropped, compares the container against the baseline, and — with `--frontier`
— walks the kinds cheapest first to map what deleting several at once costs in
bytes and returns in seconds.

It builds and drives a development codec (`-DADAPT_SIZES_OVERRIDE`), whose
`ADAPT_DROP_MASK` exists for this ablation. Containers written under a mask
decode only under the same mask, so the tool's containers are never a
deliverable; its numbers are, and running it again reproduces them.

```sh
make                                              # the codec the end-to-end test needs
python3 -B bench/slot_price.py --fixture static/corpus/enwik8 --limit 8MiB
python3 -B bench/slot_price.py --fixture static/corpus/enwik8 --limit 8MiB \
        --frontier --json bench/results/slot-price-enwik8.json
python3 -B bench/test_slot_price.py               # stub-driven, plus one real point
```

The tables it gave for enwik8 and for three Silesia rows, and the rung they
price, are in
[research/speed-3x-experiments-2026-09-16.md](../research/speed-3x-experiments-2026-09-16.md).
