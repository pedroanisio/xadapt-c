"""Hold the markup-book prototype to a byte-exact round trip.

A transform that does not invert is worth nothing whatever it measures, so
every case here encodes and decodes and compares against the source. Run
after make; an optional positional argument selects an already built binary.

Authored with Claude Opus 5 (Claude Code), 2026-09-17.
"""

import os
import random
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTO: Path | None = None


class MarkupBookProtoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if PROTO is not None:
            cls.proto = PROTO
            return
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.proto = Path(temporary.name) / "markup-book-proto"
        command = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-o", str(cls.proto), str(ROOT / "bench/markup_book_proto.c"),
        ]
        built = subprocess.run(command, capture_output=True)
        if built.returncode:
            raise RuntimeError(built.stderr.decode(errors="replace"))

    def round_trip(self, data: bytes) -> int:
        """Encode, decode, assert the source comes back; return coded bytes."""
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "in"
            coded = Path(scratch) / "coded"
            back = Path(scratch) / "back"
            source.write_bytes(data)
            subprocess.run([str(self.proto), "enc", str(source), str(coded)], check=True, capture_output=True)
            subprocess.run([str(self.proto), "dec", str(coded), str(back)], check=True, capture_output=True)
            self.assertEqual(back.read_bytes(), data, "the round trip did not return the source")
            return coded.stat().st_size

    def test_repeated_tags_shrink(self):
        data = b"<page><title>x</title></page>" * 2000
        self.assertLess(self.round_trip(data), len(data) // 2)

    def test_prose_without_markup_is_untouched_in_size(self):
        data = b"the quick brown fox jumps over the lazy dog. " * 2000
        self.assertEqual(self.round_trip(data), len(data))

    def test_high_bytes_round_trip_through_the_escape(self):
        data = bytes(range(128, 256)) * 500
        self.assertEqual(self.round_trip(data), 2 * len(data))

    def test_every_byte_value_round_trips(self):
        self.round_trip(bytes(range(256)) * 200)

    def test_unterminated_and_oversized_tags_round_trip(self):
        self.round_trip(b"<" * 5000)
        self.round_trip(b"<" + b"a" * 200 + b">" + b"<ok><ok><ok><ok>")
        self.round_trip(b"<a<b<c>>>" * 500)

    def test_random_bytes_round_trip(self):
        rng = random.Random(20260917)
        self.round_trip(bytes(rng.randrange(256) for _ in range(200000)))

    def test_random_markupish_bytes_round_trip(self):
        rng = random.Random(1)
        parts = []
        tags = [b"<page>", b"</page>", b"<id>", b"<x y='1'>", b"<", b">", b"\xc3\xa9"]
        for _ in range(20000):
            parts.append(rng.choice(tags) if rng.random() < 0.5 else bytes([rng.randrange(256)]))
        self.round_trip(b"".join(parts))

    def test_empty_input_round_trips(self):
        self.round_trip(b"")

    def test_bad_arguments_are_refused(self):
        self.assertEqual(subprocess.run([str(self.proto)], capture_output=True).returncode, 2)
        self.assertEqual(
            subprocess.run([str(self.proto), "wat", "a", "b"], capture_output=True).returncode, 2)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        PROTO = Path(sys.argv.pop(1)).resolve()
    unittest.main()
