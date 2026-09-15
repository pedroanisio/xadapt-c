#!/usr/bin/env python3
"""Enforce branch coverage of the C codec from gcov's JSON.

The library — adapt_decode.c, adapt_encode.c, adapt_archive.c, and
adapt_model.h, adapt_adaptive.h, adapt_bmp.h and adapt_text.h as each of
the first two compiles them — must reach 80 % of its
branches together, the command line (cadapt.c) 60 %: the crate's floors, applied to its second implementation.
A source missing from the report fails the gate, so a file that stopped
being instrumented cannot pass by absence. The header is inlined into both
library units and counted under each, since each compiles its own copy.

    python3 check-gcov.py <unit.gcov.json>...

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
"""

import json
import sys
from pathlib import Path

LIBRARY = {"adapt_decode.c", "adapt_encode.c", "adapt_archive.c", "adapt_model.h", "adapt_adaptive.h", "adapt_bmp.h",
           "adapt_text.h"}
CLI = {"cadapt.c"}
FLOORS = {"library": 80.0, "cli": 60.0}


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
        if name not in LIBRARY | CLI:
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
    missing = (LIBRARY | CLI) - sources
    if missing:
        print(f"coverage report lacks {sorted(missing)}", file=sys.stderr)
        return 1

    def rate(names: set[str]) -> tuple[float, int, int]:
        covered = sum(c for key, (c, _) in seen.items() if key.rsplit(":", 1)[-1] in names)
        count = sum(n for key, (_, n) in seen.items() if key.rsplit(":", 1)[-1] in names)
        return (100.0 * covered / count if count else 0.0, covered, count)

    ok = True
    for label, names in (("library", LIBRARY), ("cli", CLI)):
        pct, covered, count = rate(names)
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
