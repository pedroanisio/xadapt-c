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
if cfg.get("trace"):
    with open(cfg["trace"], "a") as f:
        f.write(os.path.basename(sys.argv[0]) + " " + args[0] + "\n")
time.sleep(cfg.get("sleep", 0))
end = time.process_time() + cfg.get("busy", 0)
while time.process_time() < end:
    pass
if args[0] == "encode":
    src, out = args[-3], args[-1]
    shutil.copy(src, out + ".src")
    with open(out, "wb") as f:
        f.write((os.urandom(1) if cfg.get("vary") else b"x") * cfg["bytes"])
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
        if "cpu_seconds" in entry:
            entry.pop("wall_seconds", None)
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

    def test_a_cpu_floor_is_blind_to_waiting_and_sees_work(self):
        """A floor in CPU seconds is what the coder does, not what the host
        was doing meanwhile: a run that waits (as one does on a loaded
        host) passes it where the same run fails a wall floor; a run that
        burns the CPU fails it."""
        size = self.fixture.stat().st_size
        self.configure(self.binary, bytes=1, sleep=1.0)
        self.write_floor(entry={"cpu_seconds": 0.5, "min_bytes_per_second": size / 0.5})
        code, text = self.run_check()
        self.assertEqual(code, 0, text)
        self.assertIn("cpu", text)
        self.write_floor(entry={"wall_seconds": 0.5, "min_bytes_per_second": size / 0.5})
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("wall", text)
        self.configure(self.binary, bytes=1, busy=0.8)
        self.write_floor(entry={"cpu_seconds": 0.1, "min_bytes_per_second": size / 0.1})
        code, text = self.run_check()
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL", text)
        self.assertIn("throughput", text)

    def test_runs_take_the_median_and_must_agree_byte_for_byte(self):
        self.configure(self.binary, bytes=1)
        code, text = self.run_check("--runs", "3")
        self.assertEqual(code, 0, text)
        self.assertIn("median of 3 runs", text)
        self.assertRegex(text, r"runs: [0-9.]+, [0-9.]+, [0-9.]+ s")
        # RUN-5: the same command writes the same bytes every run, or the check fails
        self.configure(self.binary, bytes=1, vary=True)
        code, text = self.run_check("--runs", "3")
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL", text)
        self.assertIn("differ", text)
        # the baseline binary is measured the same way, the two taking turns;
        # each spins a fixed CPU time, so on a CPU floor the two agree
        # however busy the host is (a wall clock would not, under load)
        trace = self.root / "trace.log"
        size = self.fixture.stat().st_size
        self.write_floor(entry={"cpu_seconds": 100.0, "min_bytes_per_second": size / 100.0})
        self.configure(self.binary, bytes=1, busy=0.3, trace=str(trace))
        self.configure(self.baseline, bytes=1, busy=0.3, trace=str(trace))
        code, text = self.run_check("--runs", "3", "--baseline-binary", str(self.baseline))
        self.assertEqual(code, 0, text)
        self.assertRegex(text, r"median of 3 runs.*vs baseline binary.*median of 3 runs")
        self.assertEqual(trace.read_text().split("\n")[:-1],
                         ["fake-cadapt encode", "fake-baseline encode"] * 3)

    def test_a_floor_names_exactly_one_measure(self):
        self.write_floor()
        data = json.loads(self.floor.read_text())
        entry = data["floors"][0]
        for keys in ({"cpu_seconds": 1.0, "wall_seconds": 1.0}, {}):
            trial = {k: v for k, v in entry.items() if k != "wall_seconds"}
            trial.update(keys)
            self.floor.write_text(json.dumps({**data, "floors": [trial]}))
            with self.assertRaises(floor.FloorError, msg=sorted(keys)):
                floor.load_floor(self.floor)
        for key, measure in (("cpu_seconds", "cpu"), ("wall_seconds", "wall")):
            trial = {k: v for k, v in entry.items() if k != "wall_seconds"}
            trial[key] = 2.5
            self.floor.write_text(json.dumps({**data, "floors": [trial]}))
            loaded = floor.load_floor(self.floor).floors[0]
            self.assertEqual((loaded.measure, loaded.seconds), (measure, 2.5))

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

    def test_corpus_dir_resolves_every_floor_by_name(self):
        import contextlib
        import io

        corpus = self.root / "mixed"
        (corpus / "silesia").mkdir(parents=True)
        for name in ("a", "b"):
            (corpus / "silesia" / name).write_bytes(self.fixture.read_bytes())
        entry = {"input_bytes": self.fixture.stat().st_size, "input_md5": self.md5, "max_container_bytes": 1000,
                 "wall_seconds": 100.0, "min_bytes_per_second": self.fixture.stat().st_size / 100.0,
                 "measured_with": "fake"}
        data = {"schema": floor.SCHEMA, "locked_on": "2026-09-14", "host": "test host", "throughput_tolerance": 0.10,
                "arguments": ["encode"], "floors": [dict(entry, fixture="silesia/a"), dict(entry, fixture="silesia/b")]}
        self.floor.write_text(json.dumps(data))
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = floor.main(["--binary", str(self.binary), "--floor", str(self.floor), "--corpus", str(corpus),
                               "--timeout", "5", "--output-dir", str(self.root / "out")])
        text = out.getvalue()
        self.assertEqual(code, 0, text)
        self.assertIn("PASS silesia/a size", text)
        self.assertIn("PASS silesia/b size", text)
        # a missing file is a setup error, not a pass
        (corpus / "silesia" / "b").unlink()
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = floor.main(["--binary", str(self.binary), "--floor", str(self.floor), "--corpus", str(corpus),
                               "--timeout", "5", "--output-dir", str(self.root / "out")])
        self.assertEqual(code, 2, out.getvalue())
        self.assertIn("silesia/b", out.getvalue())

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
        self.assertEqual(by_name["enwik8-8MiB"].max_container_bytes, 1_669_716)
        self.assertEqual(by_name["enwik8-8MiB"].input_bytes, 8 * 1024 * 1024)
        self.assertEqual(by_name["enwik8-16MiB"].max_container_bytes, 3_236_535)
        self.assertEqual(by_name["enwik8"].max_container_bytes, 17_800_141)
        self.assertEqual(by_name["enwik9"].max_container_bytes, 145_784_134)
        self.assertEqual(by_name["enwik8"].input_md5, "a1fa5ffddb56f4953e226637dabbb36a")
        self.assertEqual(by_name["enwik9"].input_md5, "e206c3450ac99950df65bf70ef61a12d")
        # the operator's baseline of 2026-09-16: the lean context set (LOCKS.md)
        self.assertEqual(locked.arguments, ["encode", "--max", "--allocation", "context", "--contexts", "lean"])
        # re-locked 2026-09-16 on that command, a quiet host: CPU seconds, the
        # median of three runs on the enwik8 rows; the sizes and enwik9's
        # seconds re-locked the same day on the keyed-arbiters binary
        self.assertEqual({f.measure for f in locked.floors}, {"cpu"})
        self.assertEqual(by_name["enwik8-8MiB"].seconds, 10.7)
        self.assertEqual(by_name["enwik8-16MiB"].seconds, 19.8)
        self.assertEqual(by_name["enwik8"].seconds, 107.4)
        self.assertEqual(by_name["enwik9"].seconds, 1054.0)
        self.assertAlmostEqual(by_name["enwik8-8MiB"].min_bytes_per_second, 8 * 1024 * 1024 / 10.7, places=3)
        self.assertAlmostEqual(by_name["enwik8-16MiB"].min_bytes_per_second, 16 * 1024 * 1024 / 19.8, places=3)
        self.assertAlmostEqual(by_name["enwik8"].min_bytes_per_second, 100_000_000 / 107.4, places=3)
        self.assertAlmostEqual(by_name["enwik9"].min_bytes_per_second, 1_000_000_000 / 1054.0, places=3)
        self.assertEqual(locked.throughput_tolerance, 0.10)


if __name__ == "__main__":
    unittest.main()
