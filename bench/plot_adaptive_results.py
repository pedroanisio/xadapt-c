#!/usr/bin/env python3
"""Plot the published causal experiment results (optional Matplotlib tooling).

Source: OpenAI GPT-6 via Codex. Inputs are measured complete-container sizes
and command timings; this figure makes no enterprise workload extrapolation.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
    """Render savings and enwik8 costs from completed, checked experiment records."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    report = json.loads(args.results.read_text())
    if (
        report.get("schema") != "cadapt.causal-results.v1"
        or report.get("complete") is not True
    ):
        parser.error("expected a complete cadapt.causal-results.v1 report")
    comparisons, enwik = [], []
    for experiment in report["experiments"]:
        if (
            not experiment["complete"]
            or len(experiment["runs"]) != experiment["expected_runs"]
        ):
            parser.error("experiment is incomplete")
        comparisons.extend(experiment["comparisons"])
        enwik.extend(row for row in experiment["runs"] if row["fixture"] == "enwik8")
    if not enwik:
        parser.error("full enwik8 measurements are required for the runtime panel")
    cohorts = {
        (e["memory_budget_bytes"], e.get("base_profile", "normal"))
        for e in report["experiments"]
    }
    if len(cohorts) != 1:
        parser.error("one figure must compare one memory budget and base profile")
    memory, base = next(iter(cohorts))
    savings = []
    for row in comparisons:
        value = (
            100 * (row["baseline_bytes"] - row["auto_bytes"]) / row["baseline_bytes"]
        )
        if (
            not row["auto_equals_smallest"]
            or not math.isfinite(value)
            or not 0 <= value <= 100
        ):
            parser.error("inconsistent selection comparison")
        savings.append(value)
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plt.rcParams.update(
        {"font.size": 10, "axes.spines.top": False, "axes.spines.right": False}
    )
    figure, (left, right) = plt.subplots(
        1,
        2,
        figsize=(14, max(9, len(comparisons) * 0.29 + 2)),
        gridspec_kw={"width_ratios": [1.25, 1]},
        layout="constrained",
    )
    figure.suptitle(
        f"Experimental codec · {base} profile · {memory // (1 << 20)} MiB budget",
        fontsize=19,
        fontweight="bold",
    )
    positions = list(range(len(comparisons)))
    colors = [
        "#B77712" if row["fixture"] == "gradient.bmp" else "#146A80"
        for row in comparisons
    ]
    left.barh(positions, savings, color=colors)
    left.set_yticks(positions, [row["fixture"] for row in comparisons])
    left.invert_yaxis()
    left.set(
        xlabel="Smaller than the unchanged codec (%)",
        title="Automatic selection: complete-container savings",
        xlim=(0, 100),
    )
    left.axvline(30, color="#A23E48", linestyle="--", linewidth=1, label="30% target")
    for y, value in zip(positions, savings):
        left.text(max(value + 1, 1), y, f"{value:.3f}%", va="center", fontsize=8)
    left.legend(loc="lower right")
    left.grid(axis="x", alpha=0.15)
    positions = list(range(len(enwik)))
    right.barh(
        [p - 0.19 for p in positions],
        [r["encode"]["metrics"]["elapsed_seconds"] for r in enwik],
        height=0.36,
        color="#146A80",
        label="Encode",
    )
    right.barh(
        [p + 0.19 for p in positions],
        [r["decode"]["metrics"]["elapsed_seconds"] for r in enwik],
        height=0.36,
        color="#91AFBA",
        label="Decode",
    )
    right.set_yticks(positions, [r["profile"] for r in enwik])
    right.invert_yaxis()
    right.set(
        xlabel="Elapsed seconds (one observation per command)",
        title="Full enwik8: 100,000,000 bytes",
    )
    right.legend(loc="lower right")
    right.grid(axis="x", alpha=0.15)
    figure.supxlabel(
        "Generated probes and a small natural holdout; not an enterprise workload sample.\n"
        "Photographic BMPs are prepared inputs; their original PNGs are measured separately.",
        fontsize=10,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(
        args.output,
        metadata={
            "Creator": "OpenAI GPT-6 via Codex; Matplotlib",
            "Description": "Measured results from " + args.results.name,
        },
    )
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
