#!/usr/bin/env python3
"""Ranking comparisons computed from the Large Text Compression Benchmark snapshot.

Computes a ranking threshold and memory comparison from the published snapshot.
The selected place is a query parameter, not an encoder acceptance policy.

    python3 tools/ltcb/ltcb_goal.py            # the figures for cadapt 0.12.0, place 20

The threshold is the total (enwik9 plus the decompressor zip, the ranked
quantity) of the entry holding the place; the enwik9 target is that total
less the entry's own decompressor less one byte, since a tie does not
overtake. The memory figure names the least memory of every entry ranked
above; it is a comparison statistic, not a release requirement.
"""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "research" / "ltcb-main-table.csv"
PROGRAM = "cadapt 0.12.0"
PLACE = 20


class GoalError(ValueError):
    """The snapshot lacks what the goal is computed from."""


@dataclass(frozen=True)
class Goal:
    program: str
    rank: int
    entries: int
    total: int
    decompressor_bytes: int
    memory_mb: float
    place: int
    threshold_program: str
    threshold_total: int
    target_enwik9: int
    least_memory_above: tuple[float, str] | None

    @property
    def leads_memory_above(self) -> bool:
        return self.least_memory_above is None or self.memory_mb < self.least_memory_above[0]


def _rows(table: Path) -> list[dict[str, str]]:
    try:
        with open(table, newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except OSError as error:
        raise GoalError(f"{table}: {error}") from error
    if not rows:
        raise GoalError(f"{table}: no rows")
    return rows


def compute(table: Path = TABLE, program: str = PROGRAM, place: int = PLACE) -> Goal:
    rows = _rows(table)
    ranked = sorted(rows, key=lambda r: int(r["rank"]))
    own = [r for r in ranked if r["program"] == program]
    if len(own) != 1:
        raise GoalError(f"{table}: {len(own)} rows name the program {program!r}, expected one")
    if not 1 <= place <= len(ranked):
        raise GoalError(f"{table}: no place {place} in a table of {len(ranked)} entries")
    me = own[0]
    holder = ranked[place - 1]
    above = [r for r in ranked if int(r["rank"]) < int(me["rank"]) and r["mem_mb"]]
    least = min(((float(r["mem_mb"]), r["program"]) for r in above), default=None)
    return Goal(
        program=program,
        rank=int(me["rank"]),
        entries=len(ranked),
        total=int(me["enwik9_plus_prog_bytes"]),
        decompressor_bytes=int(me["decompressor_zip_bytes"]),
        memory_mb=float(me["mem_mb"]),
        place=place,
        threshold_program=holder["program"],
        threshold_total=int(holder["enwik9_plus_prog_bytes"]),
        target_enwik9=int(holder["enwik9_plus_prog_bytes"]) - int(me["decompressor_zip_bytes"]) - 1,
        least_memory_above=least,
    )


def _mb(value: float) -> str:
    return f"{value:,.0f} MB" if value == int(value) else f"{value:,} MB"


def report(g: Goal) -> str:
    lines = [
        f"{g.program}: rank {g.rank} of {g.entries}, total {g.total:,} B "
        f"(decompressor {g.decompressor_bytes:,} B), {_mb(g.memory_mb)}",
        f"place {g.place} is {g.threshold_program} at {g.threshold_total:,} B total",
        f"enwik9 target for place {g.place}: at most {g.target_enwik9:,} B "
        f"(that total less the decompressor less one byte)",
    ]
    if g.least_memory_above is None:
        lines.append("memory: nothing is ranked above")
    else:
        mem, who = g.least_memory_above
        verdict = "the least of every entry ranked above" if g.leads_memory_above else "NOT the least of the entries ranked above"
        lines.append(f"memory: {_mb(g.memory_mb)} is {verdict} (the least above: {who} at {_mb(mem)})")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None, out=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--table", type=Path, default=TABLE)
    parser.add_argument("--program", default=PROGRAM)
    parser.add_argument("--place", type=int, default=PLACE)
    args = parser.parse_args(argv)
    try:
        g = compute(args.table, args.program, args.place)
    except GoalError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    (out or sys.stdout).write(report(g))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
