#!/usr/bin/env python3
"""Enforce branch coverage of the C codec from gcov's JSON.

The library — every adapt_* unit and header with code (adapt_decode.c,
adapt_encode.c, adapt_archive.c, adapt_adaptive.h, adapt_bmp.h, adapt_text.h
and the ten sections of adapt_model.h, as each unit compiles them) — must
reach 80 % of its branches together, the command line (every cadapt* unit)
60 %: the crate's floors, applied to its second implementation. The groups
are by name, never a list, so a new unit counts the day it exists. A source
missing from the report fails the gate, so a file that stopped being
instrumented cannot pass by absence. A header is inlined into both library
units and counted under each, since each compiles its own copy.

    python3 check-gcov.py <unit.gcov.json>...

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
"""

import json
import re
import sys
from pathlib import Path

CODEC = Path(__file__).resolve().parent
FLOORS = {"library": 80.0, "cli": 60.0}
# a function body in a header: what makes gcov report the header at all
BODY = re.compile(r"^static\b[^;]*?\)\s*\{", re.M | re.S)


def group_of(name: str) -> str | None:
    """The group a source counts toward, by its name: adapt_* is the library
    (the units, adapt_adaptive.h, adapt_bmp.h, adapt_text.h and the sections
    of adapt_model.h), cadapt* the command line; anything else — a system
    header, adapt.h, a test — is not counted. No list to keep."""
    if re.fullmatch(r"cadapt(_\w+)?\.[ch]", name):
        return "cli"
    if re.fullmatch(r"adapt_\w+\.[ch]", name):
        return "library"
    return None


def expected_sources(codec: Path = CODEC) -> set[str]:
    """Every source the reports must hold: each unit of the tree, and each
    header of a group that defines a function (gcov reports those, where
    the code is; a header of declarations alone never appears). A file that
    stopped being instrumented cannot pass by absence."""
    out = set()
    for path in codec.glob("*.[ch]"):
        if group_of(path.name) is None:
            continue
        if path.suffix == ".c" or BODY.search(path.read_text(encoding="utf-8")):
            out.add(path.name)
    return out


def branches(path: str) -> dict[str, tuple[int, int]]:
    """Covered and total branch counts per source file in one gcov report.

    A report is one compilation unit; a header it inlines appears as its
    own file entry there. Counts are keyed by unit and file, so the same
    header compiled into two units is two pieces of evidence.
    """
    seen: dict[str, tuple[int, int]] = {}
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    if report.get("format_version") != "1":
        raise ValueError(f"unexpected gcov format version {report.get('format_version')!r}")
    if not isinstance(report["files"], list) or not report["files"]:
        raise ValueError("files must be a nonempty list")
    for file in report["files"]:
        name = Path(file["file"]).name
        if group_of(name) is None:
            continue
        if name in seen:
            raise ValueError(f"duplicate source evidence: {name}")
        covered = count = 0
        for line in file["lines"]:
            for branch in line.get("branches", []):
                if type(branch["count"]) is not int or branch["count"] < 0:
                    raise ValueError(f"invalid branch count in {name}")
                count += 1
                covered += branch["count"] > 0
        seen[name] = (covered, count)
    return seen


def main(paths: list[str]) -> int:
    """Enforce the floors over the gcov reports at ``paths``.

    Prints each group's branch coverage; the exit status is 0 only when every
    floor is met and every report is readable.
    """
    seen: dict[str, tuple[int, int]] = {}
    try:
        for path in paths:
            for name, (covered, count) in branches(path).items():
                key = f"{Path(path).name}:{name}" if name.endswith(".h") else name
                if key in seen:
                    raise ValueError(f"duplicate source evidence: {name}")
                seen[key] = (covered, count)
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"invalid coverage report: {error}", file=sys.stderr)
        return 1
    sources = {key.rsplit(":", 1)[-1] for key in seen}
    missing = expected_sources() - sources
    if missing:
        print(f"coverage report lacks {sorted(missing)}", file=sys.stderr)
        return 1

    def rate(label: str) -> tuple[float, int, int]:
        covered = sum(c for key, (c, _) in seen.items() if group_of(key.rsplit(":", 1)[-1]) == label)
        count = sum(n for key, (_, n) in seen.items() if group_of(key.rsplit(":", 1)[-1]) == label)
        return (100.0 * covered / count if count else 0.0, covered, count)

    ok = True
    for label in ("library", "cli"):
        pct, covered, count = rate(label)
        floor = FLOORS[label]
        verdict = "ok " if pct >= floor else "LOW"
        ok &= pct >= floor
        print(f"{verdict} {label:8s} branches {covered}/{count} = {pct:.1f}% (floor {floor:.0f}%)")
    for key in sorted(seen):
        c, n = seen[key]
        percent = f"{100.0 * c / n:5.1f}%" if n else "   n/a"
        print(f"    {key:36s} {c:>4}/{n:<4} {percent}")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
