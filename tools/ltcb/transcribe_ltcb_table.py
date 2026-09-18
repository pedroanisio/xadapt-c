#!/usr/bin/env python3
"""Transcribe the Large Text Compression Benchmark's main table into CSV.

Source: Claude Opus 5 via Claude Code, 2026-09-15. Parsing rules are documented
in research/ltcb-tables.md. The source table is the
`<pre><ol>` block of mattmahoney.net/dc/text.html, one `<li>` per row;
every row is matched against one pattern anchored on the right-hand numeric
columns (a row that does not match is an error, never skipped); a single
time value is Comp or Decomp by the column it starts in; `~` marks a very
approximate value; `<u>` marks a Pareto value; the program is the text
before the first run of two or more spaces, else before the first `-`-led
token, else the whole label.

    python3 tools/ltcb/transcribe_ltcb_table.py --html page.html --output research/ltcb-main-table.csv

The page is fetched by the caller (curl), never here: the transcription is
deterministic for a saved page, whose date and SHA-256 the tool prints.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import re
import sys
from dataclasses import dataclass
from pathlib import Path

COLUMNS = ["rank", "published_label", "program", "options", "enwik8_bytes", "enwik9_bytes",
           "decompressor_zip_bytes", "decompressor_flags", "enwik9_plus_prog_bytes", "comp_ns_per_byte",
           "comp_approximate", "decomp_ns_per_byte", "decomp_approximate", "mem_mb", "mem_approximate", "alg",
           "note", "comp_pareto", "decomp_pareto", "mem_pareto"]

# the `<u>` markers survive the tag stripping as these two characters
U_OPEN, U_CLOSE = "\x01", "\x02"
SIZE = r"\d{1,3}(?:,\d{3})+"  # every published size has thousands separators
ROW = re.compile(
    rf"^(?P<label>\S.*?)\s+(?:(?P<enwik8>{SIZE})\s+)?(?P<enwik9>{SIZE})\s+(?P<zip>\d{{1,3}}(?:,\d{{3}})*)\s+"
    rf"(?:(?P<flags>[a-z]+)\s+)?(?P<total>{SIZE})(?P<tail>(?:\s+\S+)*)\s*$"
)
NUMBER = re.compile(r"^~?[0-9]+(?:\.[0-9]+)?$")
DATE = re.compile(r"Last update(?:d)?:?\s*([A-Za-z]+\.?\s+\d{1,2},?\s+\d{4})", re.I)


class TranscriptionError(ValueError):
    """A page or a row the rules do not cover; nothing is written."""


@dataclass(frozen=True)
class Column:
    """A table column's start offset in the tag-stripped header line."""
    comp: int
    decomp: int


def table_block(page: str) -> tuple[str, list[str]]:
    """The header line and the row lines of the main table, tags stripped
    except the underline markers, entities decoded."""
    start = page.find("<pre><ol>")
    end = page.find("</ol></pre>", start)
    if start < 0 or end < 0:
        raise TranscriptionError("the page has no <pre><ol> ... </ol></pre> table block")
    block = page[start:end]
    block = block.replace("<u>", U_OPEN).replace("</u>", U_CLOSE)
    lines = [html.unescape(re.sub(r"<(?!/?li)[^>]*>", "", line)) for line in block.split("\n")]
    header = next((line for line in lines if line.startswith("Program") and "Decomp" in line), None)
    if header is None:
        raise TranscriptionError("the table block has no header line naming Program and Decomp")
    rows = [line[len("<li>"):] for line in lines if line.startswith("<li>")]
    return header, rows


def columns(header: str) -> Column:
    return Column(comp=header.index("Comp"), decomp=header.index("Decomp"))


def split_label(label: str) -> tuple[str, str]:
    """program and options: at the first run of two or more spaces, else
    before the first `-`-led token, else the whole label."""
    parts = re.split(r" {2,}", label.strip(), maxsplit=1)
    if len(parts) == 2:
        return parts[0], parts[1]
    tokens = label.split()
    for i, token in enumerate(tokens):
        if i > 0 and token.startswith("-"):
            return " ".join(tokens[:i]), " ".join(tokens[i:])
    return label.strip(), ""


def _number(token: str) -> tuple[str, str, str]:
    """(value, approximate, pareto) of a time or memory token."""
    pareto = "1" if U_OPEN in token else "0"
    token = token.replace(U_OPEN, "").replace(U_CLOSE, "")
    approximate = "1" if token.startswith("~") else "0"
    return token.lstrip("~"), approximate, pareto


def transcribe_row(rank: int, raw: str, col: Column) -> dict[str, str]:
    line = raw.rstrip()
    match = ROW.match(line.replace(U_OPEN, "").replace(U_CLOSE, ""))
    if match is None:
        raise TranscriptionError(f"row {rank} does not match the table pattern: {line.strip()!r}")
    # the tail keeps its markers: the marked and the stripped lines split into
    # the same tokens, so the tail is the same number of tokens from the end;
    # two figures the page runs together ("108960~110000") are split apart
    marked = line.split()
    tokens = [part for token in marked[len(marked) - len(match["tail"].split()):]
              for part in re.split(r"(?<=\d)(?=~)", token)]
    numbers, rest = [], []
    for token in tokens:
        if NUMBER.match(token.replace(U_OPEN, "").replace(U_CLOSE, "")) and not rest:
            numbers.append(token)
        else:
            rest.append(token)
    if not numbers:
        raise TranscriptionError(f"row {rank} has no memory figure: {line.strip()!r}")
    if len(rest) > 2 or (len(rest) == 2 and not rest[1].isdigit()):
        raise TranscriptionError(f"row {rank} has more than an algorithm and a note after the memory: {line.strip()!r}")
    alg = rest[0] if rest else ""
    note = rest[1] if len(rest) == 2 else ""
    mem, mem_approximate, mem_pareto = _number(numbers[-1])
    times = numbers[:-1]
    if len(times) > 2:
        raise TranscriptionError(f"row {rank} has {len(times)} time values: {line.strip()!r}")
    comp = decomp = ("", "0", "0")
    if len(times) == 2:
        comp, decomp = _number(times[0]), _number(times[1])
    elif len(times) == 1:
        # one published time: Comp when it starts left of the Decomp header, else Decomp
        stripped = line.replace(U_OPEN, "").replace(U_CLOSE, "")
        value = times[0].replace(U_OPEN, "").replace(U_CLOSE, "")
        start = stripped.index(value, match.start("tail"))  # the tail's first token, so the first occurrence
        if start < col.decomp:
            comp = _number(times[0])
        else:
            decomp = _number(times[0])
    label = " ".join(match["label"].split())
    program, options = split_label(match["label"])
    if (program + " " + options).strip() != label:
        raise TranscriptionError(f"row {rank}: the program/options split loses text: {label!r}")
    return {
        "rank": str(rank),
        "published_label": label,
        "program": program,
        "options": options,
        "enwik8_bytes": (match["enwik8"] or "").replace(",", ""),
        "enwik9_bytes": match["enwik9"].replace(",", ""),
        "decompressor_zip_bytes": match["zip"].replace(",", ""),
        "decompressor_flags": match["flags"] or "",
        "enwik9_plus_prog_bytes": match["total"].replace(",", ""),
        "comp_ns_per_byte": comp[0],
        "comp_approximate": comp[1],
        "decomp_ns_per_byte": decomp[0],
        "decomp_approximate": decomp[1],
        "mem_mb": mem,
        "mem_approximate": mem_approximate,
        "alg": alg,
        "note": note,
        "comp_pareto": comp[2],
        "decomp_pareto": decomp[2],
        "mem_pareto": mem_pareto,
    }


def transcribe(page: str) -> list[dict[str, str]]:
    header, rows = table_block(page)
    col = columns(header)
    return [transcribe_row(rank, raw, col) for rank, raw in enumerate(rows, start=1)]


def page_date(page: str) -> str:
    match = DATE.search(page)
    return match.group(1) if match else ""


def write_csv(rows: list[dict[str, str]], path: Path) -> None:
    with open(path, "w", newline="", encoding="utf-8") as out:
        writer = csv.DictWriter(out, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--html", type=Path, required=True, help="the saved text.html")
    parser.add_argument("--output", type=Path, required=True, help="the CSV to write")
    args = parser.parse_args(argv)
    try:
        data = args.html.read_bytes()
        rows = transcribe(data.decode("utf-8", errors="replace"))
    except (OSError, TranscriptionError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    write_csv(rows, args.output)
    date = page_date(data.decode("utf-8", errors="replace"))
    print(f"{len(rows)} rows from {args.html} (page dated {date or 'unknown'}, sha256 {hashlib.sha256(data).hexdigest()}) "
          f"-> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
