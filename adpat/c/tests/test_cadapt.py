"""Exercise optional encoding verification and folder coding through the
compiled CLI."""

import os
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


def snapshot(root: Path) -> dict:
    """Every entry under root, relative, with its kind and content."""
    seen = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        base = Path(dirpath)
        for name in dirnames:
            path = base / name
            rel = str(path.relative_to(root))
            seen[rel] = ("link", os.readlink(path)) if path.is_symlink() else ("dir",)
        for name in filenames:
            path = base / name
            rel = str(path.relative_to(root))
            seen[rel] = ("link", os.readlink(path)) if path.is_symlink() else ("file", path.read_bytes())
    return seen


class FolderTest(unittest.TestCase):
    """A folder is coded as one container and restored byte for byte, with
    its structure: the constraints in docs/CONSTRAINTS.md."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.folder = self.root / "tree"
        (self.folder / "sub" / "deep").mkdir(parents=True)
        (self.folder / "empty").mkdir()
        (self.folder / "a.txt").write_bytes(b"alpha beta gamma\n" * 400)
        (self.folder / "sub" / "zero").write_bytes(b"")
        (self.folder / "sub" / "deep" / "bin").write_bytes(bytes(range(256)) * 700)
        (self.folder / "sub" / "deep" / "one").write_bytes(b"x")
        (self.folder / "sub" / "caf\u00e9 \u00fc.txt").write_bytes("caf\u00e9 \u00fcnicode\n".encode())
        if os.name != "nt":
            os.symlink("../a.txt", self.folder / "sub" / "link")
            os.symlink("nowhere", self.folder / "dangling")

    def run_codec(self, *args, data=None):
        return subprocess.run([str(CADAPT), *map(str, args)], input=data, capture_output=True, timeout=60)

    def test_folder_round_trip_preserves_every_entry(self) -> None:
        container = self.root / "tree.adpt"
        restored = self.root / "back"
        encoded = self.run_codec("encode", self.folder, "-o", container, "--no-verify")
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b" entries, ", encoded.stderr)
        info = self.run_codec("info", container)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn(b"holds           a folder", info.stdout)
        decoded = self.run_codec("decode", container, "-o", restored)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertIn(b"entries restored to", decoded.stderr)
        self.assertEqual(snapshot(restored), snapshot(self.folder))
        self.assertTrue(restored.is_dir() and (restored / "empty").is_dir())
        self.assertEqual([], [p for p in self.root.iterdir() if ".part" in p.name])

    def test_folder_container_is_deterministic_and_named_after_the_folder(self) -> None:
        first = self.run_codec("encode", self.folder, "-o", self.root / "one.adpt", "--no-verify")
        second = self.run_codec("encode", str(self.folder) + os.sep, "--no-verify")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual((self.root / "one.adpt").read_bytes(), (self.root / "tree.adpt").read_bytes())
        decoded = self.run_codec("decode", self.root / "tree.adpt")
        self.assertEqual(decoded.returncode, 1, decoded.stderr)
        self.assertIn(b"exists; pass --force", decoded.stderr)
        forced = self.run_codec("decode", self.root / "tree.adpt", "--force")
        self.assertEqual(forced.returncode, 1, forced.stderr)
        self.assertIn(b"is not empty; remove it first", forced.stderr)
        self.assertEqual(snapshot(self.folder), snapshot(self.folder))

    def test_folder_is_proven_both_ways(self) -> None:
        container = self.root / "tree.adpt"
        alongside = self.run_codec("encode", self.folder, "-o", container, "--verify", "--max-memory", str(300 << 20))
        self.assertEqual(alongside.returncode, 0, alongside.stderr)
        self.assertIn(b", verified\n", alongside.stderr)
        after = self.run_codec("encode", self.folder, "-o", container, "--force", "--verify", "--max-memory", LOW_MEMORY)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertIn(b"verified after encoding", after.stderr)
        restored = self.root / "back"
        decoded = self.run_codec("decode", container, "-o", restored, "--max-memory", LOW_MEMORY)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(snapshot(restored), snapshot(self.folder))

    def test_folder_refusals_leave_nothing_behind(self) -> None:
        container = self.root / "tree.adpt"
        self.assertEqual(self.run_codec("encode", self.folder, "-o", container, "--no-verify").returncode, 0)
        to_stdout = self.run_codec("decode", container, "-o", "-")
        self.assertEqual(to_stdout.returncode, 1, to_stdout.stderr)
        self.assertIn(b"cannot be written to standard output", to_stdout.stderr)
        self.assertEqual(to_stdout.stdout, b"")
        # a file input flagged as a folder is refused at the first byte of its bytes
        plain = self.root / "plain.adpt"
        self.assertEqual(self.run_codec("encode", self.folder / "a.txt", "-o", plain, "--no-verify").returncode, 0)
        flagged = bytearray(plain.read_bytes())
        flagged[5] |= 0x80
        (self.root / "flagged.adpt").write_bytes(flagged)
        refused = self.run_codec("decode", self.root / "flagged.adpt", "-o", self.root / "flagged")
        self.assertEqual(refused.returncode, 1, refused.stderr)
        self.assertIn(b"invalid archive at byte", refused.stderr)
        self.assertFalse((self.root / "flagged").exists())
        # --force replaces an empty directory or a file, never a directory's contents
        (self.root / "back").mkdir()
        self.assertEqual(self.run_codec("decode", container, "-o", self.root / "back", "--force").returncode, 0)
        (self.root / "asfile").write_bytes(b"old")
        self.assertEqual(self.run_codec("decode", container, "-o", self.root / "asfile", "--force").returncode, 0)
        self.assertEqual(snapshot(self.root / "asfile"), snapshot(self.folder))
        self.assertEqual([], [p for p in self.root.iterdir() if ".part" in p.name])

    @unittest.skipIf(os.name == "nt", "a fifo is a POSIX thing")
    def test_an_entry_of_another_kind_refuses_the_folder(self) -> None:
        os.mkfifo(self.folder / "pipe")
        encoded = self.run_codec("encode", self.folder, "-o", self.root / "x.adpt", "--no-verify")
        self.assertEqual(encoded.returncode, 1, encoded.stderr)
        self.assertIn(b"not a directory, a regular file or a symbolic link", encoded.stderr)
        self.assertFalse((self.root / "x.adpt").exists())


class SurfaceTest(unittest.TestCase):
    """Every command, option and refusal the CLI has, driven once."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.bin"
        self.source.write_bytes(b"surface of the command line, every option once\n" * 300)

    def run_codec(self, *args, data=None):
        return subprocess.run([str(CADAPT), *map(str, args)], input=data, capture_output=True, timeout=60)

    def test_help_version_and_unknown_commands(self) -> None:
        for args in (("--help",), ("-h",), ("help",)):
            r = self.run_codec(*args)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(b"cadapt encode <input>", r.stdout)
        for args in (("--version",), ("-V",)):
            r = self.run_codec(*args)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(b"format version", r.stdout)
        self.assertEqual(self.run_codec().returncode, 2)
        r = self.run_codec("frobnicate")
        self.assertEqual(r.returncode, 2)
        self.assertIn(b"unknown command", r.stderr)

    def test_usage_errors_name_the_option(self) -> None:
        cases = [
            (("encode",), b"exactly one input"),
            (("encode", self.source, self.source), b"exactly one input"),
            (("encode", self.source, "-o"), b"-o"),
            (("encode", self.source, "--bogus"), b"--bogus"),
            (("encode", self.source, "--fast", "--max"), b"pass one"),
            (("encode", self.source, "--max-memory", "lots"), b"--max-memory"),
            (("encode", self.source, "--max-memory", ""), b"--max-memory"),
            (("encode", self.source, "--max-memory", "99999999999999999999999"), b"--max-memory"),
            (("encode", self.source, "--threads", "0"), b"--threads"),
            (("encode", self.source, "--threads", "2", "--segment-bytes", "100"), b"--segment-bytes"),
            (("encode", self.source, "--segment-bytes", "262144"), b"needs --threads"),
            (("decode",), b"exactly one input"),
            (("decode", self.source, "--threads", "x"), b"--threads"),
            (("decode", self.source, "--max-output", "x"), b"--max-output"),
            (("decode", self.source, "--max-memory", "x"), b"--max-memory"),
            (("decode", self.source, "--bogus"), b"--bogus"),
            (("info",), b"exactly one file"),
            (("info", self.source, "--force"), b"--force"),
        ]
        for args, expect in cases:
            with self.subTest(args=args):
                r = self.run_codec(*args)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(expect, r.stderr)
                self.assertEqual(r.stdout, b"")

    def test_missing_and_damaged_inputs(self) -> None:
        r = self.run_codec("encode", self.root / "absent")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"No such file", r.stderr)
        r = self.run_codec("decode", self.root / "absent")
        self.assertEqual(r.returncode, 1)
        r = self.run_codec("info", self.root / "absent")
        self.assertEqual(r.returncode, 1)
        # not a container, a truncated one, a damaged one
        r = self.run_codec("decode", self.source, "-o", "-")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"bad signature", r.stderr)
        container = self.root / "source.bin.adpt"
        self.assertEqual(self.run_codec("encode", self.source, "--no-verify").returncode, 0)
        coded = container.read_bytes()
        (self.root / "short.adpt").write_bytes(coded[:20])
        r = self.run_codec("decode", self.root / "short.adpt", "-o", "-")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"truncated", r.stderr)
        r = self.run_codec("info", self.root / "short.adpt")
        self.assertEqual(r.returncode, 1)
        (self.root / "tiny.adpt").write_bytes(coded[:5])
        self.assertEqual(self.run_codec("info", self.root / "tiny.adpt").returncode, 1)
        damaged = bytearray(coded)
        damaged[-1] ^= 0xFF
        (self.root / "damaged.adpt").write_bytes(damaged)
        r = self.run_codec("decode", self.root / "damaged.adpt", "-o", "-")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"checksum", r.stderr)
        r = self.run_codec("decode", container, "-o", "-", "--max-output", "10")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"exceeds the limit", r.stderr)
        r = self.run_codec("decode", container, "-o", "-", "--max-memory", "1000")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"memory budget", r.stderr)

    def test_names_pipes_threads_and_info(self) -> None:
        container = self.root / "source.bin.adpt"
        original = self.source.read_bytes()
        r = self.run_codec("encode", self.source, "--threads", "2", "--segment-bytes", "262144", "--no-verify")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"segments on 1 thread", r.stderr)
        # decode strips .adpt, here back onto the source; a name without it gains .out
        r = self.run_codec("decode", container, "--force", "--threads", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"segments on 1 thread", r.stderr)
        self.assertEqual(self.source.read_bytes(), original)
        other = self.root / "renamed"
        other.write_bytes(container.read_bytes())
        r = self.run_codec("decode", other)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.root / "renamed.out").read_bytes(), original)
        self.assertEqual(self.run_codec("decode", other).returncode, 1)
        # pipes both ways, with the thread note for a pipe
        r = self.run_codec("decode", "-", "--threads", "2", data=container.read_bytes())
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"a pipe is read in order", r.stderr)
        self.assertEqual(r.stdout, original)
        r = self.run_codec("encode", "-", "--threads", "2", "--no-verify", data=original)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"a pipe is read in order", r.stderr)
        # info on a file, on a pipe, on every profile, on a segmented file
        for profile in ((), ("--fast",), ("--max",)):
            out = self.root / "p.adpt"
            self.assertEqual(self.run_codec("encode", self.source, "-o", out, "--force", "--no-verify", *profile).returncode, 0)
            r = self.run_codec("info", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(b"holds           a file", r.stdout)
            self.assertIn(b"profile", r.stdout)
            piped = self.run_codec("info", "-", data=out.read_bytes())
            self.assertEqual(piped.returncode, 0, piped.stderr)
            self.assertEqual(piped.stdout, r.stdout)
        r = self.run_codec("info", container)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"segments        ", r.stdout)
        empty = self.root / "empty"
        empty.write_bytes(b"")
        self.assertEqual(self.run_codec("encode", empty, "--no-verify").returncode, 0)
        r = self.run_codec("info", self.root / "empty.adpt")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"declared bytes  0", r.stdout)
        # a record period is reported
        image = self.root / "image.raw"
        image.write_bytes(
            bytes(
                (((x // 5) * 31 + (y // 4) * 17) + c * 85) & 255
                for y in range(300)
                for x in range(40)
                for c in range(3)
            )
        )
        self.assertEqual(self.run_codec("encode", image, "--no-verify").returncode, 0)
        r = self.run_codec("info", self.root / "image.raw.adpt")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"record period   120 bytes", r.stdout)
        # an existing output without --force, and a folder output for a file
        self.assertEqual(self.run_codec("encode", self.source).returncode, 1)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        CADAPT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
