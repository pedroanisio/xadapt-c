"""Exercise the allocation probe against the real standalone decoder.

Source: OpenAI GPT-6 via Codex.
"""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROBE = ROOT / "bench/results/current-experiment/allocation-probe"
CODEC = ROOT / "adpat/c/build/cadapt"


class AllocationTest(unittest.TestCase):
    def test_roundtrip_and_budget_refusal(self):
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / "source"
            output = Path(scratch) / "encoded"
            source.write_bytes(bytes(range(256)) * 128)
            for table, window in ((20, 16), (19, 17)):
                os.chmod(source, 0o641)
                os.utime(source, ns=(1600000000123456789, 1600000001987654321))
                run = subprocess.run(
                    [
                        str(PROBE),
                        str(source),
                        str(output),
                        str(128 << 20),
                        str(table),
                        str(window),
                    ],
                    capture_output=True,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                decoded = subprocess.run(
                    [str(CODEC), "decode", str(output), "-o", "-"], capture_output=True
                )
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, source.read_bytes())
                restored = Path(scratch) / "restored"
                full = subprocess.run(
                    [str(CODEC), "decode", str(output), "-o", str(restored)],
                    capture_output=True,
                )
                self.assertEqual(full.returncode, 0, full.stderr)
                state = restored.stat()
                self.assertEqual(stat.S_IMODE(state.st_mode), 0o641)
                self.assertEqual(
                    (state.st_atime_ns, state.st_mtime_ns),
                    (1600000000123456789, 1600000001987654321),
                )
                self.assertEqual(restored.read_bytes(), source.read_bytes())
                restored.unlink()
                refused = subprocess.run(
                    [str(PROBE), str(source), str(output), str(128 << 20), "20", "16"],
                    capture_output=True,
                )
                self.assertNotEqual(refused.returncode, 0)
                output.unlink()
            for table, window in (
                ("27", "16"),
                ("20", "15"),
                ("999", "16"),
                ("-1", "16"),
                ("bad", "16"),
            ):
                bad = subprocess.run(
                    [
                        str(PROBE),
                        str(source),
                        str(output),
                        str(128 << 20),
                        table,
                        window,
                    ],
                    capture_output=True,
                )
                self.assertNotEqual(bad.returncode, 0)
                self.assertFalse(output.exists())
            denied = subprocess.run(
                [str(PROBE), str(source), str(output), "1048576", "20", "16"],
                capture_output=True,
            )
            self.assertNotEqual(denied.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    if len(sys.argv) > 1:
        PROBE = Path(sys.argv.pop(1)).resolve()
    unittest.main()
