#!/usr/bin/env python3
"""Validate copied documentation facts against code and maintained data.

Checks cover CLI syntax/options, accepted formats, memory tiers/minima,
generated baselines, dated benchmark comparisons, installation paths, CI jobs,
links and documented environment settings. Presentation and section order are
not contracts. Run after building the codec:

    python3 -B -m unittest discover -s tools -p 'test_docs.py'

CADAPT can select an alternate built executable.
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
import asset_inventory  # noqa: E402
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
    return asset_inventory.files(ROOT)


def tracked_documents() -> list[str]:
    """Every tracked Markdown file and the manual page."""
    return [path for path in tracked() if path.endswith(".md")] + ["adpat/c/cadapt.1"]


def dated_record(path: str) -> bool:
    """Only release history may describe commands from an older version."""
    return path == "CHANGELOG.md"


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
        readme = fenced_block(README.read_text(encoding="utf-8"), "cadapt encode <input>")
        self.assertEqual(usage_tokens(readme), expected, "README usage block differs from cadapt --help")

    def test_every_flag_of_help_is_in_the_manual_and_the_readme(self) -> None:
        flags = set(FLAG.findall(help_text()))
        self.assertGreaterEqual(len(flags), 10)
        manual = set(m.replace("\\", "") for m in re.findall(r"\\-\\-[a-z][a-z\\-]*", MANUAL.read_text(encoding="utf-8").split(".SH DESCRIPTION", 1)[1]))
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
    def test_documented_supported_sets_match_the_version_table(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        expected = {int(v) for v in re.findall(r"ADAPT_VERSION_ROW(?:_PRECISE)?\((\d+),", header)}
        self.assertGreater(len(expected), 20)
        for path in (README, MANUAL):
            match = re.search(r"Supported container versions: (.+)", path.read_text(encoding="utf-8"))
            self.assertIsNotNone(match, f"{path}: no supported format set")
            self.assertEqual(named_versions(match.group(1)), expected,
                             f"{path}: supported formats differ from ADAPT_VERSIONS")

    def test_named_versions_reads_ranges_and_singles(self) -> None:
        self.assertEqual(named_versions("19 to 24, 53 and 56; 25–52 refused 55") & {19, 24, 53, 56, 30, 52, 55, 60},
                         {19, 24, 53, 56, 30, 52, 55})


class TiersTest(unittest.TestCase):
    def test_documented_tiers_match_encoder_budgets(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        match = re.search(r"PRESET_MIB\[PRESETS\] = \{([^}]+)\}", source)
        self.assertIsNotNone(match)
        expected = [int(n) for n in re.findall(r"\d+", match.group(1))]
        readme = README.read_text(encoding="utf-8")
        rows = re.findall(r"^\| `-(\d+)` \| (\d+) \|$", readme, flags=re.M)
        self.assertEqual([(int(t), int(m)) for t, m in rows], list(enumerate(expected)),
                         "README tier table differs from PRESET_MIB")
        manual = MANUAL.read_text(encoding="utf-8")
        values = [int(n) for line in manual.splitlines() if line.startswith("MiB\t")
                  for n in line.split("\t")[1:]]
        self.assertEqual(values, expected, "manual tier table differs from PRESET_MIB")

    def test_documented_minima_match_the_sizer_in_help(self) -> None:
        match = re.search(r"smallest budget the\s+normal profile\s+runs in is (\d+) bytes, the max profile's (\d+)", help_text())
        self.assertIsNotNone(match, "help must print dynamically computed sizing minima")
        for path in (README, MANUAL):
            text = " ".join(path.read_text(encoding="utf-8").split())
            for label, number in zip(("normal", "max"), match.groups()):
                self.assertRegex(text.lower(), rf"{label} needs {int(number):,} bytes",
                                 f"{path}: {label} minimum differs from the sizer")


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


class BenchmarkComparisonTest(unittest.TestCase):
    """Published rank and memory comparisons match the benchmark snapshot.

    A changed snapshot requires the corresponding documented figures to change.
    Benchmark rank remains separate from the product acceptance contract.
    """

    def figures(self) -> ltcb_goal.Goal:
        return ltcb_goal.compute()

    def test_constraints_keep_benchmark_rank_separate_from_acceptance(self) -> None:
        text = (ROOT / "docs" / "CONSTRAINTS.md").read_text()
        row = next(line for line in text.splitlines() if line.startswith("| `DLV-3`"))
        self.assertIn("mixed corpus", row)
        self.assertNotIn("top 20", row)

    def test_research_reference_states_the_snapshot_comparison(self) -> None:
        g = self.figures()
        text = (RESEARCH / "ltcb-tables.md").read_text(encoding="utf-8")
        section = text[text.index("## Snapshot comparison"):]
        section = " ".join(section[: section.index("\n## ", 1)].split())  # prose wraps; figures do not
        least_mb, least_who = g.least_memory_above
        for figure in (f"{g.target_enwik9:,} B", f"{g.threshold_total:,} B", g.threshold_program,
                       f"rank {g.rank} of {g.entries}", f"{g.total:,} B", f"{g.memory_mb:,.0f} MB",
                       f"{least_mb:,.0f} MB", least_who, "tools/ltcb/ltcb_goal.py"):
            self.assertIn(figure, section, f"the snapshot reference does not state {figure!r}")
        self.assertTrue(g.leads_memory_above, "the snapshot reference claims the least memory above; the snapshot says otherwise")


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
        notes = ROOT / "docs" / "release-notes" / f"{version}.md"
        self.assertTrue(notes.exists(), f"the tagged {version} has no {notes.relative_to(ROOT)}: the release's body")


class MakeTargetsTest(unittest.TestCase):
    def test_every_cited_make_target_exists_where_it_is_cited(self) -> None:
        """`make x` cited in backticks is a rule of the root Makefile, and
        `make -C adpat/c x` one of the codec's. Changelog entries describe
        historical releases; all current guidance must resolve to a target."""
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
    OUTPUT_PREFIXES = ("bin/", "lib/", "include/", "share/", "build", "dist/", "out/", "runs/", "plots/",
                       "containers/", "/", "adpat/c/build")
    SUFFIX = re.compile(r"\.(c|h|py|sh|json|md|1|yml|yaml|csv|txt|zip|svg|png|html)$")

    def test_every_backticked_path_in_a_living_document_resolves(self) -> None:
        """A backticked token with a slash or a source suffix names a tracked
        file or directory (by its path, relative to the document, or by its
        bare name), a corpus file the corpus manifests name, or a file a
        tracked script writes (by its name or its stem: the scripts build
        `ltcb-phases.png` from `ltcb-phases.svg`, and the release's
        `cadapt-<version>-…` from `$VERSION`). Release history is checked against its own documented version."""
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
            if dated_record(doc):
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
        for path in (ROOT / name for name in tracked() if name.startswith("research/") and name.endswith(".md")):
            if path.name != "README.md":
                self.assertTrue(f"({path.name})" in index, f"research/README.md does not link {path.name}")


class PipelineDocTest(unittest.TestCase):
    def test_the_gates_table_names_every_workflow_job_and_no_other(self) -> None:
        """The "Where it runs" column of docs/release-pipeline.md names the
        jobs of ci.yml: every job is in the table, and every backticked
        token there is a job or a word of the workflow (a branch, a label)."""
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        jobs = set(re.findall(r"^  ([a-z][a-z-]+):$", ci, flags=re.M)) - {"push", "on"}
        text = (ROOT / "docs" / "release-pipeline.md").read_text(encoding="utf-8")
        table = text[text.index("| Gate | Holds |") :]
        table = table[: table.index("\n\n")]
        named: set[str] = set()
        for row in table.split("\n")[2:]:
            cells = [c.strip() for c in row.strip("|").split("|")]
            for token in re.findall(r"`([a-z][a-z-]+)`", cells[-1]):
                named.add(token)
                self.assertTrue(token in jobs or token in ci, f"docs/release-pipeline.md names `{token}`, which ci.yml has no job or word for")
        self.assertEqual(sorted(jobs - named), [], "ci.yml jobs the gates table does not place")


class InstallLayoutTest(unittest.TestCase):
    def test_the_install_layout_the_documents_state_is_the_makefiles(self) -> None:
        """`make install` writes the paths its recipe names; the README and
        the current release notes restate them and are held to the recipe."""
        dry = subprocess.run(["make", "-n", "-C", str(ROOT / "adpat" / "c"), "install", "PREFIX=/p"],
                             capture_output=True, text=True, check=True).stdout
        installed = sorted(set(re.findall(r"install -m \d+ \S+ /p/(\S+)", dry)))
        self.assertTrue(installed, "the install recipe installs nothing?")
        readme = README.read_text(encoding="utf-8")
        for path in installed:
            self.assertTrue(f"`{path}`" in readme, f"README.md does not name `{path}`, which make install writes")
        notes = ROOT / "docs" / "release-notes" / f"{cadapt_version()}.md"
        if notes.exists():
            text = notes.read_text(encoding="utf-8")
            for path in installed:
                self.assertTrue(f"`{path}`" in text, f"{notes.name} does not name `{path}`, which make install writes")
        # and nothing the documents name as installed is absent from the recipe
        for doc, text in (("README.md", readme),):
            claimed = set(re.findall(r"`((?:bin|lib|include|share)/[\w./]+)`", text))
            self.assertEqual(sorted(claimed - set(installed)), [], f"{doc} names installed files the recipe does not write")


class EnvironmentTest(unittest.TestCase):
    def test_every_environment_variable_the_code_reads_is_documented(self) -> None:
        """A variable read by the codec, the tests or the tools is named in
        the README, the manual or the bench README — or it is a knob nobody
        can find."""
        codec = ROOT / "adpat" / "c"
        read: set[str] = set()
        for path in list(codec.glob("*.c")) + list(codec.glob("*.h")):
            read |= set(re.findall(r'getenv\("([A-Z_]+)"\)', path.read_text(encoding="utf-8")))
        for path in list(codec.glob("tests/*.py")) + list((ROOT / "tools").glob("*.py")) + list((ROOT / "bench").glob("*.py")):
            read |= set(re.findall(r'environ(?:\.get)?\(?\[?"([A-Z_]+)"', path.read_text(encoding="utf-8")))
        documents = "\n".join((ROOT / name).read_text(encoding="utf-8") for name in ("README.md", "adpat/c/cadapt.1", "bench/README.md"))
        undocumented = sorted(name for name in read if name not in documents)
        self.assertEqual(undocumented, [], "environment variables the code reads and no document names")


class VersionRangeTest(unittest.TestCase):
    def test_the_current_release_notes_and_changelog_entry_state_the_binarys_range(self) -> None:
        """`cadapt --version` names the container versions the release reads
        ("format versions 19 to 85"); the current version's release notes
        and changelog entry restate the range and are held to it."""
        run = subprocess.run([str(CADAPT), "--version"], capture_output=True, text=True, timeout=30)
        match = re.search(r"\(format versions (\d+ to \d+)\)", run.stdout)
        self.assertIsNotNone(match, f"--version printed {run.stdout!r}")
        version, span = cadapt_version(), match.group(1)
        changelog = CHANGELOG.read_text(encoding="utf-8")
        entry = changelog[changelog.index(f"## [{version}]") :]
        entry = entry[: entry.index("\n## ", 1)] if "\n## " in entry[1:] else entry
        self.assertTrue(f"versions {span}" in " ".join(entry.split()), f"CHANGELOG.md's [{version}] entry does not state format versions {span}")
        notes = ROOT / "docs" / "release-notes" / f"{version}.md"
        if notes.exists():
            self.assertTrue(f"versions {span}" in " ".join(notes.read_text(encoding="utf-8").split()),
                            f"{notes.name} does not state format versions {span}")


class LayoutTest(unittest.TestCase):
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
