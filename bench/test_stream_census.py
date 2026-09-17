"""Hold the stream census to inputs whose composition is known in advance.

Run after make. The test compiles the census into a private directory; an
optional positional argument selects an already built one instead.

Authored with Claude Opus 5 (Claude Code), 2026-09-17.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CENSUS: Path | None = None

ROW = re.compile(r"^\s{2}(.+?)\s{2,}(\d+)\s+([\d.]+)%")


def parse(output: str) -> dict[str, tuple[int, float]]:
    """Read the census rows into {label: (bytes, percent)}."""
    rows: dict[str, tuple[int, float]] = {}
    for line in output.splitlines():
        found = ROW.match(line)
        if found:
            rows[found.group(1).strip()] = (int(found.group(2)), float(found.group(3)))
    if not rows:
        raise AssertionError(f"the census printed no rows:\n{output}")
    return rows


class StreamCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if CENSUS is not None:
            cls.census = CENSUS
            return
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.census = Path(temporary.name) / "stream-census"
        command = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-o", str(cls.census), str(ROOT / "bench/stream_census.c"),
        ]
        built = subprocess.run(command, capture_output=True)
        if built.returncode:
            raise RuntimeError(built.stderr.decode(errors="replace"))

    def census_of(self, data: bytes, *arguments: str) -> dict[str, tuple[int, float]]:
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "input"
            source.write_bytes(data)
            done = subprocess.run([str(self.census), str(source), *arguments],
                                  capture_output=True, check=True)
        return parse(done.stdout.decode())

    BUCKETS = ("word, codeable", "word, still raw", "digit groups", "utf-8 high bytes",
               "markup tokens", "repeated-byte runs", "everything else")

    def test_the_buckets_partition_the_input(self):
        """Every byte lands in exactly one bucket, so the rows sum to the input."""
        data = ("the quick brown fox <page>tex\u00e9t</page>   1234567\n" * 2000).encode()
        rows = self.census_of(data)
        self.assertEqual(sum(rows[b][0] for b in self.BUCKETS), len(data))

    def test_the_buckets_partition_an_adversarial_input(self):
        data = bytes(range(256)) * 400 + b"<a><a><a>" * 100 + b"\t" * 5000
        rows = self.census_of(data)
        self.assertEqual(sum(rows[b][0] for b in self.BUCKETS), len(data))

    def test_a_repeated_word_becomes_codeable(self):
        """Past the third sighting every occurrence is one the transform reaches."""
        rows = self.census_of(b"alpha " * 1000)
        codeable, raw = rows["word, codeable"][0], rows["word, still raw"][0]
        self.assertEqual(codeable + raw, 5000)
        # the first three occurrences pass through raw, the rest are coded
        self.assertEqual(codeable, 5000 - 3 * 5)

    def test_a_long_byte_run_is_counted_as_a_run(self):
        rows = self.census_of(b"." * 100000)
        self.assertEqual(rows["repeated-byte runs"][0], 100000)
        self.assertEqual(rows["everything else"][0], 0)

    def test_repeated_markup_is_counted_and_becomes_codeable(self):
        rows = self.census_of(b"<page>" * 1000)
        self.assertEqual(rows["markup tokens"][0], 6000)
        self.assertEqual(rows["word, codeable"][0], 0)

    def test_digit_groups_are_separated_from_words(self):
        """Pure digit runs repeat rarely, so no word dictionary reaches them."""
        data = b"".join(b"%07d " % n for n in range(4000))
        rows = self.census_of(data)
        self.assertEqual(rows["digit groups"][0], 4000 * 7)
        self.assertEqual(rows["word, codeable"][0], 0)

    def test_high_bytes_are_counted_as_utf8(self):
        rows = self.census_of("é".encode() * 5000)
        self.assertEqual(rows["utf-8 high bytes"][0], 2 * 5000)

    def test_limit_reads_only_the_prefix(self):
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "input"
            source.write_bytes(b"x" * 100000)
            done = subprocess.run([str(self.census), str(source), "--limit", "4096"],
                                  capture_output=True, check=True)
        self.assertIn("4096 bytes", done.stdout.decode())

    def test_bad_arguments_and_missing_files_are_refused(self):
        self.assertEqual(subprocess.run([str(self.census)], capture_output=True).returncode, 2)
        self.assertEqual(
            subprocess.run([str(self.census), "x", "--nonsense", "1"], capture_output=True).returncode, 2)
        with tempfile.TemporaryDirectory() as scratch:
            missing = Path(scratch) / "absent"
            self.assertEqual(
                subprocess.run([str(self.census), str(missing)], capture_output=True).returncode, 1)
            empty = Path(scratch) / "empty"
            empty.write_bytes(b"")
            self.assertEqual(
                subprocess.run([str(self.census), str(empty)], capture_output=True).returncode, 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        CENSUS = Path(sys.argv.pop(1)).resolve()
    unittest.main()
