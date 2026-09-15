#!/usr/bin/env python3
"""Render a dashboard of the Large Text Compression Benchmark main table.

Source: Claude Opus 5 via Claude Code. The input is research/ltcb-main-table.csv,
the published LTCB figures transcribed in research/ltcb-tables.md; every panel is derived
from those published values and nothing here is measured. Matplotlib is an
optional dependency (bench/requirements.txt) imported only when rendering.
"""

from __future__ import annotations

import argparse
import csv
import statistics
from dataclasses import dataclass
from pathlib import Path

DEFAULT_SOURCE = "mattmahoney.net/dc/text.html, page dated Sept 12, 2026"
RESEARCH = Path(__file__).resolve().parents[2] / "research"  # tracked data and ignored renderings
ENWIK8_BYTES = 100_000_000
ENWIK9_BYTES = 1_000_000_000

FAMILIES = ("CM", "Neural", "PPM", "BWT", "LZ", "Other")
FAMILY_LABELS = {
    "CM": "Context mixing",
    "Neural": "Neural (Transformer, LSTM)",
    "PPM": "PPM",
    "BWT": "BWT / sort transform",
    "LZ": "LZ / dictionary",
    "Other": "Other (order-n, SR, DMC, ...)",
}
SHORT_LABELS = {"CM": "Context mixing", "Neural": "Neural", "PPM": "PPM", "BWT": "BWT",
                "LZ": "LZ / dictionary", "Other": "Other"}
_FAMILY_OF_ALG = {
    "CM": "CM", "CM1": "CM",
    "Tr": "Neural", "LSTM": "Neural",
    "PPM": "PPM",
    "BWT": "BWT", "BWTS": "BWT", "ST": "BWT",
    "LZ77": "LZ", "LZ": "LZ", "LZSS": "LZ", "LZW": "LZ", "ROLZ": "LZ", "LZP": "LZ",
    "Dict": "LZ", "Gram": "LZ", "BPE": "LZ",
}
# Bit-at-a-time modelling pays the same cost to decode; string parsers do not.
SYMMETRIC_FAMILIES = ("CM", "Neural", "PPM")

REQUIRED_COLUMNS = (
    "rank", "published_label", "program", "options", "enwik8_bytes", "enwik9_bytes",
    "decompressor_zip_bytes", "decompressor_flags", "enwik9_plus_prog_bytes",
    "comp_ns_per_byte", "decomp_ns_per_byte", "mem_mb", "alg", "comp_pareto",
    "decomp_pareto", "mem_pareto",
)

# Chart chrome and the two validated categorical slots (light surface).
SURFACE, INK, INK2, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#898781"
GRID, AXIS, CONTEXT = "#e1e0d9", "#c3c2b7", "#c3c2b7"
BLUE, ORANGE = "#2a78d6", "#eb6834"


class TableError(ValueError):
    """The CSV is not the LTCB main table this module expects."""


@dataclass(frozen=True)
class Entry:
    rank: int
    label: str
    program: str
    enwik8_bytes: int | None
    enwik9_bytes: int
    decompressor_zip_bytes: int
    enwik9_plus_prog_bytes: int
    comp_ns_per_byte: float | None
    decomp_ns_per_byte: float | None
    mem_mb: float
    alg: str
    comp_pareto: bool
    decomp_pareto: bool
    mem_pareto: bool

    @property
    def family(self) -> str:
        return family_of(self.alg)

    @property
    def enwik9_bpb(self) -> float:
        return bits_per_byte(self.enwik9_bytes, ENWIK9_BYTES)

    @property
    def total_bpb(self) -> float:
        return bits_per_byte(self.enwik9_plus_prog_bytes, ENWIK9_BYTES)

    @property
    def enwik8_bpb(self) -> float | None:
        if self.enwik8_bytes is None:
            return None
        return bits_per_byte(self.enwik8_bytes, ENWIK8_BYTES)


LOCAL_COLUMNS = (
    "published_label", "program", "options", "enwik8_bytes", "enwik9_bytes",
    "decompressor_zip_bytes", "decompressor_flags", "comp_ns_per_byte", "decomp_ns_per_byte",
    "mem_mb", "alg", "timed_on", "source",
)


@dataclass(frozen=True)
class LocalEntry:
    """A locally measured codec drawn beside the published table, never merged into it."""
    label: str
    program: str
    enwik8_bytes: int | None
    enwik9_bytes: int | None
    decompressor_zip_bytes: int
    comp_ns_per_byte: float | None
    decomp_ns_per_byte: float | None
    mem_mb: float
    alg: str
    timed_on: str
    source: str

    @property
    def family(self) -> str:
        return family_of(self.alg)

    @property
    def enwik9_plus_prog_bytes(self) -> int | None:
        if self.enwik9_bytes is None:
            return None
        return self.enwik9_bytes + self.decompressor_zip_bytes

    @property
    def enwik9_bpb(self) -> float | None:
        if self.enwik9_bytes is None:
            return None
        return bits_per_byte(self.enwik9_bytes, ENWIK9_BYTES)

    @property
    def total_bpb(self) -> float | None:
        if self.enwik9_plus_prog_bytes is None:
            return None
        return bits_per_byte(self.enwik9_plus_prog_bytes, ENWIK9_BYTES)

    @property
    def enwik8_bpb(self) -> float | None:
        if self.enwik8_bytes is None:
            return None
        return bits_per_byte(self.enwik8_bytes, ENWIK8_BYTES)


def family_of(alg: str) -> str:
    """Fold the page's algorithm labels into six families; unknown labels are Other."""
    return _FAMILY_OF_ALG.get(alg, "Other")


def bits_per_byte(compressed: int, original: int) -> float:
    return 8.0 * compressed / original


def frontier(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Non-dominated (lower x, lower y) points, sorted by x: the speed/ratio frontier."""
    best = float("inf")
    kept = []
    for x, y in sorted(points):
        if y < best:
            kept.append((x, y))
            best = y
    return kept


def _int(row: dict, key: str, line: int, optional: bool = False) -> int | None:
    text = row[key].strip()
    if text == "" and optional:
        return None
    try:
        value = int(text)
    except ValueError as error:
        raise TableError(f"line {line}: {key} is not an integer: {text!r}") from error
    if value < 0:
        raise TableError(f"line {line}: {key} is negative")
    return value


def _float(row: dict, key: str, line: int, optional: bool = False) -> float | None:
    text = row[key].strip()
    if text == "" and optional:
        return None
    try:
        value = float(text)
    except ValueError as error:
        raise TableError(f"line {line}: {key} is not a number: {text!r}") from error
    if value < 0:
        raise TableError(f"line {line}: {key} is negative")
    return value


def load_rows(path: Path) -> list[Entry]:
    """Parse the transcribed main table into typed entries, rejecting anything malformed."""
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        missing = [c for c in REQUIRED_COLUMNS if c not in (reader.fieldnames or [])]
        if missing:
            raise TableError(f"{path}: missing columns {missing}")
        entries = []
        for line, row in enumerate(reader, start=2):
            rank = _int(row, "rank", line)
            if rank != len(entries) + 1:
                raise TableError(f"line {line}: rank {rank} is out of order")
            enwik9 = _int(row, "enwik9_bytes", line)
            total = _int(row, "enwik9_plus_prog_bytes", line)
            if enwik9 == 0 or total == 0:
                raise TableError(f"line {line}: zero compressed size")
            entries.append(Entry(
                rank=rank,
                label=row["published_label"].strip(),
                program=row["program"].strip(),
                enwik8_bytes=_int(row, "enwik8_bytes", line, optional=True),
                enwik9_bytes=enwik9,
                decompressor_zip_bytes=_int(row, "decompressor_zip_bytes", line),
                enwik9_plus_prog_bytes=total,
                comp_ns_per_byte=_float(row, "comp_ns_per_byte", line, optional=True),
                decomp_ns_per_byte=_float(row, "decomp_ns_per_byte", line, optional=True),
                mem_mb=_float(row, "mem_mb", line),
                alg=row["alg"].strip(),
                comp_pareto=row["comp_pareto"].strip() == "1",
                decomp_pareto=row["decomp_pareto"].strip() == "1",
                mem_pareto=row["mem_pareto"].strip() == "1",
            ))
    if not entries:
        raise TableError(f"{path}: no rows")
    return entries


def load_local(path: Path) -> list[LocalEntry]:
    """Parse locally measured entries; each needs a name and at least one compressed size."""
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        missing = [c for c in LOCAL_COLUMNS if c not in (reader.fieldnames or [])]
        if missing:
            raise TableError(f"{path}: missing columns {missing}")
        entries = []
        for line, row in enumerate(reader, start=2):
            entry = LocalEntry(
                label=row["published_label"].strip(),
                program=row["program"].strip(),
                enwik8_bytes=_int(row, "enwik8_bytes", line, optional=True),
                enwik9_bytes=_int(row, "enwik9_bytes", line, optional=True),
                decompressor_zip_bytes=_int(row, "decompressor_zip_bytes", line),
                comp_ns_per_byte=_float(row, "comp_ns_per_byte", line, optional=True),
                decomp_ns_per_byte=_float(row, "decomp_ns_per_byte", line, optional=True),
                mem_mb=_float(row, "mem_mb", line),
                alg=row["alg"].strip(),
                timed_on=row["timed_on"].strip(),
                source=row["source"].strip(),
            )
            if not entry.label or (entry.enwik8_bytes is None and entry.enwik9_bytes is None):
                raise TableError(f"line {line}: a local entry needs a label and a compressed size")
            if entry.enwik8_bytes == 0 or entry.enwik9_bytes == 0:
                raise TableError(f"line {line}: zero compressed size")
            entries.append(entry)
    return entries


def enwik8_rank(rows: list[Entry], enwik8_bytes: int) -> int:
    """Position a size would take in the published enwik8 column (ties rank after)."""
    return 1 + sum(1 for r in rows if r.enwik8_bytes is not None and r.enwik8_bytes < enwik8_bytes)


def total_rank(rows: list[Entry], total_bytes: int) -> int:
    """Position a total would take in the published enwik9 + decompressor ranking."""
    return 1 + sum(1 for r in rows if r.enwik9_plus_prog_bytes < total_bytes)


def _thousands(value: float, _position=None) -> str:
    return f"{value:g}" if value < 1 else f"{value:,.0f}"


def _style(plt) -> None:
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.size": 9,
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "axes.edgecolor": AXIS,
        "axes.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.titlecolor": INK,
        "axes.titlesize": 11.5,
        "axes.titleweight": "semibold",
        "axes.titlelocation": "left",
        "axes.labelcolor": INK2,
        "axes.labelsize": 9,
        "axes.grid": True,
        "grid.color": GRID,
        "grid.linewidth": 0.8,
        "grid.linestyle": "-",
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "xtick.labelsize": 8.5,
        "ytick.labelsize": 8.5,
        "legend.frameon": False,
        "legend.fontsize": 8.5,
        "legend.labelcolor": INK2,
        "svg.fonttype": "none",
        "svg.hashsalt": "ltcb-dashboard",  # stable element ids: same input, same SVG bytes
    })


def _subtitle(ax, text: str) -> None:
    ax.text(0, 1.02, text, transform=ax.transAxes, color=INK2, fontsize=8.5, va="bottom")


def _title(ax, text: str, subtitle: str) -> None:
    ax.set_title(text, pad=22)
    _subtitle(ax, subtitle)


def _dots(ax, xs, ys, color, size=34, alpha=1.0, zorder=3, label=None):
    return ax.scatter(xs, ys, s=size, c=color, alpha=alpha, linewidths=1.0,
                      edgecolors=SURFACE, zorder=zorder, label=label)


def _annotate(ax, x, y, text, dx=6, dy=4, ha="left"):
    ax.annotate(text, (x, y), xytext=(dx, dy), textcoords="offset points",
                fontsize=7.5, color=INK2, ha=ha, va="bottom", zorder=5)


def _local_mark(ax, x, y, text, dx=7, dy=-4, ha="left", provisional=False):
    """The locally measured codec: an ink diamond with a surface ring, named beside it.

    A provisional mark (hollow) stands in for a value not yet measured on enwik9."""
    if provisional:
        ax.scatter([x], [y], s=52, marker="D", facecolors=SURFACE, edgecolors=INK, linewidths=1.4,
                   zorder=6)
    else:
        ax.scatter([x], [y], s=52, marker="D", c=INK, linewidths=1.2, edgecolors=SURFACE, zorder=6)
    if text:
        ax.annotate(text, (x, y), xytext=(dx, dy), textcoords="offset points", fontsize=8,
                    fontweight="semibold", color=INK, ha=ha, va="center", zorder=7)


def _ratio_for_enwik9_axis(entry: LocalEntry) -> tuple[float, bool] | None:
    """The ratio to draw on an enwik9 axis: the measured one, else the enwik8 one flagged."""
    if entry.enwik9_bpb is not None:
        return entry.enwik9_bpb, False
    if entry.enwik8_bpb is not None:
        return entry.enwik8_bpb, True
    return None


def _local_caption(entry: LocalEntry, provisional: bool) -> str:
    return f"{entry.program} (local{', enwik8 ratio' if provisional else ''})"


def _spread_labels(ax, entries, texts, x_text, y_top, y_step) -> None:
    """Label clustered points from a column of text joined by hairline leaders."""
    for index, (entry, text) in enumerate(zip(entries, texts)):
        ax.annotate(text, entry, xytext=(x_text, y_top - index * y_step), textcoords="data",
                    fontsize=7.5, color=INK2, ha="left", va="center", zorder=5,
                    arrowprops={"arrowstyle": "-", "color": AXIS, "linewidth": 0.8,
                                "shrinkA": 0, "shrinkB": 3})


def _kpi_row(ax, rows: list[Entry], source: str, local: list[LocalEntry]) -> None:
    ax.axis("off")
    best_total = min(rows, key=lambda r: r.enwik9_plus_prog_bytes)
    with_e8 = [r for r in rows if r.enwik8_bytes is not None]
    best_e8 = min(with_e8, key=lambda r: r.enwik8_bytes)
    timed = [r for r in rows if r.comp_ns_per_byte is not None]
    fastest = min(timed, key=lambda r: r.comp_ns_per_byte)
    slowest = max(timed, key=lambda r: r.comp_ns_per_byte)
    leanest = min(rows, key=lambda r: r.mem_mb)
    hungriest = max(rows, key=lambda r: r.mem_mb)
    tiles = [
        ("Entries in the table", f"{len(rows)}",
         f"{len(with_e8)} with enwik8 · {len(timed)} timed"),
        ("Best total (enwik9 + decompressor)", f"{best_total.enwik9_plus_prog_bytes:,} B",
         f"{best_total.label} · {best_total.total_bpb:.3f} bits/byte"),
        ("Best enwik8", f"{best_e8.enwik8_bytes:,} B",
         f"{best_e8.label} · {best_e8.enwik8_bpb:.3f} bits/byte"),
        ("Compression speed span",
         f"{fastest.comp_ns_per_byte:,.0f} → {slowest.comp_ns_per_byte:,.0f} ns/B",
         f"{fastest.program.split()[0]} → {slowest.program.split()[0]} · "
         f"{slowest.comp_ns_per_byte / fastest.comp_ns_per_byte:,.0f}×"),
        ("Memory span", f"{leanest.mem_mb:g} MB → {hungriest.mem_mb / 1000:,.0f} GB",
         f"{leanest.program.split()[0]} → {hungriest.program.split()[0]}"),
    ]
    for entry in local:
        if entry.enwik9_plus_prog_bytes is not None:
            rank = total_rank(rows, entry.enwik9_plus_prog_bytes)
            tiles.append((f"◆ {entry.label} (local)", f"{entry.enwik9_plus_prog_bytes:,} B total",
                          f"would rank #{rank} of {len(rows)} · {entry.total_bpb:.3f} bpb"))
        else:
            rank = enwik8_rank(rows, entry.enwik8_bytes)
            tiles.append((f"◆ {entry.label} (local)", f"{entry.enwik8_bytes:,} B enwik8",
                          f"would rank #{rank} of {len(with_e8)} · {entry.enwik8_bpb:.3f} bpb · "
                          f"enwik9 pending"))
    for index, (label, value, sub) in enumerate(tiles):
        x = index / len(tiles)
        ax.text(x, 0.92, label, color=INK2, fontsize=9, va="top", transform=ax.transAxes)
        ax.text(x, 0.58, value, color=INK, fontsize=17 if len(tiles) <= 5 else 15,
                fontweight="semibold", va="center", transform=ax.transAxes)
        ax.text(x, 0.2, sub, color=MUTED, fontsize=8.5, va="center", transform=ax.transAxes)
    ax.text(0, -0.12, f"Source: {source}. Published values only; times are ns per byte on "
            "assorted machines; memory is the tester's peak for compression.",
            color=MUTED, fontsize=8.5, va="top", transform=ax.transAxes)


def _speed_ratio_panels(fig, spec, rows: list[Entry], local: list[LocalEntry]) -> None:
    timed = [r for r in rows if r.comp_ns_per_byte is not None]
    front = frontier([(r.comp_ns_per_byte, r.enwik9_bpb) for r in timed])
    fx = [x for x, _ in front] + [max(r.comp_ns_per_byte for r in timed)]
    fy = [y for _, y in front] + [front[-1][1]]
    grid = spec.subgridspec(2, 3, hspace=0.45, wspace=0.12)
    axes = grid.subplots(sharex=True, sharey=True)
    for index, family in enumerate(FAMILIES):
        ax = axes[index // 3][index % 3]
        members = [r for r in timed if r.family == family]
        others = [r for r in timed if r.family != family]
        _dots(ax, [r.comp_ns_per_byte for r in others], [r.enwik9_bpb for r in others],
              CONTEXT, size=16, zorder=2)
        ax.step(fx, fy, where="post", color=INK2, linewidth=1.0, alpha=0.6, zorder=2)
        _dots(ax, [r.comp_ns_per_byte for r in members], [r.enwik9_bpb for r in members],
              BLUE, zorder=3)
        if members:
            best = min(members, key=lambda r: r.enwik9_bpb)
            fast = min(members, key=lambda r: r.comp_ns_per_byte)
            _annotate(ax, best.comp_ns_per_byte, best.enwik9_bpb, best.program, dy=-12)
            if fast is not best:
                _annotate(ax, fast.comp_ns_per_byte, fast.enwik9_bpb, fast.program, dy=5)
        for entry in local:
            placed = _ratio_for_enwik9_axis(entry)
            if placed is not None and entry.comp_ns_per_byte is not None:
                ratio, provisional = placed
                _local_mark(ax, entry.comp_ns_per_byte, ratio,
                            _local_caption(entry, provisional) if entry.family == family else "",
                            dy=8, provisional=provisional)
        ax.set_title(f"{FAMILY_LABELS[family]} · {len(members)} timed", pad=6, fontsize=10)
        ax.set_xscale("log")
        ax.set_xlim(1, 2e7)
        ax.set_ylim(0.5, 8.5)
        if index % 3 == 0:
            ax.set_ylabel("enwik9, bits per byte")
        if index // 3 == 1:
            ax.set_xlabel("compression, ns per byte (log)")
    head = axes[0][0]
    head.text(0, 1.32, "Ratio against compression speed, one algorithm family per panel",
              transform=head.transAxes, color=INK, fontsize=11.5, fontweight="semibold")
    head.text(0, 1.2, f"Grey: every other entry. Line: the speed/ratio frontier across all "
              f"{len(timed)} timed entries. Labels: the family's best ratio and fastest entry.",
              transform=head.transAxes, color=INK2, fontsize=8.5)


def _memory_panel(ax, rows: list[Entry], local: list[LocalEntry]) -> None:
    top = [r for r in rows if r.rank <= 20]
    rest = [r for r in rows if r.rank > 20]
    _dots(ax, [r.mem_mb for r in rest], [r.enwik9_bpb for r in rest], CONTEXT, size=16, zorder=2)
    _dots(ax, [r.mem_mb for r in top], [r.enwik9_bpb for r in top], BLUE, zorder=3)
    leaders = sorted(top, key=lambda r: r.rank)[:3]
    _spread_labels(ax, [(r.mem_mb, r.enwik9_bpb) for r in leaders],
                   [f"#{r.rank} {r.program}" for r in leaders], x_text=12000, y_top=4.4, y_step=0.55)
    hungriest = max(rows, key=lambda r: r.mem_mb)
    _annotate(ax, hungriest.mem_mb, hungriest.enwik9_bpb, hungriest.program, dx=-6, ha="right")
    for entry in local:
        placed = _ratio_for_enwik9_axis(entry)
        if placed is not None:
            ratio, provisional = placed
            _local_mark(ax, entry.mem_mb, ratio, _local_caption(entry, provisional), dy=9,
                        provisional=provisional)
    floor = min(r.mem_mb for r in top)
    _title(ax, "Ratio against memory",
           f"Blue: the top 20 by total size, all at {floor:,.0f} MB or more. Grey: the rest.")
    ax.set_xscale("log")
    ax.set_xlabel("compression memory, MB (log)")
    ax.set_ylabel("enwik9, bits per byte")
    ax.set_ylim(0.5, 8.5)
    ax.xaxis.set_major_formatter(_thousands_formatter())


def _thousands_formatter():
    from matplotlib.ticker import FuncFormatter

    return FuncFormatter(_thousands)


def _median_ratio(group: list[Entry]) -> float:
    return statistics.median(r.decomp_ns_per_byte / r.comp_ns_per_byte for r in group)


def _symmetry_panel(ax, rows: list[Entry], local: list[LocalEntry]) -> None:
    both = [r for r in rows if r.comp_ns_per_byte is not None and r.decomp_ns_per_byte is not None]
    sym = [r for r in both if r.family in SYMMETRIC_FAMILIES]
    lz = [r for r in both if r.family == "LZ"]
    other = [r for r in both if r.family not in SYMMETRIC_FAMILIES and r.family != "LZ"]
    lo, hi = 1, 2e7
    ax.plot([lo, hi], [lo, hi], color=INK2, linewidth=1.0, alpha=0.6, zorder=2)
    ax.text(hi * 0.6, hi * 0.6, "decode = encode", color=INK2, fontsize=7.5, ha="right",
            va="bottom", rotation=45, rotation_mode="anchor")
    _dots(ax, [r.comp_ns_per_byte for r in other], [r.decomp_ns_per_byte for r in other],
          CONTEXT, size=16, zorder=2, label=f"BWT and other · {len(other)}")
    _dots(ax, [r.comp_ns_per_byte for r in sym], [r.decomp_ns_per_byte for r in sym], BLUE,
          zorder=3, label=f"CM, neural, PPM · {len(sym)}")
    _dots(ax, [r.comp_ns_per_byte for r in lz], [r.decomp_ns_per_byte for r in lz], ORANGE,
          zorder=3, label=f"LZ / dictionary · {len(lz)}")
    for entry in local:
        if entry.comp_ns_per_byte is not None and entry.decomp_ns_per_byte is not None:
            _local_mark(ax, entry.comp_ns_per_byte, entry.decomp_ns_per_byte,
                        f"{entry.program} (local, {entry.timed_on})", dy=9)
    _title(ax, "Decompression against compression time",
           f"Median decode/encode: CM, neural, PPM {_median_ratio(sym):.2f}× · "
           f"LZ / dictionary {_median_ratio(lz):.3f}×")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(lo, hi)
    ax.set_ylim(lo, hi)
    ax.set_xlabel("compression, ns per byte (log)")
    ax.set_ylabel("decompression, ns per byte (log)")
    ax.legend(loc="upper left")


def _family_strip(ax, rows: list[Entry], local: list[LocalEntry]) -> None:
    groups = {f: [r.enwik9_bpb for r in rows if r.family == f] for f in FAMILIES}
    order = sorted(FAMILIES, key=lambda f: statistics.median(groups[f]))
    for position, family in enumerate(order):
        values = groups[family]
        # Deterministic spread inside the band, no randomness.
        offsets = [((i * 7) % 9 - 4) / 9 * 0.32 for i in range(len(values))]
        _dots(ax, values, [position + o for o in offsets], BLUE, size=26, alpha=0.7, zorder=3)
        median = statistics.median(values)
        ax.plot([median, median], [position - 0.34, position + 0.34], color=INK, linewidth=2,
                zorder=4)
        ax.text(median, position + 0.4, f"{median:.2f}", color=INK2, fontsize=7.5, ha="center",
                va="bottom", zorder=5)
    for entry in local:
        placed = _ratio_for_enwik9_axis(entry)
        if placed is not None:
            ratio, provisional = placed
            _local_mark(ax, ratio, order.index(entry.family) - 0.42,
                        _local_caption(entry, provisional), dy=-8, provisional=provisional)
    ax.set_yticks(range(len(order)))
    ax.set_yticklabels([f"{SHORT_LABELS[f]} · {len(groups[f])}" for f in order])
    ax.set_ylim(-0.6, len(order) - 0.4)
    ax.set_xlim(0.5, 8.5)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("enwik9, bits per byte")
    _title(ax, "Ratio by algorithm family", "Sorted by median (black tick, labelled).")


def _scaling_panel(ax, rows: list[Entry], local: list[LocalEntry]) -> None:
    both = [r for r in rows if r.enwik8_bytes is not None]
    ax.plot([0.5, 8.5], [0.5, 8.5], color=INK2, linewidth=1.0, alpha=0.6, zorder=2)
    ax.text(8.3, 8.3, "same ratio", color=INK2, fontsize=7.5, ha="right", va="bottom",
            rotation=45, rotation_mode="anchor")
    _dots(ax, [r.enwik8_bpb for r in both], [r.enwik9_bpb for r in both], BLUE, zorder=3)
    gaps = sorted(both, key=lambda r: r.enwik9_bpb / r.enwik8_bpb)[:3]
    _spread_labels(ax, [(r.enwik8_bpb, r.enwik9_bpb) for r in gaps],
                   [f"{r.program} {r.enwik9_bpb / r.enwik8_bpb:.2f}×" for r in gaps],
                   x_text=3.6, y_top=2.4, y_step=0.55)
    for entry in local:
        if entry.enwik8_bpb is not None and entry.enwik9_bpb is not None:
            _local_mark(ax, entry.enwik8_bpb, entry.enwik9_bpb, f"{entry.program} (local)", dy=9)
        elif entry.enwik8_bpb is not None:
            ax.axvline(entry.enwik8_bpb, color=INK, linewidth=1.0, zorder=4)
            ax.text(entry.enwik8_bpb + 0.08, 7.2, f"{entry.program} (local) enwik8 "
                    f"{entry.enwik8_bpb:.3f}, would rank #{enwik8_rank(rows, entry.enwik8_bytes)}",
                    color=INK, fontsize=8, fontweight="semibold", va="top", zorder=5)
    ratios = [r.enwik9_bpb / r.enwik8_bpb for r in both]
    _title(ax, "enwik9 against enwik8 ratio",
           f"{len(both)} publish both; median enwik9/enwik8 = {statistics.median(ratios):.2f}×. "
           f"Labels: the three widest gaps.")
    ax.set_xlim(0.5, 8.5)
    ax.set_ylim(0.5, 8.5)
    ax.set_aspect("equal")
    ax.set_xlabel("enwik8, bits per byte")
    ax.set_ylabel("enwik9, bits per byte")


def _top_bars(ax, rows: list[Entry], local: list[LocalEntry], count: int = 25) -> None:
    top = [r for r in rows if r.rank <= count]
    positions = list(range(len(top)))[::-1]
    extra = [e for e in local if e.enwik9_plus_prog_bytes is not None]
    for offset, entry in enumerate(extra, start=1):
        y = -1.0 - offset
        data_mb, prog_mb = entry.enwik9_bytes / 1e6, entry.decompressor_zip_bytes / 1e6
        ax.barh([y], [data_mb], height=0.62, color=BLUE, edgecolor=SURFACE, linewidth=1.0, zorder=3)
        ax.barh([y], [prog_mb], left=[data_mb], height=0.62, color=ORANGE, edgecolor=SURFACE,
                linewidth=1.0, zorder=3)
        _local_mark(ax, data_mb + prog_mb, y,
                    f"{data_mb + prog_mb:,.1f}  ·  {entry.label} (local), would rank "
                    f"#{total_rank(rows, entry.enwik9_plus_prog_bytes)}", dx=9, dy=0)
    pending = [e for e in local if e.enwik9_plus_prog_bytes is None]
    for offset, entry in enumerate(pending, start=len(extra) + 1):
        y = -1.0 - offset
        _local_mark(ax, 0, y, f"{entry.label} (local): enwik9 pending, so no total to rank; "
                    f"enwik8 {entry.enwik8_bytes:,} B", dx=9, dy=0, provisional=True)
    extra = extra + pending
    ticks = positions + [-1.0 - i for i in range(1, len(extra) + 1)]
    tick_labels = [f"#{r.rank}  {r.label}" for r in top] + ["◆ local" for _ in extra]
    data = [r.enwik9_bytes / 1e6 for r in top]
    prog = [r.decompressor_zip_bytes / 1e6 for r in top]
    ax.barh(positions, data, height=0.62, color=BLUE, edgecolor=SURFACE, linewidth=1.0,
            label="enwik9 compressed", zorder=3)
    ax.barh(positions, prog, left=data, height=0.62, color=ORANGE, edgecolor=SURFACE,
            linewidth=1.0, label="decompressor (zip)", zorder=3)
    for y, r in zip(positions, top):
        ax.text(r.enwik9_plus_prog_bytes / 1e6 + 1.2, y, f"{r.enwik9_plus_prog_bytes / 1e6:,.1f}",
                color=INK2, fontsize=8, va="center", zorder=4)
    ax.set_yticks(ticks)
    ax.set_yticklabels(tick_labels, fontsize=8.5)
    ax.set_ylim(-1.7 - len(extra), len(top) - 0.3)
    widest = max([r.enwik9_plus_prog_bytes for r in top]
                 + [e.enwik9_plus_prog_bytes for e in extra if e.enwik9_plus_prog_bytes])
    ax.set_xlim(0, widest / 1e6 * 1.08)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("MB (10⁶ bytes)")
    ax.tick_params(axis="y", length=0)
    biggest = max(top, key=lambda r: r.decompressor_zip_bytes)
    _title(ax, f"Top {count} by total size",
           f"The ranking metric is enwik9 plus the zipped decompressor; {biggest.program} ships "
           f"{biggest.decompressor_zip_bytes / 1e6:,.1f} MB of decompressor.")
    ax.legend(loc="upper right")


def _rank_curve(ax, rows: list[Entry], local: list[LocalEntry]) -> None:
    ranks = [r.rank for r in rows]
    ax.plot(ranks, [r.total_bpb for r in rows], color=BLUE, linewidth=2, zorder=3)
    top = rows[0]
    twentieth = rows[min(19, len(rows) - 1)]
    _annotate(ax, top.rank, top.total_bpb, f"#{top.rank} {top.program} {top.total_bpb:.3f}")
    _annotate(ax, twentieth.rank, twentieth.total_bpb,
              f"#{twentieth.rank} {twentieth.program} {twentieth.total_bpb:.3f}", dy=9)
    last = rows[-1]
    _annotate(ax, last.rank, last.total_bpb, f"#{last.rank} {last.program}", dx=-6, dy=-12,
              ha="right")
    for index, entry in enumerate(local):
        if entry.enwik9_plus_prog_bytes is not None:
            rank = total_rank(rows, entry.enwik9_plus_prog_bytes)
            _local_mark(ax, rank, entry.total_bpb, f"{entry.program} (local) would rank #{rank}",
                        dy=12)
        elif entry.enwik8_bytes is not None:
            rank = enwik8_rank(rows, entry.enwik8_bytes)
            _local_mark(ax, rank, entry.enwik8_bpb, f"{entry.program} (local): #{rank} on the "
                        f"enwik8 column at {entry.enwik8_bpb:.3f}; enwik9 rank pending",
                        dy=22, provisional=True)
    spread = twentieth.total_bpb / top.total_bpb
    _title(ax, "The whole table, by rank",
           f"Total (enwik9 + decompressor) in bits per byte. Rank 20 is {spread:.2f}× rank 1; "
           f"the tail runs to {last.total_bpb:.2f}.")
    ax.set_xlim(0, len(rows) + 1)
    ax.set_ylim(0, 8.5)
    ax.set_xlabel("rank")
    ax.set_ylabel("bits per byte")


def render(rows: list[Entry], output: Path, source: str = DEFAULT_SOURCE,
           local: list[LocalEntry] = ()) -> list[Path]:
    """Write the dashboard as SVG at `output` and as PNG beside it; return both paths."""
    local = list(local)
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    _style(plt)
    fig = plt.figure(figsize=(18, 27))
    outer = fig.add_gridspec(6, 1, height_ratios=[1.3, 5.6, 4.4, 4.4, 6.4, 3.2],
                             hspace=0.42, left=0.075, right=0.97, top=0.955, bottom=0.03)
    fig.text(0.075, 0.985, "Large Text Compression Benchmark, the main table at a glance",
             color=INK, fontsize=20, fontweight="semibold", va="top")
    fig.text(0.075, 0.968, "Every program the benchmark lists, on enwik9 (10⁹ bytes of Wikipedia) "
             "and enwik8 (its first 10⁸ bytes). Lower bits per byte is better.",
             color=INK2, fontsize=11, va="top")
    _kpi_row(fig.add_subplot(outer[0]), rows, source, local)
    _speed_ratio_panels(fig, outer[1], rows, local)
    middle = outer[2].subgridspec(1, 2, wspace=0.18)
    _memory_panel(fig.add_subplot(middle[0]), rows, local)
    _symmetry_panel(fig.add_subplot(middle[1]), rows, local)
    lower = outer[3].subgridspec(1, 2, wspace=0.18, width_ratios=[1.25, 1])
    _family_strip(fig.add_subplot(lower[0]), rows, local)
    _scaling_panel(fig.add_subplot(lower[1]), rows, local)
    bars = outer[4].subgridspec(1, 20)
    _top_bars(fig.add_subplot(bars[0, 3:]), rows, local)
    _rank_curve(fig.add_subplot(outer[5]), rows, local)
    svg = Path(output)
    png = svg.with_suffix(".png")
    fig.savefig(svg, format="svg", metadata={"Date": None})
    fig.savefig(png, format="png", dpi=100)
    plt.close(fig)
    return [svg, png]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, nargs="?", default=RESEARCH / "ltcb-main-table.csv")
    parser.add_argument("--output", type=Path, required=True, help="SVG path; a PNG is written beside it")
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="provenance line printed on the figure")
    parser.add_argument("--local", type=Path, help="CSV of locally measured entries to draw beside the table")
    args = parser.parse_args(argv)
    if args.output.suffix.lower() != ".svg":
        parser.error("--output must be an .svg path")
    try:
        rows = load_rows(args.table)
        local = load_local(args.local) if args.local else []
    except (OSError, TableError) as error:
        parser.error(str(error))
    for path in render(rows, args.output, source=args.source, local=local):
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
