"""Tests for the interactive page builder.

Source: Claude Opus 5 via Claude Code. Builds from the repository's own data
files; the inline script is syntax-checked with node when node is available.
"""

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import analyze_ltcb
import build_ltcb_interactive as build
import plot_ltcb_phases as phases_tool
import plot_ltcb_tables as ltcb
from test_plot_ltcb_tables import LOCAL_HEADER

HERE = Path(__file__).resolve().parent
DATA = ltcb.RESEARCH
# The phase table and the analysis are renderings `make ltcb` builds, and
# .gitignore excludes them, so a fresh clone does not have them. Building both
# here from the tracked CSVs keeps the suite runnable on any checkout and
# needs no Matplotlib: the phase table's numbers come from the writer the
# figure shares, and the analysis tool draws nothing.
_generated = tempfile.TemporaryDirectory()
GENERATED = Path(_generated.name)
phases_tool.write_phase_table(ltcb.load_rows(DATA / "ltcb-main-table.csv"),
                              GENERATED / "ltcb-phases.csv")
analyze_ltcb.main(["--local", str(DATA / "ltcb-local-entries.csv"),
                   "--output", str(GENERATED / "ltcb-analysis.json")])

INPUTS = dict(table=DATA / "ltcb-main-table.csv", local=DATA / "ltcb-local-entries.csv",
              phases=GENERATED / "ltcb-phases.csv", analysis=GENERATED / "ltcb-analysis.json",
              template=HERE / "ltcb-interactive.template.html")


def embedded(html: str) -> dict:
    match = re.search(r'<script id="ltcb-data" type="application/json">(.*?)</script>', html, re.S)
    return json.loads(match.group(1).replace("<\\/", "</"))


class BuildTest(unittest.TestCase):
    def test_page_embeds_every_input(self):
        html = build.build(**INPUTS, source="unit source", fetched="unit fetched")
        self.assertIn("<title>LTCB Frontier Explorer</title>", html)
        self.assertNotIn(build.PLACEHOLDER, html)
        data = embedded(html)
        self.assertEqual(len(data["rows"]), 226)
        self.assertEqual(data["rows"][0]["rank"], 1)
        self.assertEqual(set(data["phases"]), {"compression", "decompression"})
        self.assertEqual(data["meta"]["source"], "unit source")
        self.assertEqual(set(data["meta"]["families"]), set(ltcb.FAMILIES))
        self.assertIn("scaling", data["analysis"])
        self.assertTrue(all(r["family"] in ltcb.FAMILIES for r in data["rows"]))
        self.assertGreaterEqual(len(data["local"]), 1)
        self.assertTrue(data["local"][0]["label"].startswith("cadapt"))

    def test_closing_tag_in_data_cannot_break_out_of_the_script(self):
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary) / "local.csv"
            local.write_text(LOCAL_HEADER + "x</script><b>y,x,,18564510,,1,s,1,1,1,CM,enwik8,here\n")
            html = build.build(**{**INPUTS, "local": local}, source="s", fetched="f")
        self.assertNotIn("x</script>", html)
        self.assertEqual(embedded(html)["local"][0]["label"], "x</script><b>y")

    def test_template_without_placeholder_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "t.html"
            template.write_text("<title>t</title>")
            with self.assertRaises(ltcb.TableError):
                build.build(**{**INPUTS, "template": template}, source="s", fetched="f")

    def test_no_scratch_paths_leak_into_the_page(self):
        html = build.build(**INPUTS, source="s", fetched="f")
        self.assertNotIn("/tmp/", html)
        self.assertNotIn("_tmp/", html)

    def test_inline_script_parses(self):
        node = shutil.which("node")
        if node is None:
            self.skipTest("node is not installed")
        html = build.build(**INPUTS, source="s", fetched="f")
        script = re.findall(r"<script>(.*?)</script>", html, re.S)[-1]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "page.js"
            path.write_text(script)
            result = subprocess.run([node, "--check", str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_main_writes_the_page(self):
        """main() defaults to the renderings in research/, so it is given the
        generated ones explicitly rather than requiring `make ltcb` first."""
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "page.html"
            self.assertEqual(build.main([
                "--output", str(out),
                "--phases", str(INPUTS["phases"]),
                "--analysis", str(INPUTS["analysis"]),
            ]), 0)
            self.assertGreater(out.stat().st_size, 50_000)


if __name__ == "__main__":
    unittest.main()
