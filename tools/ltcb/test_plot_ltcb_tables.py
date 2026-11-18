"""Tests for the LTCB dashboard: data loading, derived series and rendering.

Source: Claude Opus 5 via Claude Code. The data functions run with the
standard library alone; the render test needs Matplotlib and is skipped
without it, as the codec's default gate is independent of plotting.
"""

import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

import plot_ltcb_tables as ltcb

HERE = ltcb.RESEARCH  # the tracked data lives beside the renderings, not the code
HEADER = (
    "rank,published_label,program,options,enwik8_bytes,enwik9_bytes,"
    "decompressor_zip_bytes,decompressor_flags,enwik9_plus_prog_bytes,"
    "comp_ns_per_byte,comp_approximate,decomp_ns_per_byte,decomp_approximate,"
    "mem_mb,mem_approximate,alg,note,comp_pareto,decomp_pareto,mem_pareto\n"
)
# A small table exercising every optional column: no enwik8, one time only,
# no time at all, no alg, approximate values, page Pareto marks.
SAMPLE = HEADER + (
    "1,top 1.0,top 1.0,,,96849689,0,xd,96849689,190633,0,187200,0,9537,0,Tr,113,1,1,1\n"
    "2,cm-a -x,cm-a,-x,14623723,107963380,281387,sd,108244767,622949,0,638442,0,30950,0,CM,83,0,0,0\n"
    "3,lstm-b,lstm-b,,,107828411,0,xd,107828411,,0,301180,0,10233,0,LSTM,106,0,0,0\n"
    "4,ppm-c -m1700,ppm-c,-m1700,19055092,157007383,42019,x,157049402,3574,0,3600,1,1700,0,PPM,,0,0,0\n"
    "5,bwt-d,bwt-d,,20786794,163884462,74297,xd,163958759,23,0,8,0,5000,0,BWT,96,0,0,0\n"
    "6,lz-e -9,lz-e,-9,36445248,322591995,53981,s,322645976,101,0,17,0,1,0,LZ77,,1,1,0\n"
    "7,rolz-f,rolz-f,,30000000,300000000,1000,x,300001000,50,0,10,0,64,0,ROLZ,,0,0,0\n"
    "8,old-g,old-g,,25203248,219655072,0,xd,219655072,858,0,270,0,76,0,,,0,0,0\n"
    "9,notime-h,notime-h,,40000000,400000000,2000,x,400002000,,0,,0,2000,1,SR,,0,0,0\n"
    "10,o0-i,o0-i,,63862371,648370029,3600,sd,648373629,216,0,164,0,0.2,0,o0,,0,0,0\n"
)


def write_sample(directory: Path, text: str = SAMPLE) -> Path:
    path = directory / "table.csv"
    path.write_text(text)
    return path


class LoadTest(unittest.TestCase):
    def test_sample_rows_are_typed_and_optional_fields_are_none(self):
        with tempfile.TemporaryDirectory() as temporary:
            rows = ltcb.load_rows(write_sample(Path(temporary)))
        self.assertEqual(len(rows), 10)
        top = rows[0]
        self.assertEqual(top.rank, 1)
        self.assertIsNone(top.enwik8_bytes)
        self.assertEqual(top.enwik9_bytes, 96_849_689)
        self.assertEqual(top.comp_ns_per_byte, 190633.0)
        self.assertTrue(top.comp_pareto)
        self.assertIsNone(rows[2].comp_ns_per_byte)
        self.assertEqual(rows[2].decomp_ns_per_byte, 301180.0)
        self.assertIsNone(rows[8].comp_ns_per_byte)
        self.assertIsNone(rows[8].decomp_ns_per_byte)
        self.assertEqual(rows[7].alg, "")
        self.assertEqual(rows[9].mem_mb, 0.2)

    def test_published_csv_loads_completely(self):
        rows = ltcb.load_rows(HERE / "ltcb-main-table.csv")
        self.assertEqual(len(rows), 238)
        self.assertEqual([r.rank for r in rows], list(range(1, 239)))
        self.assertTrue(all(r.enwik9_bytes > 0 and r.enwik9_plus_prog_bytes > 0 for r in rows))

    def test_missing_column_is_a_typed_error(self):
        broken = SAMPLE.replace("enwik9_bytes,", "size9,")
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ltcb.TableError):
                ltcb.load_rows(write_sample(Path(temporary), broken))

    def test_non_numeric_value_is_a_typed_error(self):
        broken = SAMPLE.replace("96849689,0,xd", "lots,0,xd")
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ltcb.TableError):
                ltcb.load_rows(write_sample(Path(temporary), broken))

    def test_rank_order_is_checked(self):
        broken = SAMPLE.replace("\n2,cm-a", "\n3,cm-a", 1)
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ltcb.TableError):
                ltcb.load_rows(write_sample(Path(temporary), broken))


class DerivedTest(unittest.TestCase):
    def test_every_published_alg_maps_to_a_family(self):
        with open(HERE / "ltcb-main-table.csv", newline="") as handle:
            algs = {row["alg"] for row in csv.DictReader(handle)}
        for alg in algs:
            self.assertIn(ltcb.family_of(alg), ltcb.FAMILIES, alg)
        self.assertEqual(ltcb.family_of("CM"), "CM")
        self.assertEqual(ltcb.family_of("CM1"), "CM")
        self.assertEqual(ltcb.family_of("Tr"), "Neural")
        self.assertEqual(ltcb.family_of("LSTM"), "Neural")
        self.assertEqual(ltcb.family_of("ROLZ"), "LZ")
        self.assertEqual(ltcb.family_of("BWTS"), "BWT")
        self.assertEqual(ltcb.family_of(""), "Other")
        self.assertEqual(ltcb.family_of("never-seen"), "Other")

    def test_bits_per_byte(self):
        self.assertAlmostEqual(ltcb.bits_per_byte(125_000_000, 1_000_000_000), 1.0)
        self.assertAlmostEqual(ltcb.bits_per_byte(14_623_723, 100_000_000), 1.16989784)

    def test_frontier_keeps_only_non_dominated_points(self):
        points = [(10.0, 3.0), (100.0, 2.0), (50.0, 2.5), (200.0, 2.5), (1000.0, 1.0), (20.0, 4.0)]
        self.assertEqual(
            ltcb.frontier(points),
            [(10.0, 3.0), (50.0, 2.5), (100.0, 2.0), (1000.0, 1.0)],
        )
        self.assertEqual(ltcb.frontier([]), [])


LOCAL_HEADER = (
    "published_label,program,options,enwik8_bytes,enwik9_bytes,decompressor_zip_bytes,"
    "decompressor_flags,comp_ns_per_byte,decomp_ns_per_byte,mem_mb,alg,timed_on,source\n"
)
LOCAL_E8_ONLY = LOCAL_HEADER + (
    "ours 1.0 --max,ours 1.0,--max,18564510,,89959,s,2630,2704.5,675,CM,enwik8,this repo\n"
)
LOCAL_FULL = LOCAL_HEADER + (
    "ours 1.0 --max,ours 1.0,--max,18564510,150000000,89959,s,2800,2900,675,CM,enwik9,this repo\n"
)


class LocalTest(unittest.TestCase):
    def test_enwik8_only_entry_has_no_total(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(LOCAL_E8_ONLY)
            local = ltcb.load_local(path)
        self.assertEqual(len(local), 1)
        entry = local[0]
        self.assertEqual(entry.enwik8_bytes, 18_564_510)
        self.assertIsNone(entry.enwik9_bytes)
        self.assertIsNone(entry.enwik9_plus_prog_bytes)
        self.assertEqual(entry.decompressor_zip_bytes, 89_959)
        self.assertEqual(entry.family, "CM")
        self.assertEqual(entry.timed_on, "enwik8")

    def test_full_entry_computes_total(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(LOCAL_FULL)
            entry = ltcb.load_local(path)[0]
        self.assertEqual(entry.enwik9_plus_prog_bytes, 150_089_959)
        self.assertAlmostEqual(entry.total_bpb, 1.200719672)

    def test_local_entry_needs_at_least_one_size(self):
        broken = LOCAL_HEADER + "x,x,,,,0,s,,,1,CM,enwik8,here\n"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "local.csv"
            path.write_text(broken)
            with self.assertRaises(ltcb.TableError):
                ltcb.load_local(path)

    def test_would_be_ranks_count_strictly_smaller_published_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            rows = ltcb.load_rows(write_sample(Path(temporary)))
        # SAMPLE enwik8 values: 14623723, 19055092, 20786794, 25203248, ... -> 18,564,510 is 2nd
        self.assertEqual(ltcb.enwik8_rank(rows, 18_564_510), 2)
        self.assertEqual(ltcb.enwik8_rank(rows, 1), 1)
        # totals: 96849689, 107828411, 108244767, 157049402, ... -> 150,089,959 is 4th
        self.assertEqual(ltcb.total_rank(rows, 150_089_959), 4)


class RenderTest(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("matplotlib") is None:
            self.skipTest("matplotlib is not installed")

    def test_sample_renders_svg_and_png_with_kpis(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rows = ltcb.load_rows(write_sample(root))
            outputs = ltcb.render(rows, root / "dash.svg", source="unit test")
            self.assertEqual([p.name for p in outputs], ["dash.svg", "dash.png"])
            for path in outputs:
                self.assertGreater(path.stat().st_size, 1000, path)
            svg = (root / "dash.svg").read_text()
            self.assertIn("top 1.0", svg)  # best total is named in the KPI row
            self.assertIn("cm-a", svg)  # best enwik8 is named in the KPI row
            self.assertIn("unit test", svg)

    def test_local_entries_are_drawn_and_named(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rows = ltcb.load_rows(write_sample(root))
            for text in (LOCAL_E8_ONLY, LOCAL_FULL):
                (root / "local.csv").write_text(text)
                local = ltcb.load_local(root / "local.csv")
                ltcb.render(rows, root / "dash.svg", source="unit test", local=local)
                svg = (root / "dash.svg").read_text()
                self.assertIn("ours 1.0", svg)
                self.assertIn("would rank", svg)

    def test_main_writes_next_to_requested_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            table = write_sample(root)
            code = ltcb.main([str(table), "--output", str(root / "out.svg")])
            self.assertEqual(code, 0)
            self.assertTrue((root / "out.svg").exists())
            self.assertTrue((root / "out.png").exists())

    def test_main_rejects_non_svg_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            table = write_sample(root)
            with self.assertRaises(SystemExit):
                ltcb.main([str(table), "--output", str(root / "out.pdf")])


class SmokeTest(unittest.TestCase):
    def test_module_docstring_names_the_source(self):
        self.assertIn("Source:", ltcb.__doc__)


if __name__ == "__main__":
    unittest.main()
