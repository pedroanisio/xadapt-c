#!/usr/bin/env python3
"""Assemble the interactive LTCB explorer page from the CSVs and the analysis JSON.

Source: Claude Opus 5 via Claude Code. The page is ltcb-interactive.template.html
with the data embedded as JSON: the transcribed main table (ltcb-main-table.csv),
the local entries (ltcb-local-entries.csv), the frontier phases (ltcb-phases.csv,
from plot_ltcb_phases.py) and the statistics (ltcb-analysis.json, from
analyze_ltcb.py). Nothing is computed here beyond joining those files, so the
page shows exactly what the reports show.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import plot_ltcb_tables as ltcb

HERE = Path(__file__).resolve().parent
RESEARCH = ltcb.RESEARCH
PLACEHOLDER = "/*__DATA__*/"


def _num(text: str) -> float | None:
    text = text.strip()
    return None if text == "" else float(text)


def _int(text: str) -> int | None:
    text = text.strip()
    return None if text == "" else int(text)


def load_table_rows(path: Path) -> list[dict]:
    """Every published column, validated through load_rows first."""
    ltcb.load_rows(path)
    with open(path, newline="", encoding="utf-8") as handle:
        out = []
        for row in csv.DictReader(handle):
            out.append({
                "rank": int(row["rank"]), "label": row["published_label"], "program": row["program"],
                "options": row["options"], "e8": _int(row["enwik8_bytes"]), "e9": int(row["enwik9_bytes"]),
                "zip": int(row["decompressor_zip_bytes"]), "flags": row["decompressor_flags"],
                "total": int(row["enwik9_plus_prog_bytes"]), "comp": _num(row["comp_ns_per_byte"]),
                "decomp": _num(row["decomp_ns_per_byte"]), "mem": float(row["mem_mb"]), "alg": row["alg"],
                "family": ltcb.family_of(row["alg"]), "note": row["note"],
                "cp": row["comp_pareto"] == "1", "dp": row["decomp_pareto"] == "1", "mp": row["mem_pareto"] == "1",
            })
    return out


def load_local_rows(path: Path | None) -> list[dict]:
    if path is None:
        return []
    return [{"label": e.label, "program": e.program, "e8": e.enwik8_bytes, "e9": e.enwik9_bytes,
             "zip": e.decompressor_zip_bytes, "comp": e.comp_ns_per_byte, "decomp": e.decomp_ns_per_byte,
             "mem": e.mem_mb, "alg": e.alg, "family": e.family, "timed_on": e.timed_on, "source": e.source}
            for e in ltcb.load_local(path)]


def load_phases(path: Path) -> dict[str, list[dict]]:
    phases: dict[str, list[dict]] = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            phases.setdefault(row["axis"], []).append({
                "phase": int(row["phase"]), "family": row["family"], "program": row["program"],
                "speed": float(row["speed_ns_per_byte"]), "bpb": float(row["enwik9_bpb"]),
                "phase_start": float(row["phase_start_ns_per_byte"]), "phase_end": float(row["phase_end_ns_per_byte"]),
                "phase_gain_per_decade": _num(row["phase_gain_per_decade"]),
                "step_gain_per_decade": _num(row["step_gain_per_decade"]),
                "transition_bpb_drop": _num(row["transition_bpb_drop"]),
                "transition_time_ratio": _num(row["transition_time_ratio"]),
            })
    if not phases:
        raise ltcb.TableError(f"{path}: no phases")
    return phases


def build(table: Path, local: Path | None, phases: Path, analysis: Path, template: Path,
          source: str, fetched: str) -> str:
    data = {
        "meta": {"source": source, "fetched": fetched, "families": dict(ltcb.FAMILY_LABELS)},
        "rows": load_table_rows(table),
        "local": load_local_rows(local),
        "phases": load_phases(phases),
        "analysis": json.loads(analysis.read_text()),
    }
    text = template.read_text()
    if text.count(PLACEHOLDER) != 1:
        raise ltcb.TableError(f"{template}: expected exactly one {PLACEHOLDER}")
    # JSON inside a <script> block must never contain the closing tag.
    payload = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    return text.replace(PLACEHOLDER, payload)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", type=Path, default=RESEARCH / "ltcb-main-table.csv")
    parser.add_argument("--local", type=Path, default=RESEARCH / "ltcb-local-entries.csv")
    parser.add_argument("--phases", type=Path, default=RESEARCH / "ltcb-phases.csv")
    parser.add_argument("--analysis", type=Path, default=RESEARCH / "ltcb-analysis.json")
    parser.add_argument("--template", type=Path, default=HERE / "ltcb-interactive.template.html")
    parser.add_argument("--output", type=Path, default=RESEARCH / "ltcb-interactive.html")
    parser.add_argument("--source", default=ltcb.DEFAULT_SOURCE)
    parser.add_argument("--fetched", default="2026-09-14 20:04 UTC")
    args = parser.parse_args(argv)
    try:
        html = build(args.table, args.local if args.local.exists() else None, args.phases,
                     args.analysis, args.template, args.source, args.fetched)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_text(html)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
