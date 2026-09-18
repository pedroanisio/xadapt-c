#!/usr/bin/env python3
"""Render the locked floors of tools/ratio_floor.json for the documents.

Source: Claude Opus 5 via Claude Code, 2026-09-15. The floors are the only
numbers a run has verified (LOCKS.md); README.md, cadapt.1 and the release
notes of the version cadapt.c names (docs/release-notes/<version>.md, when
they exist) carry them in a marked block that this tool writes, so the prose
never restates a figure the next re-lock would leave behind. An earlier
release's notes keep the floors of their day.

    python3 tools/render_floors.py markdown     # the README block's body
    python3 tools/render_floors.py troff        # the manual page block's body
    python3 tools/render_floors.py update       # rewrite the blocks in place
    python3 tools/render_floors.py check        # exit 1 when a block is stale

The README's and the notes' block sits between `<!-- floors:start -->` and
`<!-- floors:end -->`, the manual's between `.\" floors:start` and
`.\" floors:end`; the lines between them belong to this tool. Notes of the
current version without the block are an error: the floors they state
would be typed.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLOOR = ROOT / "tools" / "ratio_floor.json"
README = ROOT / "README.md"
MANUAL = ROOT / "adpat" / "c" / "cadapt.1"
SOURCE = ROOT / "adpat" / "c" / "cadapt.c"
NOTES = ROOT / "docs" / "release-notes"
MARKDOWN_START, MARKDOWN_END = "<!-- floors:start -->", "<!-- floors:end -->"
TROFF_START, TROFF_END = '.\\" floors:start', '.\\" floors:end'


class RenderError(Exception):
    """A floor file or a document that cannot be rendered or found."""


def load() -> dict:
    try:
        data = json.loads(FLOOR.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RenderError(f"{FLOOR}: {error}") from error
    if not isinstance(data, dict) or not isinstance(data.get("floors"), list) or not data["floors"]:
        raise RenderError(f"{FLOOR}: no floors")
    for floor in data["floors"]:
        for key in ("fixture", "input_bytes", "max_container_bytes"):
            if key not in floor:
                raise RenderError(f"{FLOOR}: floor without {key!r}")
        if len(set(floor) & set(MEASURES)) != 1:
            raise RenderError(f"{FLOOR}: floor without exactly one of {sorted(MEASURES)}")
    return data


# a floor's seconds are in one of these measures; the block names which
MEASURES = {"cpu_seconds": "CPU s", "wall_seconds": "wall s"}


def time_floor(floor: dict) -> str:
    """The floor's seconds with the measure they were taken in."""
    (key,) = set(floor) & set(MEASURES)
    return f"{floor[key]:,.1f} {MEASURES[key]}"


def markdown(data: dict) -> str:
    args = " ".join(data["arguments"])
    lines = [
        f"Locked {data['locked_on']} on {data['host']}, `cadapt {args}`; a change may only",
        "improve on these (`make gate` checks the first, `make ratio-floor` the rest):",
        "",
        "| Fixture | Input | Container floor | Time floor |",
        "|---|---:|---:|---:|",
    ]
    for floor in data["floors"]:
        lines.append(
            f"| {floor['fixture']} | {floor['input_bytes']:,} B | {floor['max_container_bytes']:,} B | "
            f"{time_floor(floor)} |"
        )
    return "\n".join(lines) + "\n"


def troff(data: dict) -> str:
    args = " ".join(data["arguments"])
    lines = [
        f"Locked {data['locked_on']} on {data['host']},",
        f".BR \"cadapt {args}\" ;",
        "a change may only improve on these:",
        ".TS",
        "l r r r.",
        "fixture\tinput\tcontainer floor\ttime floor",
        "_",
    ]
    for floor in data["floors"]:
        lines.append(
            f"{floor['fixture']}\t{floor['input_bytes']:,}\t{floor['max_container_bytes']:,}\t{time_floor(floor)}"
        )
    lines.append(".TE")
    return "\n".join(lines) + "\n"


def replace_block(text: str, start: str, end: str, body: str, path: Path) -> str:
    """The text with the lines between the markers replaced by `body`."""
    lines = text.split("\n")
    try:
        first = lines.index(start)
        last = lines.index(end, first + 1)
    except ValueError as error:
        raise RenderError(f"{path}: markers {start!r} and {end!r} not found in order") from error
    return "\n".join(lines[: first + 1] + body.rstrip("\n").split("\n") + lines[last:])


def release_notes() -> Path | None:
    """The notes of the version the source names, when they exist: the one
    document besides the README and the manual that states the floors. An
    earlier release's notes keep the floors of their day and are not touched."""
    try:
        source = SOURCE.read_text(encoding="utf-8")
    except OSError as error:
        raise RenderError(f"{SOURCE}: {error}") from error
    match = re.search(r'^#define CADAPT_VERSION "([^"]+)"', source, flags=re.M)
    if match is None:
        raise RenderError(f"{SOURCE}: no CADAPT_VERSION")
    path = NOTES / f"{match.group(1)}.md"
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8")
    if MARKDOWN_START not in text:
        raise RenderError(f"{path.relative_to(ROOT)}: the release notes state the floors without the "
                          f"{MARKDOWN_START} block that renders them")
    return path


def render_documents(data: dict) -> dict[Path, str]:
    """Each document's text with its block brought up to date."""
    out = {}
    documents = [(README, MARKDOWN_START, MARKDOWN_END, markdown(data)), (MANUAL, TROFF_START, TROFF_END, troff(data))]
    notes = release_notes()
    if notes is not None:
        documents.append((notes, MARKDOWN_START, MARKDOWN_END, markdown(data)))
    for path, start, end, body in documents:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise RenderError(f"{path}: {error}") from error
        out[path] = replace_block(text, start, end, body, path)
    return out


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1 or argv[0] not in ("markdown", "troff", "update", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        data = load()
        if argv[0] == "markdown":
            sys.stdout.write(markdown(data))
        elif argv[0] == "troff":
            sys.stdout.write(troff(data))
        else:
            stale = []
            for path, text in render_documents(data).items():
                if text != path.read_text(encoding="utf-8"):
                    stale.append(path)
                    if argv[0] == "update":
                        path.write_text(text, encoding="utf-8")
            if argv[0] == "check" and stale:
                for path in stale:
                    print(f"STALE {path.relative_to(ROOT)}: run python3 tools/render_floors.py update", file=sys.stderr)
                return 1
            for path in stale:
                print(f"updated {path.relative_to(ROOT)}")
    except RenderError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
