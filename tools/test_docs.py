#!/usr/bin/env python3
"""The documents against the code they describe.

Source: Claude Opus 5 via Claude Code, 2026-09-15 (the doc-hygiene audit,
docs/doc-hygiene-report-2026-09-15.md). Each test pins one thing a document
hand-copies from a source of truth: the README's usage block against
`cadapt --help`, the flags of the manual and the README against the binary,
the container versions of adapt.h against the README and the manual, the
floors block against tools/ratio_floor.json (render_floors.py), the README's
layout table against the tracked tree, and every relative link against the
tree. A failure names what moved.

    python3 -B -m unittest discover -s tools -p 'test_docs.py'

The binary is the one `make` builds (adpat/c/build/cadapt) unless CADAPT
names another; `make check` builds before it runs this.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "ltcb"))
import ltcb_goal  # noqa: E402
import render_floors as floors  # noqa: E402

README = ROOT / "README.md"
MANUAL = ROOT / "adpat" / "c" / "cadapt.1"
HEADER = ROOT / "adpat" / "c" / "adapt.h"
CADAPT = Path(os.environ.get("CADAPT", ROOT / "adpat" / "c" / "build" / "cadapt"))
FLAG = re.compile(r"--[a-z][a-z-]*")


def tracked() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return out.split()


def help_text() -> str:
    run = subprocess.run([str(CADAPT), "--help"], capture_output=True, text=True, timeout=30)
    text = run.stdout + run.stderr
    if "usage:" not in text:
        raise AssertionError(f"{CADAPT} --help printed no usage; build the codec first (make)")
    return text


def fenced_block(text: str, first_line: str) -> str:
    """The fenced code block whose first line is `first_line`."""
    blocks = re.findall(r"```[a-z]*\n(.*?)```", text, flags=re.S)
    for block in blocks:
        if block.startswith(first_line):
            return block
    raise AssertionError(f"no fenced block starting with {first_line!r}")


def usage_tokens(text: str) -> list[str]:
    """The words of the usage lines: from the first `cadapt encode` to the
    last line that starts with `cadapt` or continues one (indented)."""
    lines = text.split("\n")
    start = next(i for i, line in enumerate(lines) if line.strip().startswith("cadapt encode"))
    words: list[str] = []
    for line in lines[start:]:
        if not line.strip():
            break
        if not (line.strip().startswith("cadapt") or line.startswith("  ")):
            break
        words += line.split()
    return words


class UsageTest(unittest.TestCase):
    def test_readme_usage_block_is_the_help_text(self) -> None:
        expected = usage_tokens(help_text())
        readme = fenced_block(README.read_text(encoding="utf-8"), "cadapt encode")
        self.assertEqual(usage_tokens(readme), expected, "README usage block differs from cadapt --help")

    def test_every_flag_of_help_is_in_the_manual_and_the_readme(self) -> None:
        flags = set(FLAG.findall(help_text()))
        self.assertGreaterEqual(len(flags), 10)
        manual = set(m.replace("\\", "") for m in re.findall(r"\\-\\-[a-z][a-z\\-]*", MANUAL.read_text(encoding="utf-8")))
        readme = set(FLAG.findall(README.read_text(encoding="utf-8")))
        self.assertEqual(flags - manual, set(), "flags in --help missing from cadapt.1")
        self.assertEqual(flags - readme, set(), "flags in --help missing from README.md")


def named_versions(text: str) -> set[int]:
    """Every version a passage names, singly (`53`) or as a range
    (`25–52`, `19 to 24`, `62--66`)."""
    named: set[int] = set()
    for a, b in re.findall(r"(?<![\d.])(\d{1,3})\s*(?:–|-|--|to)\s*(\d{1,3})(?![\d])", text):
        lo, hi = int(a), int(b)
        if lo < hi <= 255:
            named.update(range(lo, hi + 1))
    named.update(int(v) for v in re.findall(r"(?<![\d.,])(\d{1,3})(?!\d|,\d)", text))
    return named


class VersionTest(unittest.TestCase):
    def versions(self) -> dict[str, int]:
        text = HEADER.read_text(encoding="utf-8")
        found = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (ADAPT_[A-Z_]*VERSION) (\d+)u", text)}
        self.assertGreaterEqual(len(found), 20)
        return found

    def test_readme_names_every_version_family(self) -> None:
        text = README.read_text(encoding="utf-8")
        named = named_versions(text[: text.index("## Layout")])
        for name, value in self.versions().items():
            self.assertIn(value, named, f"{name} = {value} is not in the README's version paragraph")

    def test_manual_names_every_version_family(self) -> None:
        text = MANUAL.read_text(encoding="utf-8")
        named = named_versions(text[text.index(".SH FILES") :])
        for name, value in self.versions().items():
            self.assertIn(value, named, f"{name} = {value} is not in the manual's FILES section")

    def test_named_versions_reads_ranges_and_singles(self) -> None:
        self.assertEqual(named_versions("19 to 24, 53 and 56; 25–52 refused 55") & {19, 24, 53, 56, 30, 52, 55, 60},
                         {19, 24, 53, 56, 30, 52, 55})


class FloorsTest(unittest.TestCase):
    def test_rendered_blocks_are_current(self) -> None:
        data = floors.load()
        for path, text in floors.render_documents(data).items():
            self.assertEqual(text, path.read_text(encoding="utf-8"), f"{path.name}: run python3 tools/render_floors.py update")

    def test_render_check_reports_a_stale_block(self) -> None:
        data = floors.load()
        text = README.read_text(encoding="utf-8")
        stale = floors.replace_block(text, floors.MARKDOWN_START, floors.MARKDOWN_END, "| stale |\n", README)
        self.assertNotEqual(stale, text)
        self.assertEqual(floors.replace_block(stale, floors.MARKDOWN_START, floors.MARKDOWN_END, floors.markdown(data), README), text)
        with self.assertRaises(floors.RenderError):
            floors.replace_block("no markers", floors.MARKDOWN_START, floors.MARKDOWN_END, "x", README)


class GoalTest(unittest.TestCase):
    """The enwik9 goal is the LTCB top-20 threshold, computed from the
    snapshot by tools/ltcb/ltcb_goal.py; the documents that state it quote
    the figures it prints, so a refreshed snapshot that moves the place
    fails here until the documents follow."""

    def figures(self) -> ltcb_goal.Goal:
        return ltcb_goal.compute()

    def test_constraints_state_the_goal_from_the_snapshot(self) -> None:
        g = self.figures()
        text = (ROOT / "docs" / "CONSTRAINTS.md").read_text(encoding="utf-8")
        row = next(line for line in text.splitlines() if line.startswith("| `DLV-3`"))
        for figure in (f"{g.target_enwik9:,} B", f"{g.threshold_total:,} B", f"{g.decompressor_bytes:,} B",
                       f"top {g.place}", "tools/ltcb/ltcb_goal.py"):
            self.assertIn(figure, row, f"DLV-3 does not state {figure!r}")

    def test_readme_states_the_goal_and_the_memory_claim(self) -> None:
        g = self.figures()
        text = README.read_text(encoding="utf-8")
        section = text[text.index("## The Large Text Compression Benchmark"):]
        section = " ".join(section[: section.index("\n## ", 1)].split())  # prose wraps; figures do not
        least_mb, least_who = g.least_memory_above
        for figure in (f"{g.target_enwik9:,} B", f"{g.threshold_total:,} B", g.threshold_program,
                       f"rank {g.rank} of {g.entries}", f"{g.total:,} B", f"{g.memory_mb:,.0f} MB",
                       f"{least_mb:,.0f} MB", least_who, "tools/ltcb/ltcb_goal.py"):
            self.assertIn(figure, section, f"the README's benchmark section does not state {figure!r}")
        self.assertTrue(g.leads_memory_above, "the README claims the least memory above; the snapshot says otherwise")


class LayoutTest(unittest.TestCase):
    def test_layout_table_has_a_row_for_every_tracked_top_level_entry(self) -> None:
        text = README.read_text(encoding="utf-8")
        layout = text[text.index("## Layout") : text.index("## Build")]
        entries = sorted({path.split("/")[0] for path in tracked()} - {".gitattributes", ".gitignore", "LICENSE", "README.md"})
        for entry in entries:
            self.assertIn(f"[{entry}", layout, f"README layout table has no row for {entry}")

    def test_every_relative_link_in_a_tracked_markdown_file_resolves(self) -> None:
        for path in tracked():
            if not path.endswith(".md"):
                continue
            text = (ROOT / path).read_text(encoding="utf-8")
            for target in re.findall(r"\]\(([^)#\s]+)", text):
                if target.startswith(("http://", "https://", "mailto:")):
                    continue
                resolved = (ROOT / path).parent / target
                self.assertTrue(resolved.exists(), f"{path}: link {target} does not exist")


if __name__ == "__main__":
    unittest.main()
