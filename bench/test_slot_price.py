"""Hold the slot-price sweep to its contract without a full codec run.

The sweep's arithmetic, its agreement check against the codec's own H_*
order, and its failure paths are exercised against a stub encoder; one case
runs the real development build on a small fixture when `make` has produced
one, so the tool is known to drive the actual codec too.

Authored with Claude Opus 5 (Claude Code), 2026-09-16.
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

import slot_price  # noqa: E402

STUB = """#!/usr/bin/env python3
import os, sys
# Each dropped kind costs a fixed number of bytes; the baseline is 100000.
mask = int(os.environ.get("ADAPT_DROP_MASK", "0"))
size = 100000 + sum(100 * (bit + 1) for bit in range(20) if mask >> bit & 1)
out = sys.argv[sys.argv.index("-o") + 1]
open(out, "wb").write(b"\\0" * size)
"""


def varied_text(size: int) -> bytes:
    """Deterministic prose-like bytes: enough variety that a context earns."""
    import random

    words = [f"{stem}{suffix}" for stem in
             ("time", "form", "line", "state", "count", "value", "order", "field",
              "block", "model", "score", "index", "table", "phase", "range")
             for suffix in ("", "s", "d", "ing", "ly", "er")]
    rng = random.Random(20260916)
    out = bytearray()
    while len(out) < size:
        sentence = " ".join(rng.choice(words) for _ in range(rng.randrange(6, 18)))
        out += sentence.encode() + (b".\n" if rng.random() < 0.3 else b". ")
    return bytes(out[:size])


class SlotPriceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.scratch = Path(cls.temporary.name)
        cls.stub = cls.scratch / "stub-cadapt"
        cls.stub.write_text(STUB)
        cls.stub.chmod(0o755)
        cls.fixture = cls.scratch / "fixture"
        cls.fixture.write_bytes(b"the quick brown fox. " * 500)

    def run_tool(self, *arguments):
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/slot_price.py"),
             "--fixture", str(self.fixture), "--binary", str(self.stub), *arguments],
            capture_output=True,
        )
        return done

    def test_the_kind_order_matches_the_codec(self):
        """A kind added to the codec must be added here before the tool runs."""
        self.assertEqual(slot_price.read_kind_order(ROOT / "adpat/c/adapt_model.h"), slot_price.KINDS)

    def test_every_kind_is_priced_against_the_baseline(self):
        out = self.scratch / "rows.json"
        done = self.run_tool("--json", str(out))
        self.assertEqual(done.returncode, 0, done.stderr.decode())
        rows = json.loads(out.read_text())["rows"]
        self.assertEqual(len(rows), 1 + len(slot_price.KINDS))
        self.assertEqual(rows[0]["label"], "baseline")
        self.assertEqual(rows[0]["bytes"], 100000)
        for index, kind in enumerate(slot_price.KINDS):
            row = rows[index + 1]
            self.assertEqual(row["label"], kind)
            self.assertEqual(row["mask"], 1 << index)
            self.assertEqual(row["delta"], 100 * (index + 1))

    def test_the_frontier_drops_kinds_cheapest_first(self):
        out = self.scratch / "frontier.json"
        done = self.run_tool("--frontier", "--json", str(out))
        self.assertEqual(done.returncode, 0, done.stderr.decode())
        rows = [r for r in json.loads(out.read_text())["rows"] if r["label"].startswith("cheapest-")]
        self.assertEqual(len(rows), len(slot_price.KINDS))
        # the stub prices kind i at 100*(i+1), so cheapest-n drops bits 0..n-1
        for n, row in enumerate(rows, start=1):
            self.assertEqual(row["mask"], (1 << n) - 1)
            self.assertEqual(row["delta"], sum(100 * (b + 1) for b in range(n)))
        deltas = [row["delta"] for row in rows]
        self.assertEqual(deltas, sorted(deltas))

    def test_limit_prices_only_the_prefix(self):
        done = self.run_tool("--limit", "1KiB")
        self.assertEqual(done.returncode, 0, done.stderr.decode())

    def test_size_parsing(self):
        self.assertEqual(slot_price.parse_size("8MiB"), 8 << 20)
        self.assertEqual(slot_price.parse_size("100MB"), 100_000_000)
        self.assertEqual(slot_price.parse_size("4096"), 4096)

    def test_a_missing_fixture_is_refused(self):
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/slot_price.py"),
             "--fixture", str(self.scratch / "absent"), "--binary", str(self.stub)],
            capture_output=True,
        )
        self.assertEqual(done.returncode, 1)
        self.assertIn("no fixture", done.stderr.decode())

    def test_an_encoder_that_fails_is_reported_not_swallowed(self):
        broken = self.scratch / "broken-cadapt"
        broken.write_text("#!/bin/sh\nexit 3\n")
        broken.chmod(0o755)
        done = subprocess.run(
            [sys.executable, "-B", str(ROOT / "bench/slot_price.py"),
             "--fixture", str(self.fixture), "--binary", str(broken)],
            capture_output=True,
        )
        self.assertEqual(done.returncode, 1)
        self.assertIn("slot_price:", done.stderr.decode())

    @unittest.skipUnless((ROOT / "adpat/c/build/cadapt").exists(), "needs make")
    def test_the_real_codec_prices_a_small_fixture(self):
        """One end-to-end point: a development build really answers the mask."""
        with tempfile.TemporaryDirectory() as name:
            build = Path(name) / "build"
            try:
                binary = slot_price.build_probe_binary(build)
            except slot_price.ProbeError as failure:  # a toolchain that cannot build it
                self.skipTest(str(failure))
            out = Path(name) / "c.adpt"
            source = Path(name) / "source"
            source.write_bytes(varied_text(300000))
            plain, _ = slot_price.encode(binary, source, out, 0, 1)
            # every hashed context out: a real input must cost more without them
            dropped, _ = slot_price.encode(binary, source, out, (1 << len(slot_price.KINDS)) - 1, 1)
            self.assertGreater(dropped, plain)


if __name__ == "__main__":
    unittest.main()
