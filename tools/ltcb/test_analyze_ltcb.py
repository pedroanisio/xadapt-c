"""Tests for the LTCB statistical analysis.

Source: Claude Opus 5 via Claude Code. Standard library only; every statistic
is checked against a closed-form case before it is trusted on the table.
"""

import json
import math
import tempfile
import unittest
from pathlib import Path

import analyze_ltcb as an
import plot_ltcb_tables as ltcb
from test_plot_ltcb_phases import entry
from test_plot_ltcb_tables import LOCAL_E8_ONLY, LOCAL_FULL, write_sample

HERE = ltcb.RESEARCH


class StatisticsTest(unittest.TestCase):
    def test_spearman_handles_monotone_reversed_and_ties(self):
        self.assertAlmostEqual(an.spearman([1, 2, 3, 4], [10, 20, 30, 40]), 1.0)
        self.assertAlmostEqual(an.spearman([1, 2, 3, 4], [40, 30, 20, 10]), -1.0)
        self.assertAlmostEqual(an.spearman([1, 2, 2, 4], [1, 3, 3, 4]), 1.0)
        self.assertTrue(math.isnan(an.spearman([1, 1, 1], [1, 2, 3])))

    def test_ols_recovers_exact_line_and_plane(self):
        xs = [1.0, 2.0, 3.0, 4.0, 5.0]
        fit = an.ols([2 + 3 * x for x in xs], [[1.0, x] for x in xs])
        self.assertAlmostEqual(fit.coef[0], 2.0)
        self.assertAlmostEqual(fit.coef[1], 3.0)
        self.assertAlmostEqual(fit.r2, 1.0)
        self.assertAlmostEqual(fit.resid_sd, 0.0)
        rows = [[1.0, x, x * x % 3] for x in xs]
        fit = an.ols([1 + 2 * r[1] - 0.5 * r[2] for r in rows], rows)
        self.assertAlmostEqual(fit.coef[1], 2.0)
        self.assertAlmostEqual(fit.coef[2], -0.5)

    def test_ols_standard_errors_match_the_textbook_case(self):
        # y = x + noise with known residuals: SE(slope) = s / sqrt(Sxx)
        xs = [1.0, 2.0, 3.0, 4.0, 5.0]
        ys = [1.1, 1.9, 3.2, 3.8, 5.0]
        fit = an.ols(ys, [[1.0, x] for x in xs])
        mean = sum(xs) / 5
        sxx = sum((x - mean) ** 2 for x in xs)
        self.assertAlmostEqual(fit.se[1], fit.resid_sd / math.sqrt(sxx))
        self.assertEqual(fit.n, 5)

    def test_ols_rejects_singular_design(self):
        with self.assertRaises(an.AnalysisError):
            an.ols([1.0, 2.0, 3.0], [[1.0, 1.0], [1.0, 1.0], [1.0, 1.0]])

    def test_quantiles(self):
        self.assertEqual(an.quartiles([5, 1, 3, 2, 4]), (2.0, 3.0, 4.0))
        self.assertEqual(an.quartiles([1.0]), (1.0, 1.0, 1.0))


def scaled_entries():
    """Entries whose enwik9 ratio is exactly 0.8 x their enwik8 ratio."""
    made = []
    for index, bpb8 in enumerate((1.2, 1.5, 2.0, 3.0, 4.0), start=1):
        e8 = int(bpb8 * ltcb.ENWIK8_BYTES / 8)
        e9 = int(0.8 * bpb8 * ltcb.ENWIK9_BYTES / 8)
        base = entry(f"p{index}", "CM", 100.0 * index, e9, rank=index)
        made.append(ltcb.Entry(**{**base.__dict__, "enwik8_bytes": e8, "mem_mb": 10.0 * index}))
    return made


class DerivedTest(unittest.TestCase):
    def test_scaling_fit_recovers_the_exact_law_and_predicts(self):
        fit = an.scaling_fit(scaled_entries())
        self.assertAlmostEqual(fit.slope, 1.0, places=6)
        self.assertAlmostEqual(fit.factor_at(1.485), 0.8, places=6)
        point, low, high = fit.predict(1.485)
        self.assertAlmostEqual(point, 1.485 * 0.8, places=6)
        self.assertLessEqual(low, point)
        self.assertGreaterEqual(high, point)

    def test_cost_ladder_reads_the_frontier(self):
        rows = [entry("fast", "LZ77", 10.0, 500_000_000), entry("mid", "BWT", 100.0, 200_000_000),
                entry("slow", "CM", 10_000.0, 100_000_000)]
        ladder = an.cost_ladder(rows, "comp_ns_per_byte", targets=(4.0, 1.6, 1.0, 0.5))
        self.assertEqual([(step.target, step.program) for step in ladder],
                         [(4.0, "fast"), (1.6, "mid"), (1.0, "slow"), (0.5, None)])
        self.assertAlmostEqual(ladder[1].time_multiplier, 10.0)
        self.assertAlmostEqual(ladder[2].time_multiplier, 1000.0)

    def test_frontier_gaps_are_zero_on_the_frontier_and_positive_off_it(self):
        rows = [entry("fast", "LZ77", 10.0, 500_000_000), entry("mid", "BWT", 100.0, 200_000_000),
                entry("worse", "PPM", 200.0, 300_000_000)]
        gaps = {g.program: g.gap for g in an.frontier_gaps(rows, "comp_ns_per_byte")}
        self.assertAlmostEqual(gaps["fast"], 0.0)
        self.assertAlmostEqual(gaps["mid"], 0.0)
        self.assertAlmostEqual(gaps["worse"], 0.8)

    def test_savings_rate_is_size_removed_per_second(self):
        rows = [entry("fast", "LZ77", 10.0, 500_000_000), entry("slow", "CM", 1000.0, 100_000_000)]
        rates = an.savings_rates(rows, "comp_ns_per_byte")
        self.assertEqual([r.program for r in rates], ["fast", "slow"])
        self.assertAlmostEqual(rates[0].saved_mb_per_s, 50.0)  # 500 MB removed in 10 s
        self.assertAlmostEqual(rates[0].throughput_mb_per_s, 100.0)
        self.assertAlmostEqual(rates[1].saved_mb_per_s, 0.9)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(LOCAL_FULL)  # 150,000,000 bytes at 2,800 ns/B
            rate = an.local_rate(ltcb.load_local(path)[0], "comp_ns_per_byte")
            self.assertAlmostEqual(rate.saved_mb_per_s, 850 / 2800)
            path.write_text(LOCAL_E8_ONLY)
            self.assertIsNone(an.local_rate(ltcb.load_local(path)[0], "comp_ns_per_byte"))

    def test_total_and_per_gb_rates(self):
        rows = [entry("fast", "LZ77", 10.0, 500_000_000), entry("slow", "CM", 1000.0, 100_000_000)]
        rows[0] = ltcb.Entry(**{**rows[0].__dict__, "decomp_ns_per_byte": 5.0, "mem_mb": 2000.0})
        rates = an.total_rates(rows)
        self.assertEqual(rates[0].program, "fast")
        self.assertAlmostEqual(rates[0].saved_mb_per_s, 500 / 15)  # comp 10 s + decomp 5 s
        self.assertAlmostEqual(rates[0].saved_mb_per_s_per_gb, 500 / 15 / 2.0)
        self.assertAlmostEqual(rates[1].saved_mb_per_s, 900 / 2000)  # decomp == comp in entry()

    def test_weighted_cost_reduces_to_ratio_at_zero_weights_and_reranks_otherwise(self):
        fast = ltcb.Entry(**{**entry("fast", "LZ77", 10.0, 500_000_000).__dict__, "mem_mb": 1.0})
        slow = ltcb.Entry(**{**entry("slow", "CM", 1000.0, 100_000_000).__dict__, "mem_mb": 1000.0})
        self.assertAlmostEqual(an.weighted_cost(fast, 0.0, 0.0), 4.0)
        self.assertAlmostEqual(an.weighted_cost(slow, 0.0, 0.0), 0.8)
        self.assertEqual([r.program for r in an.weighted_ranking([fast, slow], 0.0, 0.0)], ["slow", "fast"])
        # cost = bpb * (2*speed)^alpha * mem^beta: at alpha 0.5 the fast entry wins
        self.assertAlmostEqual(an.weighted_cost(fast, 0.5, 0.0), 4.0 * math.sqrt(20.0))
        self.assertAlmostEqual(an.weighted_cost(slow, 0.5, 0.0), 0.8 * math.sqrt(2000.0))
        self.assertEqual([r.program for r in an.weighted_ranking([fast, slow], 0.5, 0.0)], ["fast", "slow"])
        grid = an.preference_map([fast, slow], alphas=(0.0, 0.5), betas=(0.0, 1.0))
        self.assertEqual([(g.alpha, g.beta, g.program) for g in grid],
                         [(0.0, 0.0, "slow"), (0.0, 1.0, "fast"), (0.5, 0.0, "fast"), (0.5, 1.0, "fast")])

    def test_neighbours_find_the_closest_published_entries(self):
        rows = scaled_entries()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(LOCAL_E8_ONLY.replace("2630", "150").replace(",675,", ",15,"))
            local = ltcb.load_local(path)[0]
        near = an.neighbours(rows, local, k=2)
        self.assertEqual([n.program for n in near], ["p2", "p1"])
        self.assertLess(near[0].distance, near[1].distance)

    def test_analysis_of_the_published_table_is_complete_and_serialisable(self):
        rows = ltcb.load_rows(HERE / "ltcb-main-table.csv")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(LOCAL_E8_ONLY)
            local = ltcb.load_local(path)
        result = an.analyze(rows, local)
        json.dumps(result)  # must be plain data
        self.assertEqual(set(result["families"]), set(ltcb.FAMILIES))
        self.assertGreater(result["scaling"]["slope"], 0)
        self.assertGreater(result["scaling"]["n"], 200)
        self.assertLess(result["correlations"]["enwik9_bpb_vs_log_comp"]["spearman"], 0)
        self.assertEqual(len(result["local"]), 1)
        self.assertIn("predicted_enwik9_bpb", result["local"][0])
        self.assertEqual(len(result["local"][0]["neighbours"]), 5)
        self.assertTrue(any(step["target"] == 1.0 for step in result["cost_ladder"]["compression"]))
        self.assertIn("preference_map", result["efficiency_index"])
        self.assertEqual(len(result["efficiency_index"]["total_time_top"]), 10)

    def test_report_renders_markdown_with_the_sections(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rows = ltcb.load_rows(write_sample(root))
            (root / "local.csv").write_text(LOCAL_FULL)
            local = ltcb.load_local(root / "local.csv")
            text = an.report(an.analyze(rows, local), source="unit test")
        for heading in ("## Families", "## Correlations", "## enwik8 → enwik9 scaling",
                        "## Cost of a bit", "## Size removed per second", "## Weighted efficiency",
                        "## Local entries"):
            self.assertIn(heading, text)
        self.assertIn("ours 1.0", text)

    def test_main_writes_json_and_markdown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            table = write_sample(root)
            (root / "local.csv").write_text(LOCAL_E8_ONLY)
            code = an.main([str(table), "--local", str(root / "local.csv"),
                            "--output", str(root / "out.json")])
            self.assertEqual(code, 0)
            self.assertTrue((root / "out.json").exists())
            self.assertTrue((root / "out.md").exists())
            data = json.loads((root / "out.json").read_text())
            self.assertEqual(data["entries"], 10)


if __name__ == "__main__":
    unittest.main()
