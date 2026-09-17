"""Hold the codec harness to its contract with stub codecs.

The harness's job is to measure the same way for every codec and to refuse a
ratio from one that did not restore its input, so both are tested here against
stubs rather than against a real compressor.

Authored with Claude Opus 5 (Claude Code), 2026-09-17.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "bench"))

import codec_bench  # noqa: E402

HONEST = """#!/usr/bin/env python3
import sys, zlib
mode, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
open(dst, 'wb').write(zlib.compress(data, 9) if mode == 'c' else zlib.decompress(data))
"""

LOSSY = """#!/usr/bin/env python3
import sys
mode, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
open(dst, 'wb').write(data[:-1] if mode == 'c' else data)
"""

BROKEN = "#!/bin/sh\nexit 4\n"


def stub_codec(name: str, script: Path) -> dict:
    return {
        "name": name,
        "tool": str(script),
        "encode": [sys.executable, str(script), "c", "{in}", "{enc}"],
        "decode": [sys.executable, str(script), "d", "{enc}", "{dec}"],
    }


class CodecBenchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.scratch = Path(cls.temporary.name)
        cls.source = cls.scratch / "source"
        cls.source.write_bytes(b"the quick brown fox. " * 5000)
        cls.scripts = {}
        for name, body in (("honest", HONEST), ("lossy", LOSSY), ("broken", BROKEN)):
            path = cls.scratch / f"{name}.py"
            path.write_text(body)
            path.chmod(0o755)
            cls.scripts[name] = path

    def test_a_codec_that_restores_is_measured(self):
        row = codec_bench.measure(stub_codec("honest", self.scripts["honest"]), self.source, self.scratch)
        self.assertTrue(row.restored)
        self.assertEqual(row.input_bytes, self.source.stat().st_size)
        self.assertLess(row.output_bytes, row.input_bytes)
        self.assertAlmostEqual(row.ratio, row.output_bytes / row.input_bytes, places=5)
        self.assertGreaterEqual(row.encode_seconds, 0.0)
        self.assertGreater(row.encode_rss_mib, 0.0)

    def test_a_codec_that_does_not_restore_is_marked_not_restored(self):
        """A ratio from a codec that loses a byte is not a number."""
        row = codec_bench.measure(stub_codec("lossy", self.scripts["lossy"]), self.source, self.scratch)
        self.assertFalse(row.restored)

    def test_a_failing_codec_raises_rather_than_scoring_zero(self):
        with self.assertRaises(codec_bench.BenchError):
            codec_bench.measure(stub_codec("broken", self.scripts["broken"]), self.source, self.scratch)

    def test_the_run_fails_when_any_codec_did_not_restore(self):
        """The harness must not print a table and exit 0 over a broken row."""
        row = codec_bench.Row("lossy", 10, 5, 0.5, 0.1, 0.1, 1.0, 1.0, False)
        self.assertIn("NO", codec_bench.render([row]))

    def test_rows_render_smallest_first(self):
        big = codec_bench.Row("big", 100, 90, 0.9, 1.0, 1.0, 1.0, 1.0, True)
        small = codec_bench.Row("small", 100, 10, 0.1, 1.0, 1.0, 1.0, 1.0, True)
        lines = codec_bench.render([big, small]).splitlines()
        self.assertTrue(lines[1].startswith("small"))

    def test_a_missing_input_is_refused(self):
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/codec_bench.py"), "--input", str(self.scratch / "absent")],
            capture_output=True)
        self.assertEqual(done.returncode, 1)
        self.assertIn("no input", done.stderr.decode())

    def test_an_unknown_codec_name_is_refused(self):
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/codec_bench.py"), "--input", str(self.source),
             "--only", "nosuchcodec"], capture_output=True)
        self.assertEqual(done.returncode, 1)
        self.assertIn("not available here", done.stderr.decode())

    @unittest.skipUnless((ROOT / "adpat/c/build/cadapt").exists(), "needs make")
    def test_the_real_codec_runs_end_to_end(self):
        out = self.scratch / "row.json"
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/codec_bench.py"), "--input", str(self.source),
             "--only", "cadapt-fast", "--json", str(out), "--scratch", str(self.scratch)],
            capture_output=True)
        self.assertEqual(done.returncode, 0, done.stderr.decode())
        rows = json.loads(out.read_text())["rows"]
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]["restored"])


if __name__ == "__main__":
    unittest.main()
