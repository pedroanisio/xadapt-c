"""The memory gate: a peak resident set within the container's declaration
passes, one past it fails, a binary that names no need or a round trip that
differs cannot be checked, and the usage errors are usage errors. A fake
codec plays the binary: it writes a container that "declares" a need and
allocates what its configuration says on decode.

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17,
with Claude Opus 5 (Claude Code).
"""

from __future__ import annotations

import io
import json
import stat
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import check_memory

FAKE = r'''#!/usr/bin/env python3
import json, os, shutil, sys
cfg = json.load(open(sys.argv[0] + ".cfg"))
args = sys.argv[1:]
def hold(n):
    # touch n bytes so the resident set grows by about that much
    buf = bytearray(n)
    for i in range(0, n, 4096):
        buf[i] = 1
    return buf
if args[0] == "encode":
    src, out = args[1], args[-1]
    shutil.copy(src, out + ".src")
    open(out, "wb").write(b"container")
    keep = hold(cfg.get("encode_bytes", 0))
elif args[0] == "decode":
    src = args[1]
    if "--max-memory" in args:
        if cfg.get("silent"):
            sys.exit(1)
        sys.stderr.write("error: memory budget exceeded: need %d bytes, limit 1\n" % cfg["need"])
        sys.exit(1)
    out = args[args.index("-o") + 1]
    shutil.copy(src + ".src", out)
    if cfg.get("corrupt"):
        open(out, "ab").write(b"!")
    keep = hold(cfg.get("decode_bytes", 0))
else:
    sys.exit(3)
'''


class MemoryCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.binary = self.root / "fake-cadapt"
        self.binary.write_text(FAKE)
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        self.fixture = self.root / "sample.bin"
        self.fixture.write_bytes(b"the quick brown fox " * 500)

    def configure(self, **cfg) -> None:
        (self.root / "fake-cadapt.cfg").write_text(json.dumps(cfg))

    def run_check(self, *extra: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = check_memory.main(["--binary", str(self.binary), "--fixture", f"sample={self.fixture}", *extra])
        return code, out.getvalue(), err.getvalue()

    def test_a_resident_set_within_the_declaration_passes_and_one_past_it_fails(self) -> None:
        # the fake holds 64 MiB on decode; a declaration of 80 MiB covers it
        self.configure(need=80 << 20, decode_bytes=64 << 20)
        code, out, _ = self.run_check("--command", "", "--slack", "0")
        self.assertEqual(code, 0, out)
        self.assertIn("PASS sample encode decode", out)
        self.assertIn("every peak resident set is within its declaration", out)
        # a declaration of 16 MiB does not, and the slack of 1 MiB does not save it
        self.configure(need=16 << 20, decode_bytes=64 << 20)
        code, out, _ = self.run_check("--command", "", "--slack", str(1 << 20))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL sample encode decode", out)
        self.assertIn("a peak resident set exceeds its declaration", out)
        # the encode side is held the same way
        self.configure(need=16 << 20, encode_bytes=64 << 20)
        code, out, _ = self.run_check("--command", "", "--slack", str(1 << 20))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL sample encode encode", out)

    def test_every_command_is_measured_under_its_own_name(self) -> None:
        self.configure(need=200 << 20)
        code, out, _ = self.run_check("--command=--fast", "--command=--max -6")
        self.assertEqual(code, 0, out)
        self.assertIn("PASS sample encode --fast encode", out)
        self.assertIn("PASS sample encode --max -6 decode", out)
        self.assertNotIn("--mid", out)

    def test_a_binary_that_names_no_need_or_a_wrong_round_trip_cannot_be_checked(self) -> None:
        self.configure(need=200 << 20, silent=True)
        code, _, err = self.run_check("--command", "")
        self.assertEqual(code, 2, err)
        self.assertIn("no declared need", err)
        self.configure(need=200 << 20, corrupt=True)
        code, _, err = self.run_check("--command", "")
        self.assertEqual(code, 2, err)
        self.assertIn("the round trip differs", err)

    def test_usage_errors(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(check_memory.main(["--binary", str(self.binary)]), 2)
            self.assertEqual(check_memory.main(["--binary", str(self.binary), "--fixture", "nope"]), 2)
            self.assertEqual(check_memory.main(["--binary", str(self.binary), "--fixture", f"x={self.root}/absent"]), 2)
            self.assertEqual(check_memory.main(["--binary", str(self.root / "absent"), "--fixture", f"s={self.fixture}"]), 2)
        self.assertIn("NAME=PATH", err.getvalue())


if __name__ == "__main__":
    unittest.main()
