#!/usr/bin/env python3
"""Map the phases of the LTCB speed/ratio frontier and where they change.

Source: Claude Opus 5 via Claude Code. Input is ltcb-main-table.csv (published
figures, see ltcb-tables.md). The frontier is the set of entries no other entry
beats on both speed and enwik9 ratio; a phase is a maximal run of frontier
points held by one algorithm family; a transition is the frontier point where
the family changes, with the ratio it gains and the time it costs. Nothing here
is measured; Matplotlib is imported only to draw.
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path

import plot_ltcb_tables as ltcb

AXES = (("compression", "comp_ns_per_byte"), ("decompression", "decomp_ns_per_byte"))
Y_TOP = 4.4  # the frontier lives below 4 bpb; entries above are counted in the subtitle
CLUSTER_SPEED = 5e4  # frontier points past this sit within half a decade: label with leaders


@dataclass(frozen=True)
class FrontierPoint:
    speed: float
    bpb: float
    entry: ltcb.Entry


@dataclass(frozen=True)
class Phase:
    family: str
    points: list[FrontierPoint]
    end: float  # speed where the next family first holds the frontier, or the last point

    @property
    def start(self) -> float:
        return self.points[0].speed

    @property
    def gain_per_decade(self) -> float | None:
        """Ratio gained across the phase per tenfold time; None for a single point."""
        first, last = self.points[0], self.points[-1]
        if len(self.points) < 2 or last.speed <= first.speed:
            return None
        return (first.bpb - last.bpb) / math.log10(last.speed / first.speed)


@dataclass(frozen=True)
class Transition:
    speed: float
    from_family: str
    to_family: str
    bpb_drop: float
    time_ratio: float
    entry: ltcb.Entry


@dataclass(frozen=True)
class Step:
    start: FrontierPoint
    end: FrontierPoint

    @property
    def gain_per_decade(self) -> float:
        return (self.start.bpb - self.end.bpb) / math.log10(self.end.speed / self.start.speed)


def frontier_points(rows: list[ltcb.Entry], key: str) -> list[FrontierPoint]:
    timed = [(getattr(r, key), r.enwik9_bpb, r) for r in rows if getattr(r, key) is not None]
    best = float("inf")
    kept = []
    for speed, bpb, entry in sorted(timed, key=lambda t: (t[0], t[1])):
        if bpb < best:
            kept.append(FrontierPoint(speed, bpb, entry))
            best = bpb
    return kept


def phases(points: list[FrontierPoint]) -> list[Phase]:
    runs: list[list[FrontierPoint]] = []
    for point in points:
        if runs and runs[-1][0].entry.family == point.entry.family:
            runs[-1].append(point)
        else:
            runs.append([point])
    result = []
    for index, run in enumerate(runs):
        end = runs[index + 1][0].speed if index + 1 < len(runs) else run[-1].speed
        result.append(Phase(run[0].entry.family, run, end))
    return result


def transitions(found: list[Phase]) -> list[Transition]:
    result = []
    for previous, current in zip(found, found[1:]):
        last, first = previous.points[-1], current.points[0]
        result.append(Transition(first.speed, previous.family, current.family,
                                 last.bpb - first.bpb, first.speed / last.speed, first.entry))
    return result


def marginal_gains(points: list[FrontierPoint]) -> list[Step]:
    return [Step(a, b) for a, b in zip(points, points[1:]) if b.speed > a.speed]


def frontier_bpb_at(points: list[FrontierPoint], speed: float) -> float | None:
    """Best published ratio reachable at or below `speed`; None if nothing is that fast."""
    reachable = [p.bpb for p in points if p.speed <= speed]
    return min(reachable) if reachable else None


def write_phase_table(rows: list[ltcb.Entry], path: Path) -> Path:
    """Write the phase table as CSV, without drawing anything.

    The figure needs Matplotlib; the table behind it needs only the published
    entries, so anything that wants the numbers — the interactive page, a test
    — takes them from here rather than from a rendering that `make ltcb` may
    not have built.
    """
    _write_table(path, {name: frontier_points(rows, key) for name, key in AXES})
    return path


def _write_table(path: Path, per_axis: dict[str, list[FrontierPoint]]) -> None:
    fields = ["axis", "phase", "family", "program", "speed_ns_per_byte", "enwik9_bpb",
              "phase_start_ns_per_byte", "phase_end_ns_per_byte", "phase_gain_per_decade",
              "step_gain_per_decade", "transition_bpb_drop", "transition_time_ratio"]
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for axis, points in per_axis.items():
            found = phases(points)
            jumps = {t.entry.program: t for t in transitions(found)}
            steps = {s.start.entry.program: s for s in marginal_gains(points)}
            for number, phase in enumerate(found, start=1):
                for point in phase.points:
                    jump = jumps.get(point.entry.program)
                    step = steps.get(point.entry.program)
                    gain = phase.gain_per_decade
                    writer.writerow({
                        "axis": axis, "phase": number, "family": phase.family,
                        "program": point.entry.program, "speed_ns_per_byte": f"{point.speed:g}",
                        "enwik9_bpb": f"{point.bpb:.4f}",
                        "phase_start_ns_per_byte": f"{phase.start:g}",
                        "phase_end_ns_per_byte": f"{phase.end:g}",
                        "phase_gain_per_decade": "" if gain is None else f"{gain:.4f}",
                        "step_gain_per_decade": "" if step is None else f"{step.gain_per_decade:.4f}",
                        "transition_bpb_drop": "" if jump is None else f"{jump.bpb_drop:.4f}",
                        "transition_time_ratio": "" if jump is None else f"{jump.time_ratio:.3f}",
                    })


def _draw_axis(fig, spec, rows: list[ltcb.Entry], key: str, name: str,
               local: list[ltcb.LocalEntry]) -> None:
    grid = spec.subgridspec(2, 1, height_ratios=[3.2, 1.1], hspace=0.08)
    ax = fig.add_subplot(grid[0])
    ax_gain = fig.add_subplot(grid[1], sharex=ax)
    timed = [r for r in rows if getattr(r, key) is not None]
    points = frontier_points(rows, key)
    found = phases(points)
    jumps = transitions(found)
    lo, hi = 1.0, 2e7
    # Phase bands, alternating a neutral wash so the reader sees the regimes, not colors.
    for index, phase in enumerate(found):
        if index % 2 == 0:
            ax.axvspan(phase.start, phase.end, color=ltcb.GRID, alpha=0.5, linewidth=0, zorder=1)
        mid = math.sqrt(phase.start * max(phase.end, phase.start * 1.15))
        narrow = math.log10(max(phase.end, phase.start * 1.15) / phase.start) < 0.42
        ax.text(mid, Y_TOP - 0.08, f"{ltcb.SHORT_LABELS[phase.family]} · {len(phase.points)}",
                color=ltcb.INK, fontsize=8, fontweight="semibold", ha="center", va="top",
                rotation=90 if narrow else 0, zorder=5)
    ltcb._dots(ax, [getattr(r, key) for r in timed], [r.enwik9_bpb for r in timed], ltcb.CONTEXT,
               size=14, zorder=2)
    xs = [p.speed for p in points] + [hi]
    ys = [p.bpb for p in points] + [points[-1].bpb]
    ax.step(xs, ys, where="post", color=ltcb.BLUE, linewidth=2, zorder=3)
    ltcb._dots(ax, [p.speed for p in points], [p.bpb for p in points], ltcb.BLUE, size=38, zorder=4)
    # Name the first and last point of each phase: a phase's first point (where the family
    # takes the frontier) is labelled below, its last point above; the crowded tail past
    # CLUSTER_SPEED is labelled from a column with leader lines instead.
    named: dict[str, tuple[FrontierPoint, bool]] = {}
    for index, phase in enumerate(found):
        if len(phase.points) == 1:  # one point: alternate sides so neighbours stay apart
            named.setdefault(phase.points[0].entry.program, (phase.points[0], index % 2 == 1))
            continue
        named.setdefault(phase.points[0].entry.program, (phase.points[0], False))
        named.setdefault(phase.points[-1].entry.program, (phase.points[-1], True))
    cluster = [pt for pt, _ in named.values() if pt.speed >= CLUSTER_SPEED]
    for point, above in named.values():
        if point in cluster:
            continue
        ltcb._annotate(ax, point.speed, point.bpb, point.entry.program, dx=5, dy=6 if above else -13)
    if cluster:
        ltcb._spread_labels(ax, [(pt.speed, pt.bpb) for pt in cluster],
                            [f"{pt.entry.program} {pt.bpb:.3f}" for pt in cluster],
                            x_text=CLUSTER_SPEED * 12, y_top=3.1, y_step=0.27)
    # Transitions: the drop drawn as a vertical rule at the speed where the family changes,
    # quantified when it is large enough to read (>= 0.1 bpb).
    for jump in jumps:
        top = jump.bpb_drop + jump.entry.enwik9_bpb
        ax.plot([jump.speed, jump.speed], [jump.entry.enwik9_bpb, top], color=ltcb.INK2,
                linewidth=1.5, zorder=3)
        if jump.bpb_drop >= 0.5:
            ax.text(jump.speed * 1.12, (top + jump.entry.enwik9_bpb) / 2,
                    f"−{jump.bpb_drop:.2f} bpb for ×{jump.time_ratio:,.1f} time",
                    color=ltcb.INK2, fontsize=7.5, va="center", zorder=5)
        elif jump.bpb_drop >= 0.1:
            # Too short to label in place: lift the text clear of the point names on a leader.
            lift = 0.55 + 0.3 * (jumps.index(jump) % 2)
            ax.annotate(f"−{jump.bpb_drop:.2f} bpb for ×{jump.time_ratio:,.1f} time",
                        (jump.speed, (top + jump.entry.enwik9_bpb) / 2),
                        xytext=(jump.speed, top + lift), textcoords="data", fontsize=7.5,
                        color=ltcb.INK2, ha="center", va="bottom", zorder=5,
                        arrowprops={"arrowstyle": "-", "color": ltcb.AXIS, "linewidth": 0.8,
                                    "shrinkA": 0, "shrinkB": 2})
    # Locally measured entries, and their distance above the frontier at their own speed.
    for entry in local:
        speed = getattr(entry, key)
        placed = ltcb._ratio_for_enwik9_axis(entry)
        if placed is None or speed is None:
            continue
        ratio, provisional = placed
        reach = frontier_bpb_at(points, speed)
        gap = "" if reach is None else f", {ratio - reach:+.2f} bpb vs frontier"
        caption = ltcb._local_caption(entry, provisional).replace(")", gap + ")")
        ltcb._local_mark(ax, speed, ratio, caption, dy=9, provisional=provisional)
    ax.set_xscale("log")
    ax.set_xlim(lo, hi)
    ax.set_ylim(0.6, Y_TOP)
    ax.set_ylabel("enwik9, bits per byte")
    ax.tick_params(labelbottom=False)
    last = points[-1]
    hidden = sum(1 for r in timed if r.enwik9_bpb > Y_TOP)
    ax.set_title(f"{name.capitalize()} speed: {len(found)} phases across {len(points)} frontier "
                 f"points ({len(timed)} timed entries)", pad=20)
    ltcb._subtitle(ax, f"Shaded bands alternate per phase; a band runs from the first point a "
                       f"family holds to the next family's first point. Nothing beats "
                       f"{last.entry.program} ({last.bpb:.3f} bpb) past {last.speed:,.0f} ns/B. "
                       f"{hidden} entries above {Y_TOP} bpb are off the chart.")
    # Marginal gain per step: cliffs are spikes, plateaus are troughs.
    steps = marginal_gains(points)
    for step in steps:
        ax_gain.plot([step.start.speed, step.end.speed], [step.gain_per_decade] * 2,
                     color=ltcb.BLUE, linewidth=2, solid_capstyle="butt", zorder=3)
    ax_gain.axhline(0.1, color=ltcb.AXIS, linewidth=0.8, zorder=2)
    ax_gain.text(hi * 0.8, 0.1, "0.1 bpb per decade", color=ltcb.MUTED, fontsize=7.5, ha="right",
                 va="bottom")
    for step in sorted(steps, key=lambda s: -s.gain_per_decade)[:2]:
        ax_gain.annotate(f"{step.start.entry.program} → {step.end.entry.program}: "
                         f"{step.gain_per_decade:.1f}",
                         (math.sqrt(step.start.speed * step.end.speed), step.gain_per_decade),
                         xytext=(6, 0), textcoords="offset points", fontsize=7.5, color=ltcb.INK2,
                         va="center")
    ax_gain.set_xscale("log")
    ax_gain.set_yscale("log")
    ax_gain.set_ylim(0.001, 100)
    ax_gain.set_ylabel("bpb gained per\ndecade of time")
    ax_gain.set_xlabel(f"{name}, ns per byte (log)")


def render(rows: list[ltcb.Entry], output: Path, source: str = ltcb.DEFAULT_SOURCE,
           local: list[ltcb.LocalEntry] = ()) -> list[Path]:
    """Write the phase map as SVG at `output`, a PNG beside it, and the phase table as CSV."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    local = list(local)
    ltcb._style(plt)
    fig = plt.figure(figsize=(18, 15))
    outer = fig.add_gridspec(2, 1, hspace=0.3, left=0.06, right=0.98, top=0.9, bottom=0.05)
    fig.text(0.06, 0.975, "Phases of the speed/ratio frontier", color=ltcb.INK, fontsize=20,
             fontweight="semibold", va="top")
    fig.text(0.06, 0.953, "Which algorithm family holds the best enwik9 ratio at each speed, "
             "where that changes, and what each change buys. Source: " + source + ".",
             color=ltcb.INK2, fontsize=11, va="top")
    for (name, key), spec in zip(AXES, outer):
        _draw_axis(fig, spec, rows, key, name, local)
    svg = Path(output)
    png, table = svg.with_suffix(".png"), svg.with_suffix(".csv")
    fig.savefig(svg, format="svg", metadata={"Date": None})
    fig.savefig(png, format="png", dpi=100)
    plt.close(fig)
    write_phase_table(rows, table)
    return [svg, png, table]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, nargs="?", default=ltcb.RESEARCH / "ltcb-main-table.csv")
    parser.add_argument("--output", type=Path, required=True,
                        help="SVG path; a PNG and a CSV of the phases are written beside it")
    parser.add_argument("--source", default=ltcb.DEFAULT_SOURCE)
    parser.add_argument("--local", type=Path, help="CSV of locally measured entries to place")
    args = parser.parse_args(argv)
    if args.output.suffix.lower() != ".svg":
        parser.error("--output must be an .svg path")
    try:
        rows = ltcb.load_rows(args.table)
        local = ltcb.load_local(args.local) if args.local else []
    except (OSError, ltcb.TableError) as error:
        parser.error(str(error))
    for path in render(rows, args.output, source=args.source, local=local):
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
