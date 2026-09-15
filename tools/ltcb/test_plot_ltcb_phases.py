"""Tests for the speed/ratio phase map: frontier phases, transitions, marginal gains.

Source: Claude Opus 5 via Claude Code. The analysis runs with the standard
library; the render test needs Matplotlib and is skipped without it.
"""

import importlib.util
import tempfile
import unittest
from pathlib import Path

import plot_ltcb_phases as phases
import plot_ltcb_tables as ltcb
from test_plot_ltcb_tables import LOCAL_FULL, SAMPLE, write_sample

HERE = ltcb.RESEARCH


def entry(program, alg, speed, enwik9_bytes, rank=1):
    return ltcb.Entry(rank=rank, label=program, program=program, enwik8_bytes=None,
                      enwik9_bytes=enwik9_bytes, decompressor_zip_bytes=0,
                      enwik9_plus_prog_bytes=enwik9_bytes, comp_ns_per_byte=speed,
                      decomp_ns_per_byte=speed, mem_mb=1.0, alg=alg, comp_pareto=False,
                      decomp_pareto=False, mem_pareto=False)


# Speeds and sizes chosen so the frontier is: a(LZ) b(LZ) c(BWT) d(CM) e(CM); f is dominated.
ENTRIES = [
    entry("a", "LZ77", 10.0, 500_000_000),
    entry("b", "LZ77", 20.0, 400_000_000),
    entry("f", "PPM", 25.0, 450_000_000),
    entry("c", "BWT", 100.0, 200_000_000),
    entry("d", "CM", 1_000.0, 150_000_000),
    entry("e", "CM", 10_000.0, 100_000_000),
]


class FrontierPhaseTest(unittest.TestCase):
    def test_frontier_points_carry_their_entries_and_skip_dominated_ones(self):
        points = phases.frontier_points(ENTRIES, "comp_ns_per_byte")
        self.assertEqual([p.entry.program for p in points], ["a", "b", "c", "d", "e"])
        self.assertAlmostEqual(points[0].bpb, 4.0)
        self.assertEqual(points[0].speed, 10.0)

    def test_phases_are_maximal_runs_of_one_family(self):
        found = phases.phases(phases.frontier_points(ENTRIES, "comp_ns_per_byte"))
        self.assertEqual([(p.family, len(p.points)) for p in found],
                         [("LZ", 2), ("BWT", 1), ("CM", 2)])
        self.assertEqual(found[0].start, 10.0)
        self.assertEqual(found[0].end, 100.0)  # a phase holds until the next family's first point
        self.assertEqual(found[2].end, 10_000.0)  # the last phase ends at the last frontier point
        self.assertAlmostEqual(found[0].gain_per_decade, (4.0 - 3.2) / 0.30103, places=4)
        self.assertIsNone(found[1].gain_per_decade)  # one point has no slope

    def test_transitions_report_the_jump_and_the_time_cost(self):
        found = phases.transitions(phases.phases(phases.frontier_points(ENTRIES, "comp_ns_per_byte")))
        self.assertEqual([(t.from_family, t.to_family) for t in found], [("LZ", "BWT"), ("BWT", "CM")])
        first = found[0]
        self.assertEqual(first.speed, 100.0)
        self.assertAlmostEqual(first.bpb_drop, 3.2 - 1.6)
        self.assertAlmostEqual(first.time_ratio, 5.0)
        self.assertEqual(first.entry.program, "c")

    def test_marginal_gains_are_per_step(self):
        steps = phases.marginal_gains(phases.frontier_points(ENTRIES, "comp_ns_per_byte"))
        self.assertEqual(len(steps), 4)
        self.assertAlmostEqual(steps[1].gain_per_decade, (3.2 - 1.6) / 0.69897, places=4)
        self.assertEqual(steps[1].start.entry.program, "b")

    def test_published_table_has_the_documented_phase_sequence(self):
        rows = ltcb.load_rows(HERE / "ltcb-main-table.csv")
        comp = phases.phases(phases.frontier_points(rows, "comp_ns_per_byte"))
        self.assertEqual([p.family for p in comp], ["LZ", "BWT", "CM", "PPM", "CM", "Neural"])
        cliff = phases.transitions(comp)[0]
        self.assertEqual(cliff.entry.program, "bsc 3.25")
        self.assertGreater(cliff.bpb_drop, 1.8)

    def test_empty_input_is_handled(self):
        self.assertEqual(phases.frontier_points([], "comp_ns_per_byte"), [])
        self.assertEqual(phases.phases([]), [])
        self.assertEqual(phases.transitions([]), [])
        self.assertEqual(phases.marginal_gains([]), [])


class RenderTest(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("matplotlib") is None:
            self.skipTest("matplotlib is not installed")

    def test_render_writes_figure_and_phase_csv(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rows = ltcb.load_rows(write_sample(root))
            (root / "local.csv").write_text(LOCAL_FULL)
            local = ltcb.load_local(root / "local.csv")
            outputs = phases.render(rows, root / "phases.svg", source="unit test", local=local)
            self.assertEqual([p.name for p in outputs], ["phases.svg", "phases.png", "phases.csv"])
            for path in outputs:
                self.assertGreater(path.stat().st_size, 200, path)
            svg = (root / "phases.svg").read_text()
            self.assertIn("ours 1.0", svg)
            table = (root / "phases.csv").read_text().splitlines()
            self.assertEqual(table[0].split(",")[:4], ["axis", "phase", "family", "program"])
            self.assertTrue(any(line.startswith("compression,") for line in table[1:]))
            self.assertTrue(any(line.startswith("decompression,") for line in table[1:]))

    def test_main_runs_on_the_sample(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            table = write_sample(root, SAMPLE)
            self.assertEqual(phases.main([str(table), "--output", str(root / "p.svg")]), 0)
            self.assertTrue((root / "p.csv").exists())


if __name__ == "__main__":
    unittest.main()
