"""Tests for the LTCB main-table transcriber.

Source: Claude Opus 5 via Claude Code, 2026-09-15. A synthetic page carries
every shape the rules cover — no enwik8, a zip of 0, no flag letters, one
time only in either column, two figures run together, `~` values, Pareto
underlines, no algorithm, an entity in a label, the three labels that do
not split — and every row that breaks a rule is a typed error, never a
skipped row. The published snapshot is checked for what the transcription
of 2026-11-18 established: 238 rows, cadapt at rank 41.
"""

import csv
import tempfile
import unittest
from pathlib import Path

import transcribe_ltcb_table as tr

HEADER = (
    "                Compression                      Compressed size      Decompresser  <b>Total size</b>   Time (ns/byte)\n"
    "<a href=#about>Program</a>           Options                       enwik8      enwik9     size (zip)   enwik9+prog  Comp Decomp   Mem Alg <a href=#notes>Note</a>\n"
    "-------           -------                     ----------  -----------  -----------  -----------  ----- -----   --- --- ----\n"
)
ROWS = (
    # no enwik8, zip 0 with xd, every figure Pareto
    "<li><a href=#1>zmix 1.0</a>                                                   96,096,261          0 xd  96,096,261 <u>131364</u> <u>131184</u>  <u>9370</u> Tr  115\n"
    # only a Decomp time (it starts under the Decomp header)
    "<li><a href=#2>cmix-obias</a> 18 Aug 2026                                    108,009,834          0 xd 108,009,834        <u>119920</u>  9836 LSTM  108\n"
    # only a Comp time; approximate memory; label with an entity and options
    "<li><a href=#3>cm&amp;m</a> 2.0 -x11                                 18,233,295  144,854,575     79,574 s  144,934,149    394         ~5961 CM   72\n"
    # two figures run together, the second approximate
    "<li><a href=#4>ocamyd LTCB 1.0</a>   -s0 -m3                     21,285,121  182,359,986     21,030 x  182,381,016 108960~110000 300 DMC   6\n"
    # no flag letters, no note, fractional memory
    "<li><a href=#5>NTFS</a>              LZNT1                       76,955,648  636,870,656          0    636,870,656     10     9  0.1 LZ77 26\n"
    # no algorithm and no note, memory last
    "<li><a href=#6>WinACE</a>            -sfx -m5 -d4096             29,481,470  257,237,710          0 xd 257,237,710   1080    77    4\n"
    # the three labels that do not split, and one that splits on the first -token\n"
    "<li><a href=#7>m03exp</a> 2005-02-15 32MB blocks                 21,948,192  191,250,500     44,593 x  191,295,093  ~4800 ~2100  256 BWT\n"
    "<li><a href=#8>libzling</a> 20160107 e4                          30,262,436  268,873,795     45,116 s  268,918,911    105    56   14 ROLZ 90\n"
    "<li><a href=#9>KuaiZip</a> 2.3.2 x86                             31,013,296  277,150,955    148,014 x  277,298,969     16     4  ~70 LZ77 26\n"
    "<li><a href=#10>longname-that-ran-into-options</a> -9 -m2        25,002,070  202,237,199    191,600 s  202,428,799   1096   6.6 7800 LZ77 70\n"
    # no time at all\n"
    "<li><a href=#11>arb2x</a> v20060602                               99,642,909  995,674,993      3,433 sd 995,678,426              1.6 o0b\n"
)


def page(rows: str = ROWS) -> str:
    return "<html>Last update: Sept 15, 2026<pre><ol>\n" + HEADER + rows + "</ol></pre></html>"


class TranscribeTest(unittest.TestCase):
    def rows(self, text: str = ROWS) -> list[dict]:
        return tr.transcribe(page(text))

    def test_every_shape_is_read(self):
        rows = self.rows()
        self.assertEqual([r["rank"] for r in rows], [str(i) for i in range(1, 12)])
        by = {r["program"]: r for r in rows}
        z = by["zmix 1.0"]
        self.assertEqual((z["enwik8_bytes"], z["enwik9_bytes"], z["decompressor_zip_bytes"], z["decompressor_flags"]),
                         ("", "96096261", "0", "xd"))
        self.assertEqual((z["comp_ns_per_byte"], z["decomp_ns_per_byte"], z["mem_mb"]), ("131364", "131184", "9370"))
        self.assertEqual((z["comp_pareto"], z["decomp_pareto"], z["mem_pareto"]), ("1", "1", "1"))
        self.assertEqual((z["alg"], z["note"]), ("Tr", "115"))
        c = by["cmix-obias 18 Aug 2026"]
        self.assertEqual((c["comp_ns_per_byte"], c["decomp_ns_per_byte"], c["decomp_pareto"]), ("", "119920", "1"))
        m = by["cm&m 2.0"]
        self.assertEqual((m["options"], m["comp_ns_per_byte"], m["decomp_ns_per_byte"]), ("-x11", "394", ""))
        self.assertEqual((m["mem_mb"], m["mem_approximate"]), ("5961", "1"))
        o = by["ocamyd LTCB 1.0"]
        self.assertEqual((o["comp_ns_per_byte"], o["comp_approximate"], o["decomp_ns_per_byte"], o["decomp_approximate"]),
                         ("108960", "0", "110000", "1"))
        n = by["NTFS"]
        self.assertEqual((n["options"], n["decompressor_zip_bytes"], n["decompressor_flags"], n["mem_mb"], n["note"]),
                         ("LZNT1", "0", "", "0.1", "26"))
        w = by["WinACE"]
        self.assertEqual((w["alg"], w["note"], w["mem_mb"]), ("", "", "4"))
        a = by["arb2x v20060602"]
        self.assertEqual((a["comp_ns_per_byte"], a["decomp_ns_per_byte"], a["mem_mb"], a["alg"]), ("", "", "1.6", "o0b"))
        self.assertEqual(rows[6]["comp_approximate"], "1")

    def test_labels_split_by_the_rules_and_never_lose_text(self):
        by = {r["published_label"]: r for r in self.rows()}
        for whole in ("m03exp 2005-02-15 32MB blocks", "libzling 20160107 e4", "KuaiZip 2.3.2 x86"):
            self.assertEqual((by[whole]["program"], by[whole]["options"]), (whole, ""))
        long = by["longname-that-ran-into-options -9 -m2"]
        self.assertEqual((long["program"], long["options"]), ("longname-that-ran-into-options", "-9 -m2"))
        for row in by.values():
            self.assertEqual((row["program"] + " " + row["options"]).strip(), row["published_label"])

    def test_a_row_the_rules_do_not_cover_is_an_error(self):
        bad = ROWS + "<li><a href=#12>odd</a> 1.0   12,345 x 1,234,567  10 10 5 CM 1\n"  # no enwik9-sized figure
        with self.assertRaises(tr.TranscriptionError):
            self.rows(bad)
        three = ROWS + "<li><a href=#13>odd</a> 1.0  18,233,295  144,854,575  79,574 s  144,934,149  1 2 3 4 CM 72\n"
        with self.assertRaises(tr.TranscriptionError):
            self.rows(three)
        with self.assertRaises(tr.TranscriptionError):
            tr.transcribe("<html>no table here</html>")

    def test_page_date_and_csv_round_trip(self):
        self.assertEqual(tr.page_date(page()), "Sept 15, 2026")
        with tempfile.TemporaryDirectory() as scratch:
            html = Path(scratch) / "page.html"
            out = Path(scratch) / "table.csv"
            html.write_text(page(), encoding="utf-8")
            self.assertEqual(tr.main(["--html", str(html), "--output", str(out)]), 0)
            with open(out, newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(list(rows[0]), tr.COLUMNS)
            self.assertEqual(rows, self.rows())
            self.assertEqual(tr.main(["--html", str(html / "missing"), "--output", str(out)]), 2)

    def test_a_time_below_one_nanosecond_is_read_without_its_zero(self):
        """The page prints a decompression time under 1 ns/byte as `.746`."""
        self.assertIsNotNone(tr.NUMBER.match(".746"))
        self.assertIsNotNone(tr.NUMBER.match("~.5"))
        self.assertIsNotNone(tr.NUMBER.match("9.4"))
        for token in (".", "1.", "x.5", "..5"):
            self.assertIsNone(tr.NUMBER.match(token), token)

    def test_published_snapshot_is_the_page_of_2026_11_17(self):
        snapshot = Path(__file__).resolve().parents[2] / "research" / "ltcb-main-table.csv"
        with open(snapshot, newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        self.assertEqual(len(rows), 238)
        fastest = [r for r in rows if r["program"] == "nakamichi washigan+++"]
        self.assertEqual([r["decomp_ns_per_byte"] for r in fastest], [".746"])
        cadapt = [r for r in rows if r["program"] == "cadapt 0.12.0"]
        self.assertEqual(len(cadapt), 1)
        self.assertEqual((cadapt[0]["rank"], cadapt[0]["enwik8_bytes"], cadapt[0]["enwik9_bytes"],
                          cadapt[0]["decompressor_zip_bytes"], cadapt[0]["enwik9_plus_prog_bytes"], cadapt[0]["note"]),
                         ("41", "18207203", "150063628", "209893", "150273521", "114"))


if __name__ == "__main__":
    unittest.main()
