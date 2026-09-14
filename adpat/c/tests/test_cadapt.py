"""Exercise optional encoding verification through the compiled CLI."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CADAPT = Path(__file__).resolve().parent.parent / "build" / "cadapt"
LOW_MEMORY = str(16 << 20)
SOURCE = b"optional verification\n" * 128


class VerificationTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.bin"
        self.source.write_bytes(SOURCE)

    def run_codec(self, *args, data=None):
        return subprocess.run(
            [str(CADAPT), *map(str, args)],
            input=data,
            capture_output=True,
            timeout=30,
        )

    def assert_roundtrip(self, encoded):
        decoded = self.run_codec("decode", "-", data=encoded)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout, SOURCE)

    def test_file_verification_is_opt_in_for_each_profile(self) -> None:
        for profile in ((), ("--fast",), ("--max",)):
            with self.subTest(profile=profile):
                output = self.root / "plain.adpt"
                verified_output = self.root / "verified.adpt"
                plain = self.run_codec("encode", self.source, "-o", output, "--force", *profile)
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertIn(b"not verified", plain.stderr)
                verified = self.run_codec(
                    "encode", self.source, "-o", verified_output, "--force", "--verify", *profile
                )
                self.assertEqual(verified.returncode, 0, verified.stderr)
                self.assertIn(b", verified", verified.stderr)
                self.assertEqual(output.read_bytes(), verified_output.read_bytes())
                self.assert_roundtrip(output.read_bytes())

    def test_default_pipe_succeeds_with_one_model_budget(self) -> None:
        encoded = self.run_codec("encode", "-", "--max-memory", LOW_MEMORY, data=SOURCE)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b"not verified", encoded.stderr)
        self.assert_roundtrip(encoded.stdout)

    def test_verified_pipe_succeeds_with_two_model_budget(self) -> None:
        encoded = self.run_codec("encode", "-", "--verify", data=SOURCE)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b", verified", encoded.stderr)
        self.assert_roundtrip(encoded.stdout)

    def test_verified_pipe_refuses_insufficient_memory_before_output(self) -> None:
        encoded = self.run_codec("encode", "-", "--verify", "--max-memory", LOW_MEMORY, data=SOURCE)
        self.assertEqual(encoded.returncode, 1, encoded.stderr)
        self.assertIn(b"two models", encoded.stderr)
        self.assertEqual(encoded.stdout, b"")

    def test_verified_file_uses_second_pass_with_one_model_budget(self) -> None:
        output = self.root / "verified.adpt"
        encoded = self.run_codec(
            "encode", self.source, "-o", output, "--verify", "--max-memory", LOW_MEMORY
        )
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b"verified after encoding", encoded.stderr)
        self.assert_roundtrip(output.read_bytes())

    def test_no_verify_remains_compatible_with_default(self) -> None:
        args = ("encode", self.source, "-o", "-", "--max-memory", LOW_MEMORY)
        plain = self.run_codec(*args)
        explicit = self.run_codec(*args, "--no-verify")
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertEqual(explicit.returncode, 0, explicit.stderr)
        self.assertIn(b"not verified (--no-verify)", explicit.stderr)
        self.assertEqual(plain.stdout, explicit.stdout)

    def test_conflicting_verification_flags_are_rejected_before_output(self) -> None:
        for flags in (("--verify", "--no-verify"), ("--no-verify", "--verify")):
            with self.subTest(flags=flags):
                output = self.root / "conflict.adpt"
                encoded = self.run_codec("encode", self.source, "-o", output, *flags)
                self.assertEqual(encoded.returncode, 2, encoded.stderr)
                self.assertIn(b"cannot be used together", encoded.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(encoded.stdout, b"")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        CADAPT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
