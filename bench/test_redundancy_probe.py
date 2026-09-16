"""Hold the redundancy probe to inputs whose answer is known in advance.

Run after make. The test compiles the probe into a private directory; an
optional positional argument selects an already built probe instead.

Authored with Claude Opus 5 (Claude Code), 2026-09-16.
"""

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROBE: Path | None = None


def parse(output: str) -> dict[tuple[str, int, int], dict[str, float]]:
    """Read the probe's table into {(pass, a, b): {column: value}}."""
    rows: dict[tuple[str, int, int], dict[str, float]] = {}
    header: list[str] = []
    for line in output.splitlines():
        fields = line.split()
        if not fields or fields[0] == "input":
            continue
        if fields[0] == "pass":
            header = fields[3:]
            continue
        if fields[0] not in ("lzp", "gate"):
            raise AssertionError(f"unexpected row: {line}")
        key = (fields[0], int(fields[1]), int(fields[2]))
        rows[key] = dict(zip(header, (float(value) for value in fields[3:])))
    if not rows:
        raise AssertionError("the probe printed no rows")
    return rows


class RedundancyProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if PROBE is not None:
            cls.probe = PROBE
            return
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.probe = Path(temporary.name) / "redundancy-probe"
        command = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-o", str(cls.probe), str(ROOT / "bench/redundancy_probe.c"),
        ]
        built = subprocess.run(command, capture_output=True)
        if built.returncode:
            raise RuntimeError(built.stderr.decode(errors="replace"))

    def run_probe(self, data: bytes, *arguments: str):
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "input"
            source.write_bytes(data)
            done = subprocess.run(
                [str(self.probe), str(source), *arguments],
                capture_output=True, check=True,
            )
        return parse(done.stdout.decode())

    def test_a_periodic_input_is_covered_by_both_shortcuts(self):
        """Eight bytes on repeat: every context settles and always predicts."""
        rows = self.run_probe(b"abcdefgh" * 40000)
        for run in (1, 32):
            row = rows[("lzp", 6, run)]
            self.assertGreater(row["cover%"], 99.0)
            self.assertLess(row["err%"], 0.1)
            self.assertLess(row["work%"], 20.0)
        gate = rows[("gate", 4, 6)]
        self.assertGreater(gate["cover%"], 99.0)
        self.assertLess(gate["err%"], 0.1)

    def test_incompressible_input_is_covered_by_neither(self):
        """No context repeats predictably, so no shortcut may claim a byte."""
        import random

        random.seed(20260916)
        rows = self.run_probe(bytes(random.randrange(256) for _ in range(200000)))
        self.assertLess(rows[("lzp", 6, 1)]["cover%"], 1.0)
        self.assertLess(rows[("gate", 4, 6)]["cover%"], 1.0)
        self.assertGreater(rows[("lzp", 6, 1)]["work%"], 99.0)

    def test_a_longer_run_threshold_never_covers_more(self):
        """Coverage is monotone in the threshold, on any input."""
        rows = self.run_probe(b"the quick brown fox. " * 8000)
        covered = [rows[("lzp", 6, run)]["cover%"] for run in (1, 2, 4, 8, 16, 32)]
        self.assertEqual(covered, sorted(covered, reverse=True))

    def test_a_higher_min_seen_never_covers_more(self):
        """A stricter settling rule cannot widen the gate."""
        data = b"aab" * 30000 + bytes(range(256)) * 200
        loose = self.run_probe(data, "--min-seen", "2")[("gate", 4, 6)]
        strict = self.run_probe(data, "--min-seen", "16")[("gate", 4, 6)]
        self.assertLessEqual(strict["cover%"], loose["cover%"])

    def test_limit_reads_only_the_prefix(self):
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "input"
            source.write_bytes(b"x" * 100000)
            done = subprocess.run(
                [str(self.probe), str(source), "--limit", "4096"],
                capture_output=True, check=True,
            )
        self.assertIn("4096 bytes", done.stdout.decode())

    def test_bad_arguments_and_missing_files_are_refused(self):
        self.assertEqual(subprocess.run([str(self.probe)], capture_output=True).returncode, 2)
        self.assertEqual(
            subprocess.run([str(self.probe), "x", "--min-seen", "0"], capture_output=True).returncode, 2
        )
        self.assertEqual(
            subprocess.run([str(self.probe), "x", "--nonsense"], capture_output=True).returncode, 2
        )
        with tempfile.TemporaryDirectory() as scratch:
            missing = Path(scratch) / "absent"
            self.assertEqual(
                subprocess.run([str(self.probe), str(missing)], capture_output=True).returncode, 1
            )
            empty = Path(scratch) / "empty"
            empty.write_bytes(b"")
            self.assertEqual(
                subprocess.run([str(self.probe), str(empty)], capture_output=True).returncode, 1
            )


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        PROBE = Path(sys.argv.pop(1)).resolve()
    unittest.main()
