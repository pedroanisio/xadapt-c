#!/usr/bin/env python3
"""The gates against what they enumerate: the silent drift vectors of
docs/drift-risk-map-2026-09-18.md, each held so it cannot reopen.

1. The profiles the memory gate and the identical-bytes job hold come from
   the binary (`cadapt --help`) through tools/profiles.py, never a typed
   list; a usage without the profile group is refused.
2. Every part of `make check` but the coverage pass is a part of
   `make check-suites`, and the clang and macOS workflow columns run that
   target and `make sanitize-cli` with nothing inline.
3. (The tier ladder is held by tools/test_docs.py's TiersTest.)
4. Every suite under adpat/c/tests/test_*.c is built and run by `make
   test` and by the coverage pass, a new one included, with no list to add
   it to; the coverage units are the tree's, the CLI classes the file's.
5. The build manifest is not tracked and not exported: it names the commit
   and the host it was made on.
6. Every test file under tools/, bench/ and adpat/c/tests/ is run by a part
   of `make check` — the union of what the Makefiles name.
7. The corpus floors name every file of the corpus manifest, at its size.
8. A revision without its release notes is refused by tools/release.sh, and
   the workflow has no fallback body.
9. The constants LOCKS.md states as policy are the ones in force.

    python3 -B tools/test_gates.py

Authored with Claude Opus 5 (Claude Code), 2026-09-18.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import profiles  # noqa: E402

CODEC = ROOT / "adpat" / "c"
CODEC_MAKEFILE = CODEC / "Makefile"
ROOT_MAKEFILE = ROOT / "Makefile"
CI = ROOT / ".github" / "workflows" / "ci.yml"
RELEASE = ROOT / ".github" / "workflows" / "release.yml"
LOCKS = ROOT / "LOCKS.md"
CADAPT = Path(os.environ.get("CADAPT", CODEC / "build" / "cadapt"))

USAGE = "usage:\n  cadapt encode <input> [-o <output>] [--force] [--verify | --no-verify] [--explain] [--fast | --mid | --max] [-0 .. -11]\n"


def make_variable(name: str, makefile: Path = CODEC_MAKEFILE) -> list[str]:
    """A Makefile variable as make expands it."""
    out = subprocess.run(["make", "-s", "--no-print-directory", "-C", str(makefile.parent), "-f", makefile.name,
                          f"--eval=print-var: ; @echo $({name})", "print-var"],
                         capture_output=True, text=True, check=True)
    return out.stdout.split()


def rule_prerequisites(makefile_text: str, target: str) -> list[str]:
    """The prerequisites of an explicit rule, continuation lines joined."""
    match = re.search(rf"^{re.escape(target)}:((?:[^\n]*\\\n)*[^\n]*)", makefile_text, flags=re.M)
    if match is None:
        raise AssertionError(f"no rule {target}: in adpat/c/Makefile")
    return match.group(1).replace("\\\n", " ").split()


def recipe(makefile_text: str, target: str) -> str:
    """The recipe lines of a rule, joined."""
    match = re.search(rf"^{re.escape(target)}:[^\n]*\n((?:\t[^\n]*\n)+)", makefile_text, flags=re.M)
    if match is None:
        raise AssertionError(f"no recipe for {target}: in adpat/c/Makefile")
    return match.group(1)


class ProfilesTest(unittest.TestCase):
    def test_the_profiles_are_the_usages_bracket_group(self) -> None:
        self.assertEqual(profiles.profiles_of(USAGE), ["", "--fast", "--mid", "--max"])

    def test_a_usage_without_the_group_is_refused(self) -> None:
        with self.assertRaises(profiles.ProfilesError):
            profiles.profiles_of("usage:\n  cadapt encode <input> [-o <output>] [--verify]\n")

    def test_the_commands_are_every_profile_the_locked_command_and_max_6_once_each(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            floor = Path(scratch) / "floor.json"
            floor.write_text('{"arguments": ["encode", "--max", "--allocation", "context", "--contexts", "lean"]}')
            binary = Path(scratch) / "cadapt"
            binary.write_text("#!/bin/sh\nprintf '%s' \"$USAGE\"\n".replace("$USAGE", USAGE.replace("'", "'\\''")))
            binary.chmod(0o755)
            commands = profiles.commands(binary, floor)
        self.assertEqual(commands, ["", "--fast", "--mid", "--max", "--max --allocation context --contexts lean", "--max -6"])
        self.assertEqual(len(commands), len(set(commands)))

    def test_a_locked_command_that_is_not_an_encode_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            floor = Path(scratch) / "floor.json"
            floor.write_text('{"arguments": ["decode"]}')
            with self.assertRaises(profiles.ProfilesError):
                profiles.locked_command(floor)

    @unittest.skipUnless(CADAPT.exists(), "build the codec first (make)")
    def test_the_built_binary_names_its_profiles(self) -> None:
        listed = profiles.profiles(CADAPT)
        self.assertIn("", listed)
        self.assertIn("--max", listed)
        self.assertEqual(profiles.commands(CADAPT)[-1], "--max -6")

    def test_the_gates_take_their_commands_from_the_binary_not_a_typed_list(self) -> None:
        memory = (ROOT / "tools" / "check_memory.py").read_text(encoding="utf-8")
        self.assertIn("profiles.commands(", memory, "check_memory.py does not take its commands from tools/profiles.py")
        self.assertNotRegex(memory, r'\[\s*"",\s*"--fast"', "check_memory.py types a profile list")
        ci = CI.read_text(encoding="utf-8")
        job = ci[ci.index("identical-bytes:") : ci.index("size-and-memory-floors:")]
        self.assertIn("tools/profiles.py --binary", job, "the identical-bytes job does not list its flag sets from tools/profiles.py")
        self.assertNotIn('for flags in "', job, "the identical-bytes job types its flag sets")


class CheckSuitesTest(unittest.TestCase):
    def test_every_part_of_make_check_but_coverage_is_a_part_of_check_suites(self) -> None:
        text = CODEC_MAKEFILE.read_text(encoding="utf-8")
        parts = {p for p in rule_prerequisites(text, "check-parts") if p not in ("coverage", "$(BUILD)/runs/gcov-tests")}
        suites = set(recipe(text, "check-suites").split())
        missing = sorted(parts - suites)
        self.assertEqual(missing, [], f"parts of make check that make check-suites does not run: {missing}")

    def test_the_workflows_other_columns_run_the_targets_and_nothing_inline(self) -> None:
        ci = CI.read_text(encoding="utf-8")
        check_job = ci[ci.index("  check:") : ci.index("  identical-bytes:")]
        self.assertIn("make -C adpat/c check-suites CC=", check_job)
        self.assertIn("make -C adpat/c sanitize-cli", check_job)
        inline = re.findall(r"python3 -B adpat/c/tests/\S+", check_job)
        self.assertEqual(inline, [], f"the check job runs suites inline instead of through make: {inline}")
        self.assertNotIn("-fsanitize=", check_job, "the check job restates the sanitizer flags (SANITIZE_CFLAGS)")


class SuitesTest(unittest.TestCase):
    def test_the_suites_are_the_files_under_tests(self) -> None:
        files = sorted(p.stem for p in (CODEC / "tests").glob("test_*.c"))
        self.assertEqual(sorted(make_variable("SUITES")), files)

    def test_every_suite_has_a_run(self) -> None:
        runs = set(make_variable("TESTRUNS"))
        sharded = set(make_variable("SHARDED"))
        for suite in make_variable("SUITES"):
            if suite in sharded:
                shard = suite.replace("test_adapt_", "")
                self.assertTrue(any(r.startswith(f"{shard}-") for r in runs), f"{suite}: no shard runs")
            else:
                self.assertIn(suite, runs, f"{suite} is built and never run")

    def test_a_new_suite_is_built_and_run_with_no_list_to_add_it_to(self) -> None:
        """A dry run of make test with a suite that no rule names: it is
        compiled by the pattern rule and run by the runs rule."""
        probe = CODEC / "tests" / "test_zz_drift_probe.c"
        self.assertFalse(probe.exists())
        probe.write_text('#include "../adapt.h"\nint main(void) { return 0; }\n')
        try:
            out = subprocess.run(["make", "-n", "-C", str(CODEC), "test"], capture_output=True, text=True).stdout
        finally:
            probe.unlink()
        self.assertIn("tests/test_zz_drift_probe.c", out, "the new suite is not compiled")
        self.assertRegex(out, r"build/test_zz_drift_probe\s+> build/runs/test_zz_drift_probe\.log", "the new suite is not run")


class CoverageListsTest(unittest.TestCase):
    def test_the_coverage_units_are_the_tree(self) -> None:
        self.assertEqual(sorted(make_variable("COVUNITS")), sorted(p.stem for p in CODEC.glob("*.c")))

    def test_the_coverage_pass_runs_every_suite_and_every_cli_class(self) -> None:
        runs = set(make_variable("COVRUNS"))
        self.assertTrue(set(make_variable("TESTRUNS")) <= runs, "a C suite make test runs is not run under coverage")
        listed = subprocess.run([sys.executable, "-B", str(CODEC / "tests" / "test_cadapt.py"), "--classes"],
                                capture_output=True, text=True, check=True).stdout.split()
        declared = re.findall(r"^class (\w+)\(", (CODEC / "tests" / "test_cadapt.py").read_text(encoding="utf-8"), flags=re.M)
        self.assertEqual(set(declared) - set(listed), {"CodecTest"}, "a class with tests is missing from --classes, or a base class has none")
        for name in listed:
            self.assertIn(f"cli-{name}", runs, f"the CLI class {name} is not run under coverage")
        self.assertIn("safety", runs)


class ToolTestsTest(unittest.TestCase):
    """Every test file is run by some part of make check: the root Makefile's
    parts name tools/ and bench/ files and patterns, the codec's Makefile the
    suites under adpat/c/tests/."""

    @staticmethod
    def files_run_by(makefile: Path) -> set[str]:
        # recipes only: a comment naming a file runs nothing
        text = "\n".join(line for line in makefile.read_text(encoding="utf-8").split("\n") if not line.lstrip().startswith("#"))
        found: set[str] = set()
        for path in re.findall(r"(?:tools|bench|tests)/[\w/]+\.py", text):
            found.add(path)
        for directory, pattern in re.findall(r"discover -s (\S+) -p '([^']+)'", text):
            for path in (ROOT / directory).glob(pattern):
                found.add(f"{directory}/{path.name}")
        return found

    @classmethod
    def unrun(cls) -> list[str]:
        run = cls.files_run_by(ROOT_MAKEFILE)
        files = {f"{p.parent.relative_to(ROOT)}/{p.name}" for d in ("tools", "tools/ltcb", "bench") for p in (ROOT / d).glob("test_*.py")}
        return sorted(files - run)

    def test_every_tool_and_bench_test_file_is_run_by_make_check(self) -> None:
        self.assertEqual(self.unrun(), [], "test files no part of make check runs")

    def test_every_codec_python_suite_is_named_by_the_codec_makefile(self) -> None:
        text = CODEC_MAKEFILE.read_text(encoding="utf-8")
        for path in (CODEC / "tests").glob("test_*.py"):
            self.assertIn(f"tests/{path.name}", text, f"adpat/c/Makefile never runs {path.name}")

    def test_a_new_tool_test_file_outside_every_pattern_is_reported(self) -> None:
        probe = ROOT / "tools" / "test_zz_drift_probe.py"
        self.assertFalse(probe.exists())
        probe.write_text("import unittest\n")
        try:
            unrun = self.unrun()
        finally:
            probe.unlink()
        self.assertEqual(unrun, ["tools/test_zz_drift_probe.py"])


class CorpusFloorsTest(unittest.TestCase):
    def test_the_corpus_floors_name_every_file_of_the_manifest_at_its_size(self) -> None:
        manifest = json.loads((ROOT / "static" / "corpus" / "mixed" / "manifest.json").read_text(encoding="utf-8"))
        files = {f"{corpus['name']}/{entry['name']}": entry["bytes"] for corpus in manifest["corpora"] for entry in corpus["files"]}
        for floor_file in sorted((ROOT / "tools").glob("corpus_floor_*.json")):
            floors = json.loads(floor_file.read_text(encoding="utf-8"))["floors"]
            rows = {f["fixture"]: f["input_bytes"] for f in floors}
            self.assertEqual(sorted(rows), sorted(files), f"{floor_file.name}: the rows are not the manifest's files")
            for name, size in files.items():
                self.assertEqual(rows[name], size, f"{floor_file.name}: {name} is {rows[name]:,} B, the manifest says {size:,}")


class ReleaseNotesTest(unittest.TestCase):
    def test_release_sh_refuses_a_revision_without_its_notes(self) -> None:
        script = (ROOT / "tools" / "release.sh").read_text(encoding="utf-8")
        self.assertIn('docs/release-notes/$VERSION.md', script)
        # v0.11.0 predates the notes; the refusal comes before any export or build
        has_tag = subprocess.run(["git", "rev-parse", "-q", "--verify", "v0.11.0^{commit}"], cwd=ROOT, capture_output=True)
        if has_tag.returncode != 0:
            self.skipTest("no v0.11.0 in this clone")
        with tempfile.TemporaryDirectory() as scratch:
            run = subprocess.run(["tools/release.sh", "--rev", "v0.11.0", "--no-tests", "--out", scratch], cwd=ROOT,
                                 capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn("no docs/release-notes/0.11.0.md", run.stderr)

    def test_the_workflow_publishes_the_notes_with_no_fallback(self) -> None:
        text = RELEASE.read_text(encoding="utf-8")
        self.assertIn('cp "docs/release-notes/$version.md" notes.md', text)
        self.assertFalse("awk" in text, "release.yml falls back to the changelog for the release body")


class LocksTest(unittest.TestCase):
    """The constants LOCKS.md states in the present tense are the ones in
    force; a change to a source is a new lock entry, not a silent drift."""

    def test_the_check_budget_is_the_makefiles_default(self) -> None:
        stated = re.search(r"`make check` is held to (\d+) s of wall time", LOCKS.read_text(encoding="utf-8"))
        self.assertIsNotNone(stated)
        default = re.search(r"^CHECK_SECONDS \?= (\d+)", ROOT_MAKEFILE.read_text(encoding="utf-8"), flags=re.M)
        self.assertEqual(int(stated.group(1)), int(default.group(1)), "LOCKS.md and the Makefile disagree on the check budget")

    def test_the_tolerances_are_the_floor_files(self) -> None:
        text = LOCKS.read_text(encoding="utf-8")
        stays = re.search(r"The (\d+) % tolerance stays", text)
        self.assertIsNotNone(stays)
        ratio = json.loads((ROOT / "tools" / "ratio_floor.json").read_text(encoding="utf-8"))
        self.assertEqual(round(ratio["throughput_tolerance"] * 100), int(stays.group(1)), "tools/ratio_floor.json's tolerance is not the locked one")
        corpus = re.search(r"corpus floor files stay in wall seconds at their (\d+) % tolerance", text)
        self.assertIsNotNone(corpus)
        for floor_file in sorted((ROOT / "tools").glob("corpus_floor_*.json")):
            data = json.loads(floor_file.read_text(encoding="utf-8"))
            self.assertEqual(round(data["throughput_tolerance"] * 100), int(corpus.group(1)), floor_file.name)
            self.assertTrue(all("wall_seconds" in f for f in data["floors"]), f"{floor_file.name}: not in wall seconds as locked")


class ManifestTest(unittest.TestCase):
    def test_the_build_manifest_is_not_tracked_nor_exported(self) -> None:
        tracked = subprocess.run(["git", "ls-files", "build-manifest.json"], cwd=ROOT, capture_output=True, text=True).stdout.split()
        self.assertEqual(tracked, [], "build-manifest.json is tracked: it names the commit and the host, so it is stale by the next commit")
        release = (ROOT / "tools" / "release.sh").read_text(encoding="utf-8")
        archive = re.search(r"git archive [^\n]*(?:\\\n[^\n]*)*", release).group(0)
        self.assertNotIn("build-manifest.json", archive, "tools/release.sh exports a tracked build manifest")
        self.assertIn("/build-manifest.json", (ROOT / ".gitignore").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
