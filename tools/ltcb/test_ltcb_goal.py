"""Tests for the LTCB goal figures.

Source: Claude Opus 5 via Claude Code, 2026-09-16. The goal is computed from
the snapshot, never typed: a small table exercises the arithmetic (the
threshold is the twentieth total, the enwik9 target that total less the
entry's own decompressor less one byte, the memory claim the least memory
of every entry ranked above); the published snapshot is held to the
figures the documents quote.
"""

import csv
import io
import tempfile
import unittest
from pathlib import Path

import ltcb_goal as goal

HEADER = (
    "rank,published_label,program,options,enwik8_bytes,enwik9_bytes,"
    "decompressor_zip_bytes,decompressor_flags,enwik9_plus_prog_bytes,"
    "comp_ns_per_byte,comp_approximate,decomp_ns_per_byte,decomp_approximate,"
    "mem_mb,mem_approximate,alg,note,comp_pareto,decomp_pareto,mem_pareto\n"
)


def table(entries: list[tuple[str, int, int, str]]) -> str:
    """rows of (program, total, zip, mem) ranked in order."""
    lines = [HEADER]
    for rank, (program, total, zip_bytes, mem) in enumerate(entries, start=1):
        lines.append(f"{rank},{program},{program},,,{total - zip_bytes},{zip_bytes},x,{total},10,0,10,0,{mem},0,CM,1,0,0,0\n")
    return "".join(lines)


class GoalTest(unittest.TestCase):
    def write(self, text: str) -> Path:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        path = Path(scratch.name) / "table.csv"
        path.write_text(text, encoding="utf-8")
        return path

    def test_threshold_target_and_memory_from_a_small_table(self):
        entries = [(f"p{i}", 100_000_000 + i * 1_000_000, 1000, str(2000 + i)) for i in range(1, 25)]
        entries.append(("cadapt 0.12.0", 150_000_000, 209_893, "661"))
        g = goal.compute(self.write(table(entries)), program="cadapt 0.12.0", place=20)
        self.assertEqual(g.rank, 25)
        self.assertEqual(g.entries, 25)
        self.assertEqual(g.total, 150_000_000)
        self.assertEqual(g.decompressor_bytes, 209_893)
        self.assertEqual(g.place, 20)
        self.assertEqual(g.threshold_total, 120_000_000)  # the twentieth entry's total
        self.assertEqual(g.threshold_program, "p20")
        self.assertEqual(g.target_enwik9, 120_000_000 - 209_893 - 1)
        self.assertEqual(g.memory_mb, 661.0)
        self.assertEqual(g.least_memory_above, (2001.0, "p1"))
        self.assertTrue(g.leads_memory_above)

    def test_memory_claim_fails_when_an_entry_above_is_smaller(self):
        entries = [("tiny", 100_000_000, 10, "4"), ("cadapt 0.12.0", 150_000_000, 209_893, "661")]
        g = goal.compute(self.write(table(entries)), program="cadapt 0.12.0", place=1)
        self.assertEqual(g.least_memory_above, (4.0, "tiny"))
        self.assertFalse(g.leads_memory_above)
        self.assertEqual(g.threshold_total, 100_000_000)

    def test_a_missing_program_or_place_is_a_typed_error(self):
        path = self.write(table([("only", 100_000_000, 10, "4")]))
        with self.assertRaises(goal.GoalError):
            goal.compute(path, program="cadapt 0.12.0", place=1)
        with self.assertRaises(goal.GoalError):
            goal.compute(path, program="only", place=2)

    def test_report_names_every_figure(self):
        entries = [(f"p{i}", 100_000_000 + i * 1_000_000, 1000, "3000") for i in range(1, 21)]
        entries.append(("cadapt 0.12.0", 150_000_000, 209_893, "661"))
        out = io.StringIO()
        code = goal.main(["--table", str(self.write(table(entries))), "--program", "cadapt 0.12.0"], out)
        self.assertEqual(code, 0)
        text = out.getvalue()
        for figure in ("rank 21 of 21", "150,000,000", "120,000,000", "119,790,106", "661 MB", "3,000 MB", "p1"):
            self.assertIn(figure, text)

    def test_published_snapshot_gives_the_documented_figures(self):
        g = goal.compute(goal.TABLE, program="cadapt 0.12.0", place=20)
        self.assertEqual((g.rank, g.entries, g.total, g.decompressor_bytes), (30, 226, 150_273_521, 209_893))
        self.assertEqual((g.threshold_program, g.threshold_total), ("paq8px_v206fix1", 125_099_359))
        self.assertEqual(g.target_enwik9, 124_889_465)
        self.assertEqual((g.memory_mb, g.least_memory_above), (661.0, (1542.0, "drt|lpaq9m")))
        self.assertTrue(g.leads_memory_above)


if __name__ == "__main__":
    unittest.main()
