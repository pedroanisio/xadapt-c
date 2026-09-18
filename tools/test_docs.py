#!/usr/bin/env python3
"""The documents against the code they describe.

Source: Claude Opus 5 via Claude Code, 2026-09-15 (a doc-hygiene audit of
that day, not committed) and 2026-09-18 (docs/doc-hygiene-report-2026-09-18.md).
Each test pins one thing a document hand-copies from a source of truth: the
README's usage block against `cadapt --help`, the flags of the manual and the
README against the binary, the container versions of adapt.h against the
README and the manual, the floors blocks against tools/ratio_floor.json
(render_floors.py), the README's layout table against the tracked tree, every
relative link and every backticked path against the tree (or the script that
writes it), every cited `make` target against the Makefile it is invoked in,
the changelog's top entry against CADAPT_VERSION and the tag, and the research
index against the notes. A failure names what moved.

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
SOURCE = ROOT / "adpat" / "c" / "cadapt.c"
CHANGELOG = ROOT / "CHANGELOG.md"
RESEARCH = ROOT / "research"
CADAPT = Path(os.environ.get("CADAPT", ROOT / "adpat" / "c" / "build" / "cadapt"))
FLAG = re.compile(r"--[a-z][a-z-]*")


def tracked() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return out.split()


def tracked_documents() -> list[str]:
    """Every tracked Markdown file and the manual page."""
    return [path for path in tracked() if path.endswith(".md")] + ["adpat/c/cadapt.1"]


def dated_record(path: str) -> bool:
    """A document that declares itself true of its date and not kept
    current: the changelog, the lock log, the dated audits under docs/ and
    every research note but the index and the CSV note (research/README.md)."""
    name = path.rsplit("/", 1)[-1]
    if path in ("CHANGELOG.md", "LOCKS.md"):
        return True
    if path.startswith("research/"):
        return name not in ("README.md", "ltcb-tables.md")
    return path.startswith("docs/") and re.search(r"-\d{4}-\d{2}-\d{2}\.md$", name) is not None


def cadapt_version() -> str:
    match = re.search(r'^#define CADAPT_VERSION "([^"]+)"', SOURCE.read_text(encoding="utf-8"), flags=re.M)
    if match is None:
        raise AssertionError(f"{SOURCE}: no CADAPT_VERSION")
    return match.group(1)


def make_targets(makefile: Path) -> set[str]:
    """The names a rule line of the Makefile makes: `a b c:` names three."""
    names: set[str] = set()
    for line in makefile.read_text(encoding="utf-8").split("\n"):
        match = re.match(r"^([a-z][a-z -]*):(?!=)", line)
        if match:
            names.update(match.group(1).split())
    return names


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


class ChangelogTest(unittest.TestCase):
    def test_top_entry_is_the_source_version_and_the_tag(self) -> None:
        """The first versioned heading names CADAPT_VERSION with a date; when
        HEAD carries a release tag it is v<that version> and nothing waits
        under [Unreleased]."""
        text = CHANGELOG.read_text(encoding="utf-8")
        headings = re.findall(r"^## \[([^\]]+)\](?: — (\S+))?", text, flags=re.M)
        self.assertEqual(headings[0][0], "Unreleased", "CHANGELOG.md does not open with [Unreleased]")
        version, date = headings[1]
        self.assertEqual(version, cadapt_version(), f"CHANGELOG.md's top entry is {version}, cadapt.c says {cadapt_version()}")
        self.assertRegex(date, r"^\d{4}-\d{2}-\d{2}$", f"the {version} entry has no date")
        tag = subprocess.run(["git", "describe", "--exact-match", "--tags", "HEAD"], cwd=ROOT, capture_output=True, text=True)
        if tag.returncode != 0:
            return  # not a tagged commit: the entry may be in the making
        self.assertEqual(tag.stdout.strip(), f"v{version}", f"HEAD is tagged {tag.stdout.strip()}, the changelog says {version}")
        unreleased = text[text.index("## [Unreleased]") : text.index(f"## [{version}]")]
        self.assertNotIn("\n- ", unreleased, "a tagged commit leaves nothing under [Unreleased]")


class MakeTargetsTest(unittest.TestCase):
    def test_every_cited_make_target_exists_where_it_is_cited(self) -> None:
        """`make x` cited in backticks is a rule of the root Makefile, and
        `make -C adpat/c x` one of the codec's. Dated records (the changelog,
        the audits, the research notes) are history or proposals and are not
        held; LOCKS.md is, since its locks are the policy in force."""
        root, codec = make_targets(ROOT / "Makefile"), make_targets(ROOT / "adpat" / "c" / "Makefile")
        citation = re.compile(r"`make(?: (-C adpat/c))?(?: -j\d*)?(?: [A-Z_]+=\S+)* ([a-z][a-z-]*)[^`]*`")
        problems = []
        for path in tracked_documents():
            if dated_record(path) and path != "LOCKS.md":
                continue
            text = (ROOT / path).read_text(encoding="utf-8")
            for line_number, line in enumerate(text.split("\n"), 1):
                for directory, target in citation.findall(line):
                    known = codec if directory else root
                    if target not in known:
                        problems.append(f"{path}:{line_number}: `make {directory + ' ' if directory else ''}{target}` is not a rule of "
                                        f"{'adpat/c/Makefile' if directory else 'Makefile'}")
        self.assertEqual(problems, [])


class PathsTest(unittest.TestCase):
    # what a document may name that the tree does not hold: an install
    # layout, a build or release directory, a run's output, an absolute path
    OUTPUT_PREFIXES = ("bin/", "lib/", "include/", "share/", "build", "dist/", "_tmp", "out/", "runs/", "plots/",
                       "containers/", "/", "adpat/c/build")
    SUFFIX = re.compile(r"\.(c|h|py|sh|json|md|1|yml|yaml|csv|txt|zip|svg|png|html)$")

    def test_every_backticked_path_in_a_living_document_resolves(self) -> None:
        """A backticked token with a slash or a source suffix names a tracked
        file or directory (by its path, relative to the document, or by its
        bare name), a corpus file the corpus manifests name, or a file a
        tracked script writes (by its name or its stem: the scripts build
        `ltcb-phases.png` from `ltcb-phases.svg`, and the release's
        `cadapt-<version>-…` from `$VERSION`). Dated records are true of
        their date and are not held; CLAUDE.md names files that apply when
        present and is policy, not a description of the tree."""
        files = tracked()
        paths = set(files)
        directories = {str(parent) for path in files for parent in Path(path).parents if str(parent) != "."}
        basenames = {Path(path).name for path in files}
        writers = "\n".join((ROOT / path).read_text(encoding="utf-8", errors="replace") for path in files
                             if path.endswith((".py", ".sh", ".yml")) or path.endswith("Makefile")
                             or (path.startswith("static/corpus/") and path.endswith(".json")))
        version = cadapt_version()
        problems = []
        for doc in tracked_documents():
            if dated_record(doc) or doc == "CLAUDE.md":
                continue
            text = (ROOT / doc).read_text(encoding="utf-8")
            for token in sorted(set(re.findall(r"`([A-Za-z0-9_./-]+)`", text))):
                if "/" not in token and not self.SUFFIX.search(token):
                    continue
                if token.startswith(self.OUTPUT_PREFIXES) or re.fullmatch(r"[\d/kn]+", token):
                    continue
                name = token.rstrip("/")
                candidates = {name, name.lstrip("./"), str(Path(doc).parent / name)}
                if candidates & paths or candidates & directories or Path(name).name in basenames:
                    continue
                leaf = Path(name).name.replace(version, "$VERSION")
                if leaf in writers or Path(leaf).stem in writers:
                    continue  # written by a tool of the tree, or a corpus row its manifest names
                problems.append(f"{doc}: `{token}`")
        self.assertEqual(problems, [], "paths named by a document that neither the tree nor a tool of the tree holds")


class ResearchIndexTest(unittest.TestCase):
    def test_every_research_note_is_indexed(self) -> None:
        index = (RESEARCH / "README.md").read_text(encoding="utf-8")
        for path in sorted(RESEARCH.glob("*.md")):
            if path.name != "README.md":
                self.assertTrue(f"({path.name})" in index, f"research/README.md does not link {path.name}")


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
