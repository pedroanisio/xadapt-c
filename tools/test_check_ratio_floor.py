"""Tests for the locked-ratio floor checker.

Source: Claude Opus 5 via Claude Code. A fake encoder stands in for cadapt so
every outcome (within the floor, over it, wrong input, bad decode, timeout,
malformed floor file) is exercised in milliseconds.
"""

from __future__ import annotations

import hashlib
import json
import stat
import tempfile
import unittest
from pathlib import Path

import check_ratio_floor as floor

FAKE = r'''#!/usr/bin/env python3
import json, os, shutil, sys, time
cfg = json.load(open(sys.argv[0] + ".cfg"))
args = sys.argv[1:]
time.sleep(cfg.get("sleep", 0))
if args[0] == "encode":
    src, out = args[-3], args[-1]
    shutil.copy(src, out + ".src")
    with open(out, "wb") as f:
        f.write(b"x" * cfg["bytes"])
elif args[0] == "decode":
    src, out = args[1], args[-1]
    shutil.copy(src + ".src", out)
    if cfg.get("corrupt"):
        with open(out, "r+b") as f:
            f.seek(0); f.write(b"\xff")
else:
    sys.exit(3)
'''


class FloorCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.binary = self.make_binary("fake-cadapt")
        self.baseline = self.make_binary("fake-baseline")
        self.fixture = self.root / "corpus.bin"
        self.fixture.write_bytes(b"the quick brown fox " * 500)
        self.md5 = hashlib.md5(self.fixture.read_bytes()).hexdigest()
        self.floor = self.root / "floor.json"
        self.write_floor()

    def make_binary(self, name: str) -> Path:
        path = self.root / name
        path.write_text(FAKE)
        path.chmod(path.stat().st_mode | stat.S_IEXEC)
        self.configure(path, bytes=1)
        return path

    def configure(self, path: Path, **cfg) -> None:
        (self.root / (path.name + ".cfg")).write_text(json.dumps(cfg))

    def write_floor(self, **override) -> None:
        entry = {"fixture": "corpus", "input_bytes": self.fixture.stat().st_size, "input_md5": self.md5,
                 "max_container_bytes": 1000, "wall_seconds": 100.0,
                 "min_bytes_per_second": self.fixture.stat().st_size / 100.0, "measured_with": "fake"}
        entry.update(override.pop("entry", {}))
        data = {"schema": floor.SCHEMA, "locked_on": "2026-09-14", "host": floor.host_model() or "test host",
                "throughput_tolerance": 0.10,
                "arguments": ["encode", "--max", "--allocation", "context"], "floors": [entry]}
        data.update(override)
        self.floor.write_text(json.dumps(data))

    def run_check(self, *extra: str) -> tuple[int, str]:
        import contextlib
        import io

        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = floor.main(["--binary", str(self.binary), "--floor", str(self.floor),
                               "--fixture", f"corpus={self.fixture}", "--timeout", "5",
                               "--output-dir", str(self.root / "out"), *extra])
        return code, out.getvalue()

    def test_at_or_under_the_floor_passes(self):
        for size in (1000, 999, 1):
            self.configure(self.binary, bytes=size)
            code, text = self.run_check()
            self.assertEqual(code, 0, text)
            self.assertIn("PASS", text)
            self.assertIn(f"{size:,} B", text)

    def test_over_the_floor_fails_and_names_the_excess(self):
        self.configure(self.binary, bytes=1001)
        code, text = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("FAIL", text)
        self.assertIn("+1 B", text)

    def test_throughput_below_the_tolerated_floor_fails(self):
        # The fake takes ~2 s for 10,000 bytes; lock a floor it cannot meet, then one it can.
        self.configure(self.binary, bytes=1, sleep=1.5)
        self.write_floor(entry={"wall_seconds": 0.01, "min_bytes_per_second": self.fixture.stat().st_size / 0.01})
        code, text = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("throughput", text)
        self.assertIn("FAIL", text)
        self.write_floor(entry={"wall_seconds": 60.0, "min_bytes_per_second": self.fixture.stat().st_size / 60.0})
        code, text = self.run_check()
        self.assertEqual(code, 0, text)
        self.assertIn("B/s", text)

    def test_other_host_reports_throughput_unless_a_baseline_gates_it(self):
        self.configure(self.binary, bytes=1, sleep=0.6)
        self.write_floor(host="some other machine",
                         entry={"wall_seconds": 0.01, "min_bytes_per_second": self.fixture.stat().st_size / 0.01})
        code, text = self.run_check()
        self.assertEqual(code, 0, text)  # size holds; the impossible time floor is not applied here
        self.assertIn("not gated on this host", text)
        self.configure(self.baseline, bytes=1, sleep=0.0)  # baseline is much faster: relative gate fails
        code, text = self.run_check("--baseline-binary", str(self.baseline))
        self.assertEqual(code, 1)
        self.assertIn("vs baseline binary", text)
        self.assertIn("FAIL", text)
        self.configure(self.baseline, bytes=1, sleep=1.2)  # baseline slower: relative gate passes
        code, text = self.run_check("--baseline-binary", str(self.baseline))
        self.assertEqual(code, 0, text)

    def test_verify_round_trip_passes_and_corruption_fails(self):
        self.configure(self.binary, bytes=500)
        code, text = self.run_check("--verify")
        self.assertEqual(code, 0, text)
        self.assertIn("round trip verified", text)
        self.configure(self.binary, bytes=500, corrupt=True)
        code, text = self.run_check("--verify")
        self.assertEqual(code, 1)
        self.assertIn("differs", text)

    def test_wrong_input_is_a_setup_error_not_a_pass(self):
        self.fixture.write_bytes(b"different content")
        code, text = self.run_check()
        self.assertEqual(code, 2)
        self.assertIn("does not match", text)

    def test_timeout_is_reported(self):
        self.configure(self.binary, bytes=1, sleep=3)
        code, text = self.run_check("--timeout", "0.3")
        self.assertEqual(code, 2)
        self.assertIn("timed out", text)

    def test_unknown_fixture_name_is_rejected(self):
        import contextlib
        import io

        out = io.StringIO()
        with contextlib.redirect_stderr(out), self.assertRaises(SystemExit):
            floor.main(["--binary", str(self.binary), "--floor", str(self.floor),
                        "--fixture", f"other={self.fixture}"])
        self.assertIn("other", out.getvalue())

    def test_malformed_floor_files_are_typed_errors(self):
        cases = [
            dict(schema="cadapt.ratio-floor.v0"),
            dict(extra="field"),
            dict(entry={"max_container_bytes": 0}),
            dict(entry={"input_md5": "not-a-digest"}),
            dict(entry={"surprise": 1}),
            dict(entry={"wall_seconds": 0}),
            dict(entry={"min_bytes_per_second": -1}),
            dict(throughput_tolerance=1.5),
            dict(arguments=["--max"]),
            dict(floors=[]),
        ]
        for case in cases:
            self.write_floor(**case)
            with self.assertRaises(floor.FloorError, msg=str(case)):
                floor.load_floor(self.floor)
        self.floor.write_text("{not json")
        with self.assertRaises(floor.FloorError):
            floor.load_floor(self.floor)

    def test_repository_floor_file_is_valid_and_matches_the_locked_numbers(self):
        locked = floor.load_floor(floor.ROOT / "tools" / "ratio_floor.json")
        by_name = {f.fixture: f for f in locked.floors}
        self.assertEqual(sorted(by_name), ["enwik8", "enwik8-16MiB", "enwik8-8MiB", "enwik9"])
        self.assertEqual(by_name["enwik8-8MiB"].max_container_bytes, 1_716_715)
        self.assertEqual(by_name["enwik8-8MiB"].input_bytes, 8 * 1024 * 1024)
        self.assertEqual(by_name["enwik8-16MiB"].max_container_bytes, 3_340_447)
        self.assertEqual(by_name["enwik8"].max_container_bytes, 18_564_510)
        self.assertEqual(by_name["enwik9"].max_container_bytes, 155_906_798)
        self.assertEqual(by_name["enwik8"].input_md5, "a1fa5ffddb56f4953e226637dabbb36a")
        self.assertEqual(by_name["enwik9"].input_md5, "e206c3450ac99950df65bf70ef61a12d")
        self.assertEqual(locked.arguments, ["encode", "--max", "--allocation", "context"])
        # raised 2026-09-14 with the per-bit path (LOCKS.md); enwik9 not re-measured
        self.assertEqual(by_name["enwik8-8MiB"].wall_seconds, 17.9)
        self.assertEqual(by_name["enwik8-16MiB"].wall_seconds, 33.8)
        self.assertEqual(by_name["enwik8"].wall_seconds, 189.4)
        self.assertEqual(by_name["enwik9"].wall_seconds, 3123.0)
        self.assertAlmostEqual(by_name["enwik8-8MiB"].min_bytes_per_second, 8 * 1024 * 1024 / 17.9, places=3)
        self.assertAlmostEqual(by_name["enwik8-16MiB"].min_bytes_per_second, 16 * 1024 * 1024 / 33.8, places=3)
        self.assertAlmostEqual(by_name["enwik8"].min_bytes_per_second, 100_000_000 / 189.4, places=3)
        self.assertAlmostEqual(by_name["enwik9"].min_bytes_per_second, 1_000_000_000 / 3123, places=3)
        self.assertEqual(locked.throughput_tolerance, 0.10)


if __name__ == "__main__":
    unittest.main()
