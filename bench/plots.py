"""Render every numeric run/sample metric using Matplotlib, plus an HTML index."""

from __future__ import annotations

import csv
import html
import math
import os
import tempfile
from pathlib import Path
from typing import Any

from telemetry import SAMPLE_FIELDS, atomic_json, export_csv, load_runs

COLORS = {"normal": "#0072B2", "fast": "#009E73", "max": "#D55E00"}
OVERVIEW = [
    "elapsed_seconds",
    "throughput_mib_s",
    "compression_percent",
    "peak_rss_bytes",
    "cpu_percent",
    "output_bytes",
]


def metric_label(name: str) -> tuple[str, float]:
    """Return an explicit display unit and divisor without changing the raw data."""
    special = {
        "compression_percent": ("Output / input (%)", 1),
        "cpu_percent": ("CPU utilization (%)", 1),
        "rss_bytes": ("Resident memory (MiB)", 1 << 20),
        "peak_rss_bytes": ("Peak resident memory (MiB)", 1 << 20),
    }
    if name in special:
        return special[name]
    if name in ("record_period_bytes", "class_history_bytes", "framing_bytes"):
        return name.removesuffix("_bytes").replace("_", " ") + " (bytes)", 1
    if name.endswith("_bytes"):
        return name.removesuffix("_bytes").replace("_", " ") + " (MiB)", 1 << 20
    if name.endswith("_seconds"):
        return name.removesuffix("_seconds").replace("_", " ") + " (s)", 1
    if name.endswith("_percent"):
        return name.removesuffix("_percent").replace("_", " ") + " (%)", 1
    if name == "throughput_mib_s":
        return "Encoding throughput (MiB/s)", 1
    if name.endswith("_chars"):
        return name.replace("_", " ") + " (bytes)", 1
    if name in ("format_version", "context_set"):
        return name.replace("_", " ") + " (code)", 1
    if name == "bits_per_input_byte":
        return "Compressed bits per input byte", 1
    return name.replace("_", " ") + " (count)", 1


def read_samples(record: dict[str, Any]) -> list[dict[str, float | None]]:
    """Keep missing samples missing, rather than treating them as zero readings."""
    path = Path(record["_directory"]) / "samples.csv"
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as source:
        return [{key: float(value) if value else None for key, value in row.items()} for row in csv.DictReader(source)]


def plot_runs(paths: list[Path], output: Path) -> list[Path]:
    """Create paginated PNG/SVG charts and a searchable run table for all history."""
    records = load_runs(paths)
    if not records:
        raise ValueError("no run.json records found; collect a run first")
    output.mkdir(parents=True, exist_ok=True)
    # Plotting is loaded only after all measured processes have exited.
    with tempfile.TemporaryDirectory(prefix="cadapt-matplotlib-") as cache:
        os.environ.setdefault("MPLCONFIGDIR", cache)
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        plt.rcParams.update(
            {
                "font.size": 9,
                "axes.spines.top": False,
                "axes.spines.right": False,
                "axes.titleweight": "bold",
                "figure.facecolor": "#f8fafc",
                "axes.facecolor": "white",
            }
        )
        samples = [read_samples(record) for record in records]
        colors = [COLORS[record["config"]["profile"]] for record in records]
        indices = list(range(1, len(records) + 1))
        keys = sorted({key for record in records for key in record["metrics"]})
        metrics = [key for key in OVERVIEW if key in keys] + [key for key in keys if key not in OVERVIEW]
        artifacts: list[Path] = []
        figures: list[tuple[str, str]] = []

        def save(figure: Any, name: str, title: str) -> None:
            for extension in ("png", "svg"):
                path = output / f"{name}.{extension}"
                figure.savefig(path, dpi=150, bbox_inches="tight")
                artifacts.append(path)
            plt.close(figure)
            figures.append((name, title))

        for start in range(0, len(metrics), 6):
            figure, axes = plt.subplots(3, 2, figsize=(14, 12), layout="constrained")
            page = start // 6 + 1
            figure.suptitle(f"Encoding runs · summary {page} · {len(records)} runs", fontsize=17)
            for axis, key in zip(axes.flat, metrics[start : start + 6]):
                label, scale = metric_label(key)
                values = [record["metrics"].get(key) for record in records]
                for index, value, color, record in zip(indices, values, colors, records):
                    if value is not None and math.isfinite(value):
                        axis.bar(
                            index,
                            value / scale,
                            color=color,
                            hatch="//" if record["status"] != "ok" else None,
                            alpha=0.85,
                        )
                    else:
                        axis.text(
                            index,
                            0.04,
                            "n/a",
                            rotation=90,
                            ha="center",
                            va="bottom",
                            fontsize=7,
                            transform=axis.get_xaxis_transform(),
                        )
                axis.set(title=label[0].upper() + label[1:], ylabel=label, xlabel="Run number (see table)")
                axis.set_xlim(0.3, len(records) + 0.7)
                axis.set_ylim(bottom=0)
                if len(records) <= 30:
                    axis.set_xticks(indices)
                axis.grid(axis="y", alpha=0.2)
            for axis in list(axes.flat)[len(metrics[start : start + 6]) :]:
                axis.set_visible(False)
            save(figure, f"summary-{page:02d}", f"Run totals · page {page}")

        series_keys = [key for key in SAMPLE_FIELDS if key != "elapsed_seconds"]
        for start in range(0, len(series_keys), 6):
            figure, axes = plt.subplots(3, 2, figsize=(14, 12), layout="constrained")
            page = start // 6 + 1
            figure.suptitle(f"Encoding runs · resource timelines {page}", fontsize=17)
            for axis, key in zip(axes.flat, series_keys[start : start + 6]):
                label, scale = metric_label(key)
                plotted = False
                for index, record, rows, color in zip(indices, records, samples, colors):
                    points = [(row["elapsed_seconds"], row.get(key)) for row in rows]
                    # NaNs leave gaps for unavailable readings instead of joining over them.
                    if not any(value is not None for _, value in points):
                        continue
                    plotted = True
                    axis.plot(
                        [x for x, _ in points],
                        [value / scale if value is not None else math.nan for _, value in points],
                        color=color,
                        linestyle=("-", "--", ":", "-.")[(index - 1) % 4],
                        linewidth=1.1,
                        label=f"{index}: {record['config']['profile']} r{record['config']['repeat']}",
                    )
                axis.set(title=label[0].upper() + label[1:], ylabel=label, xlabel="Elapsed wall time (s)")
                axis.set_ylim(bottom=0)
                axis.grid(alpha=0.2)
                if plotted:
                    axis.legend(fontsize=6, ncol=2)
                else:
                    axis.text(0.5, 0.5, "No samples available", ha="center", va="center", transform=axis.transAxes)
            for axis in list(axes.flat)[len(series_keys[start : start + 6]) :]:
                axis.set_visible(False)
            save(figure, f"timeline-{page:02d}", f"Resource timelines · page {page}")

    export_csv(records, output / "runs.csv")
    atomic_json(
        output / "plots.json",
        {
            "schema": "cadapt.telemetry.plots.v1",
            "run_ids": [record["run_id"] for record in records],
            "summary_metrics": {key: f"summary-{index // 6 + 1:02d}" for index, key in enumerate(metrics)},
            "timeline_metrics": {key: f"timeline-{index // 6 + 1:02d}" for index, key in enumerate(series_keys)},
            "artifacts": [path.name for path in artifacts],
        },
    )
    table_rows = []
    for index, record in zip(indices, records):
        config, fixture, measured = record["config"], record["fixture"], record["metrics"]
        values = [
            index,
            record["run_id"],
            record["status"],
            config["profile"],
            config["repeat"],
            config["memory_bytes"] // (1 << 20),
            config["threads"],
            config["verify"],
            fixture["name"],
            fixture["bytes"],
            "prefix" if fixture["is_prefix"] else "full",
            fixture["sha256"][:12],
            record["binary"]["sha256"][:12],
            measured.get("elapsed_seconds"),
            measured.get("output_bytes"),
            record.get("error") or "",
        ]
        table_rows.append(
            "<tr>"
            + "".join(f"<td>{html.escape(str(value)) if value is not None else 'n/a'}</td>" for value in values)
            + "</tr>"
        )
    headings = [
        "#",
        "Run",
        "Status",
        "Profile",
        "Repeat",
        "Budget MiB",
        "Requested threads",
        "Verify",
        "Corpus",
        "Input bytes",
        "Extent",
        "Input SHA-256",
        "Binary SHA-256",
        "Seconds",
        "Output bytes",
        "Error",
    ]
    images = "".join(
        f'<section><h2>{title}</h2><a href="{name}.svg">Open SVG</a><a href="{name}.png">Open PNG</a>'
        f'<img loading="lazy" src="{name}.png" alt="{title}"></section>'
        for name, title in figures
    )
    document = """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>cadapt · Encoding telemetry</title><style>
body{font:15px system-ui,sans-serif;margin:0;background:#f8fafc;color:#14243a}main{max-width:1500px;margin:auto;padding:32px}
h1{font-size:32px;margin-bottom:8px}p{line-height:1.6}section{margin:32px 0;background:white;padding:20px;border:1px solid #d9e1eb;border-radius:8px}
img{width:100%;height:auto}a{color:#0066a6;margin-right:16px}input{padding:10px;font:inherit;width:min(90%,500px)}
.table{overflow:auto;max-height:500px}table{border-collapse:collapse;font-size:12px;width:100%}td,th{padding:9px;text-align:left;border-bottom:1px solid #d9e1eb;white-space:nowrap}
th{position:sticky;top:0;background:#eaf0f6}small{color:#45566d}button{padding:8px;margin-right:12px}
@media(prefers-color-scheme:dark){body{background:#101b2b;color:#edf2f8}section{background:#192638}a{color:#7ecbff}th{background:#263c55}small{color:#b5c8da}}
</style><main><h1>Encoding telemetry</h1>
"""
    succeeded = sum(record["status"] == "ok" for record in records)
    document += f"<p>{len(records)} recorded runs · {succeeded} successful · {len(records) - succeeded} failed or unfinished.</p>"
    document += """<p>Each run remains separate. Blue: normal; green: fast; orange: max. Hatched bars mark unsuccessful runs.
Missing readings are n/a. Timings include verification only where Verify is true; full corpora and prefixes are identified below.</p>
<p><a href="runs.csv" download>Download all run metrics (CSV)</a></p>
<section><h2>Run details</h2><label for="filter">Filter run table </label><input id="filter" type="search" placeholder="Profile, status, corpus or run ID">
<div class="table"><table><thead><tr>"""
    document += "".join(f"<th>{heading}</th>" for heading in headings)
    document += "</tr></thead><tbody>" + "".join(table_rows) + "</tbody></table></div></section>" + images
    document += """<small>CPU percentages use one logical CPU = 100%. Input file position includes read-ahead and may reset during verification;
it is not encoded-byte progress. Disk bytes are sampled kernel I/O counters; cached reads may produce zero physical I/O.
Sampled context switches refer to the main thread; GNU time totals cover the command. A successful encode without --verify does not establish a round-trip proof.
Table filtering does not alter the plots, which always show all runs.</small></main>
<script>document.getElementById('filter').addEventListener('input',event=>{const query=event.target.value.toLowerCase();
document.querySelectorAll('tbody tr').forEach(row=>row.hidden=!row.textContent.toLowerCase().includes(query));});</script></html>"""
    (output / "index.html").write_text(document, encoding="utf-8")
    print(f"Plots: {output / 'index.html'}", flush=True)
    return artifacts
