"""The compiled CLI: encoding's optional verification, folder coding, the
whole command surface driven once, and — on the committed vectors — the
exact messages and reports each command gives, the bytes `info` describes,
and the file a pipe and a file encode to."""

import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

CADAPT = Path(__file__).resolve().parent.parent / "build" / "cadapt"
VECTORS = Path(__file__).resolve().parent / "vectors"
LOW_MEMORY = str(16 << 20)
SOURCE = b"optional verification\n" * 128


def payload_stream(container: bytes) -> bytes:
    """Remove only validated filesystem metadata for legacy stream comparisons."""
    if container[4] not in (16, 17, 18):
        return container
    base = 8 + (2 if container[7] & 0x80 else 0)
    assert zlib.crc32(container[:base + 28]) == int.from_bytes(container[base + 28:base + 32], "little")
    return container[:4] + bytes([container[4] - 3]) + container[5:base] + container[base + 32:]


class CodecTest(unittest.TestCase):
    """A scratch directory with the source written, and the codec runner."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.bin"
        self.source.write_bytes(SOURCE)

    def run_codec(self, *args, data=None, umask=None):
        return subprocess.run(
            [str(CADAPT), *map(str, args)],
            input=data,
            capture_output=True,
            timeout=60,
            preexec_fn=None if umask is None else (lambda: os.umask(umask)),
        )

    def assert_usage_error(self, run, *fragments: bytes) -> None:
        """Exit 2, the named fragments on stderr, the usage text after them,
        nothing on stdout."""
        self.assertEqual(run.returncode, 2, run.stderr)
        for fragment in fragments:
            self.assertIn(fragment, run.stderr)
        self.assertIn(b"usage:", run.stderr)
        self.assertEqual(run.stdout, b"")

    def assert_failure(self, run, *fragments: bytes) -> None:
        """Exit 1 and the named fragments on stderr."""
        self.assertEqual(run.returncode, 1, run.stderr)
        for fragment in fragments:
            self.assertIn(fragment, run.stderr)


class VerificationTest(CodecTest):

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
                self.assertEqual(payload_stream(output.read_bytes()), payload_stream(verified_output.read_bytes()))
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
        self.assertEqual(payload_stream(plain.stdout), payload_stream(explicit.stdout))

    def test_published_files_take_the_mode_the_umask_leaves(self) -> None:
        """New containers honor umask; restored named files recover the
        source mode. Legacy streams continue to honor umask when decoded."""
        for umask, mode in ((0o022, 0o644), (0o002, 0o664), (0o077, 0o600)):
            with self.subTest(umask=oct(umask)):
                encoded = self.root / f"mode-{umask:o}.adpt"
                decoded = self.root / f"mode-{umask:o}.bin"
                for args, path in (
                    (("encode", self.source, "-o", encoded), encoded),
                    (("decode", encoded, "-o", decoded), decoded),
                    (("encode", self.source, "-o", encoded, "--force"), encoded),
                ):
                    run = self.run_codec(*args, umask=umask)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    wanted = stat.S_IMODE(self.source.stat().st_mode) if args[0] == "decode" else mode
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), wanted, f"{args} left {path} at {oct(path.stat().st_mode)}")
                self.assertEqual(decoded.read_bytes(), SOURCE)
                legacy = self.run_codec("decode", "-", "-o", decoded, "--force", data=payload_stream(encoded.read_bytes()), umask=umask)
                self.assertEqual(legacy.returncode, 0, legacy.stderr)
                self.assertEqual(stat.S_IMODE(decoded.stat().st_mode), mode)

    def test_conflicting_verification_flags_are_rejected_before_output(self) -> None:
        for flags in (("--verify", "--no-verify"), ("--no-verify", "--verify")):
            with self.subTest(flags=flags):
                output = self.root / "conflict.adpt"
                encoded = self.run_codec("encode", self.source, "-o", output, *flags)
                self.assertEqual(encoded.returncode, 2, encoded.stderr)
                self.assertIn(b"cannot be used together", encoded.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(encoded.stdout, b"")


@unittest.skipIf(os.name == "nt", "POSIX modes and nanosecond timestamps")
class MetadataTest(CodecTest):
    """Restoration obtains metadata from the container, not its filesystem."""

    def test_single_file_metadata_survives_copy_and_source_removal(self):
        for profile in ((), ("--fast",), ("--max",)):
            with self.subTest(profile=profile):
                self.source.write_bytes(SOURCE)
                self.source.chmod(0o751)
                times = (1_500_000_000_123_456_789, -100_000_000_987_654_321)
                os.utime(self.source, ns=times)
                encoded = self.run_codec("encode", self.source, "-o", "-", "--verify", *profile)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                self.source.unlink()
                container = self.root / "copied.adpt"
                container.write_bytes(encoded.stdout)
                container.chmod(0o600)
                restored = self.root / "restored.bin"
                decoded = self.run_codec("decode", container, "-o", restored, "--force")
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                st = restored.stat()
                self.assertEqual(stat.S_IMODE(st.st_mode), 0o751)
                self.assertEqual((st.st_atime_ns, st.st_mtime_ns), times)
                self.assertEqual(restored.read_bytes(), SOURCE)
                piped = self.run_codec("decode", "-", data=encoded.stdout)
                self.assertEqual(piped.returncode, 0, piped.stderr)
                self.assertEqual(piped.stdout, SOURCE)

    def test_folder_metadata_includes_root_and_readonly_directories(self):
        folder = self.root / "tree"
        (folder / "nested" / "empty").mkdir(parents=True)
        (folder / "nested" / "data").write_bytes(SOURCE)
        os.symlink("data", folder / "nested" / "link")
        expected = {}
        for index, (rel, mode) in enumerate((("nested/empty", 0o500), ("nested/data", 0o640),
                                             ("nested/link", 0o777), ("nested", 0o550), ("", 0o750))):
            path = folder / rel
            if not path.is_symlink():
                path.chmod(mode)
            times = (1_400_000_000_123_456_789 + index, 1_450_000_000_987_654_321 + index)
            os.utime(path, ns=times, follow_symlinks=False)
            expected[rel] = (mode, *times)
        container = self.root / "tree.adpt"
        encoded = self.run_codec("encode", folder, "-o", container, "--verify", "--max-memory", LOW_MEMORY)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        restored = self.root / "restored"
        decoded = self.run_codec("decode", container, "-o", restored)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        # Inspect timestamps before listing directories or reading content.
        for rel, wanted in expected.items():
            st = (restored / rel).lstat()
            self.assertEqual((stat.S_IMODE(st.st_mode), st.st_atime_ns, st.st_mtime_ns), wanted, rel)
        self.assertEqual((restored / "nested" / "data").read_bytes(), SOURCE)
        self.assertEqual(os.readlink(restored / "nested" / "link"), "data")
        self.assertTrue((restored / "nested" / "empty").is_dir())

    def test_metadata_damage_is_rejected_without_publishing(self):
        encoded = self.run_codec("encode", self.source, "-o", "-")
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(encoded.stdout[4], (16, 17, 18))
        damaged = bytearray(encoded.stdout)
        metadata_at = 8 + (2 if damaged[7] & 0x80 else 0)
        damaged[metadata_at] ^= 1
        output = self.root / "refused"
        decoded = self.run_codec("decode", "-", "-o", output, data=damaged)
        self.assertEqual(decoded.returncode, 1, decoded.stderr)
        self.assertFalse(output.exists())
        self.assertEqual([], list(self.root.glob("*.part")))

    def test_unrepresentable_timestamps_refuse_publication_and_clean_readonly_tree(self):
        folder = self.root / "readonly"
        (folder / "nested").mkdir(parents=True)
        (folder / "nested" / "data").write_bytes(SOURCE)
        (folder / "nested").chmod(0o500)
        for source in (self.source, folder):
            with self.subTest(source=source.name):
                encoded = self.run_codec("encode", source, "-o", "-")
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                invalid = bytearray(encoded.stdout)
                base = 8 + (2 if invalid[7] & 0x80 else 0)
                # A valid signed container timestamp outside filesystem ranges.
                invalid[base + 16:base + 24] = ((1 << 63) - 1).to_bytes(8, "little")
                invalid[base + 28:base + 32] = zlib.crc32(invalid[:base + 28]).to_bytes(4, "little")
                output = self.root / (source.name + "-refused")
                if source == self.source:
                    output.write_bytes(b"keep existing output")
                decoded = self.run_codec("decode", "-", "-o", output, "--force", data=invalid)
                self.assert_failure(decoded, b"metadata")
                if source == self.source:
                    self.assertEqual(output.read_bytes(), b"keep existing output")
                else:
                    self.assertFalse(output.exists())
                self.assertEqual([], [p for p in self.root.iterdir() if ".part" in p.name])


class MemoryContractTest(CodecTest):
    def test_one_mib_refusal_matches_documented_minimum(self):
        help_run = self.run_codec("--help")
        self.assertEqual(help_run.returncode, 0, help_run.stderr)
        help_text = help_run.stdout + help_run.stderr
        self.assertNotIn(b"never a refused input", help_text)
        advertised = re.search(rb"minimum\s+normal single-model budget is (\d+) bytes", help_text)
        self.assertIsNotNone(advertised, help_text)
        for profile in ((), ("--fast",), ("--max",)):
            output = self.root / "too-small.adpt"
            run = self.run_codec("encode", self.source, "-o", output, "--max-memory", str(1 << 20), *profile)
            self.assert_failure(run, b"memory budget exceeded")
            match = re.search(rb"need (\d+) bytes, limit (\d+)", run.stderr)
            self.assertIsNotNone(match, run.stderr)
            self.assertGreater(int(match[1]), 1 << 20)
            if not profile:
                self.assertEqual(int(match[1]), int(advertised[1]))
            self.assertEqual(int(match[2]), 1 << 20)
            self.assertFalse(output.exists())
            self.assertEqual([], list(self.root.glob("*.part")))

        # The documented floor is usable, and one byte less is refused.
        for budget, success in ((int(advertised[1]), True), (int(advertised[1]) - 1, False)):
            with self.subTest(budget=budget):
                run = self.run_codec("encode", self.source, "-o", "-", "--max-memory", str(budget))
                if success:
                    self.assertEqual(run.returncode, 0, run.stderr)
                    decoded = self.run_codec("decode", "-", "--max-memory", str(budget), data=run.stdout)
                    self.assertEqual(decoded.returncode, 0, decoded.stderr)
                    self.assertEqual(decoded.stdout, SOURCE)
                else:
                    self.assert_failure(run, b"memory budget exceeded")
                    self.assertEqual(run.stdout, b"")


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
        # Reading can advance atime, which is part of the input now. Restore
        # the same metadata before each encode, including dangling links.
        paths = [self.folder, *self.folder.rglob("*")]
        times = [(path, path.lstat()) for path in paths]
        first = self.run_codec("encode", self.folder, "-o", self.root / "one.adpt", "--no-verify")
        for path, st in times:
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=False)
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
        # Keep the new header checksum valid to exercise the archive parser.
        base = 8 + (2 if flagged[7] & 0x80 else 0)
        flagged[base + 28:base + 32] = zlib.crc32(flagged[:base + 28]).to_bytes(4, "little")
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


class CommandTest(CodecTest):
    """The exact texts behind SurfaceTest's sweep: each usage error's
    message, what a damaged file is called, what `info` says of every kind
    of committed vector, and the reports and bytes `encode` gives."""

    def test_argument_errors_are_usage_errors(self) -> None:
        cases = (
            (("encode",), b"encode takes exactly one input"),
            (("encode", self.source, "extra.bin"), b"encode takes exactly one input"),
            (("decode",), b"decode takes exactly one input"),
            (("info",), b"info takes exactly one file"),
            (("encode", self.source, "--bogus"), b"unknown option --bogus"),
            (("decode", self.source, "--fast"), b"unknown option --fast"),
            (("info", self.source, "--force"), b"unknown option --force"),
            (("encode", self.source, "-o"), b"-o needs a value"),
            (("encode", self.source, "--max-memory", "64M"), b"--max-memory: not a byte count: 64M"),
            (("encode", self.source, "--max-memory", ""), b"--max-memory: not a byte count"),
            (("encode", self.source, "--max-memory", "99999999999999999999999"), b"--max-memory: not a byte count"),
            (("decode", self.source, "--max-output", "-1"), b"--max-output: not a byte count: -1"),
            (("decode", self.source, "--max-output", "1e9"), b"--max-output: not a byte count: 1e9"),
            (("decode", self.source, "--max-memory", "lots"), b"--max-memory: not a byte count: lots"),
            (("encode", self.source, "--threads", "0"), b"--threads: not a count of one or more: 0"),
            (("encode", self.source, "--threads", "two"), b"--threads: not a count of one or more: two"),
            (("decode", self.source, "--threads", "0"), b"--threads: not a count of one or more: 0"),
            (("encode", self.source, "--fast", "--max"), b"--fast and --max are two profiles"),
            (("encode", self.source, "--segment-bytes", "1000"), b"--segment-bytes: not a whole number of 262144-byte blocks: 1000"),
            (("encode", self.source, "--segment-bytes", "262144"), b"--segment-bytes needs --threads of two or more"),
        )
        for args, fragment in cases:
            with self.subTest(args=args):
                self.assert_usage_error(self.run_codec(*args), fragment)

    def test_decode_from_and_to_pipes_and_files(self) -> None:
        encoded = VECTORS / "text.adpt"
        want = (VECTORS / "text.bin").read_bytes()
        run = self.run_codec("decode", "-", data=encoded.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, want)
        run = self.run_codec("decode", encoded, "-o", "-")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, want)
        out = self.root / "text.bin"
        run = self.run_codec("decode", "-", "-o", out, data=encoded.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(out.read_bytes(), want)
        # --threads is accepted and reported; this port decodes in order
        run = self.run_codec("decode", VECTORS / "segmented.adpt", "-o", "-", "--threads", "4")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"2 segments on 1 thread", run.stderr)
        self.assertEqual(run.stdout, (VECTORS / "segmented.bin").read_bytes())
        run = self.run_codec("decode", "-", "--threads", "4", data=encoded.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"on 1 thread (a pipe is read in order)", run.stderr)

    def test_decode_refuses_damage_limits_and_missing_files(self) -> None:
        encoded = (VECTORS / "text.adpt").read_bytes()
        cases = (
            (encoded[:5], b"file is truncated"),
            (b"RIFF" + encoded[4:], b"not an adapt file (bad signature)"),
            (encoded[:4] + bytes([255]) + encoded[5:], b"unsupported format version 255"),
            (encoded[:-1], b"file is truncated"),
            (encoded + b"!", b"1 unexpected trailing byte(s)"),
            (encoded[:-3] + bytes([encoded[-3] ^ 1]) + encoded[-2:], b"checksum mismatch"),
            (encoded[:20] + bytes([encoded[20] ^ 0x55]) + encoded[21:], b"error: "),
        )
        for damaged, fragment in cases:
            with self.subTest(fragment=fragment):
                path = self.root / "damaged.adpt"
                path.write_bytes(damaged)
                run = self.run_codec("decode", path, "-o", self.root / "damaged.out")
                self.assert_failure(run, fragment)
                self.assertFalse((self.root / "damaged.out").exists())
                self.assertEqual(run.stdout, b"")
        path = self.root / "text.adpt"
        path.write_bytes(encoded)
        self.assert_failure(self.run_codec("decode", path, "--max-output", "1000"), b"declared size", b"exceeds the limit 1000")
        self.assert_failure(self.run_codec("decode", path, "--max-memory", "1000"), b"memory budget exceeded")
        self.assert_failure(self.run_codec("decode", self.root / "absent.adpt"), b"absent.adpt: No such file or directory")
        self.assert_failure(self.run_codec("encode", self.root / "absent.bin"), b"absent.bin: No such file or directory")
        self.assert_failure(self.run_codec("info", self.root / "absent.adpt"), b"absent.adpt: No such file or directory")

    def test_info_describes_every_kind_of_file(self) -> None:
        expectations = {
            "text.adpt": (b"format          adapt container version 13", b"declared bytes  49152", b"class history   8 bytes", b"contexts        all but field, record", b"profile         normal"),
            "text-fast.adpt": (b"format          adapt container version 14", b"contexts        o2, o4", b"profile         fast"),
            "text-max.adpt": (b"format          adapt container version 15", b"profile         max"),
            "records.adpt": (b"record period   192 bytes", b"class history   none", b"contexts        all"),
            "segmented.adpt": (b"segments        2", b"declared bytes  300000"),
            "empty.adpt": (b"declared bytes  0", b"file bytes      21"),
        }
        for name, lines in expectations.items():
            with self.subTest(name=name):
                path = VECTORS / name
                run = self.run_codec("info", path)
                self.assertEqual(run.returncode, 0, run.stderr)
                for line in lines:
                    self.assertIn(line, run.stdout)
                self.assertIn(b"file bytes      %d" % path.stat().st_size, run.stdout)
                self.assertRegex(run.stdout, rb"crc32           [0-9a-f]{8}\n")
                # a stream is read to its end for the same report
                piped = self.run_codec("info", "-", data=path.read_bytes())
                self.assertEqual(piped.returncode, 0, piped.stderr)
                self.assertEqual(piped.stdout, run.stdout)

    def test_info_refuses_what_it_cannot_describe(self) -> None:
        encoded = (VECTORS / "segmented.adpt").read_bytes()
        cases = (
            (encoded[:6], b"cannot read header: failed to fill whole buffer"),
            (b"RIFF" + encoded[4:], b"not an adapt file (bad signature)"),
            (encoded[:15], b"file is truncated"),
            (encoded[:-13] + bytes(4) + encoded[-13 + 4:], b"invalid block"),
            (encoded[:-13] + b"\x02" + encoded[-12:], b"invalid block"),
        )
        for damaged, fragment in cases:
            with self.subTest(fragment=fragment):
                path = self.root / "damaged.adpt"
                path.write_bytes(damaged)
                self.assert_failure(self.run_codec("info", path), fragment)
                self.assert_failure(self.run_codec("info", "-", data=damaged), fragment)
        # a file with a record period, cut inside the period bytes
        period = (VECTORS / "records.adpt").read_bytes()
        (self.root / "cut.adpt").write_bytes(period[:9])
        self.assert_failure(self.run_codec("info", self.root / "cut.adpt"), b"cannot read header")

    def test_encode_switches_and_reports(self) -> None:
        text = VECTORS / "text.bin"
        # a file to standard output, and the profiles' version bytes
        for flags, version in (((), 16), (("--fast",), 17), (("--max",), 18)):
            with self.subTest(flags=flags):
                run = self.run_codec("encode", text, "-o", "-", *flags)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stdout[:4], b"ADPT")
                self.assertEqual(run.stdout[4], version)
                self.assertRegex(run.stderr, rb"^49152 bytes -> \d+ bytes \(\d+\.\d\d%\), not verified")
        # segments from a file and from a pipe, with their reports
        run = self.run_codec("encode", VECTORS / "segmented.bin", "-o", "-", "--threads", "2", "--segment-bytes", "262144")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b", 2 segments on 1 thread\n", run.stderr)
        self.assertEqual(payload_stream(run.stdout), (VECTORS / "segmented.adpt").read_bytes())
        run = self.run_codec("encode", "-", "--threads", "2", "--segment-bytes", "262144", data=(VECTORS / "segmented.bin").read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"2 segments on 1 thread (a pipe is read in order)", run.stderr)
        decoded = self.run_codec("decode", "-", data=run.stdout)
        self.assertEqual(decoded.stdout, (VECTORS / "segmented.bin").read_bytes())
        # a budget below the smallest model is refused before any output
        run = self.run_codec("encode", text, "-o", self.root / "tiny.adpt", "--max-memory", "1000")
        self.assert_failure(run, b"error: memory budget exceeded")
        self.assertFalse((self.root / "tiny.adpt").exists())
        # the proof alongside with segments, and the empty input
        run = self.run_codec("encode", VECTORS / "segmented.bin", "-o", self.root / "seg.adpt", "--threads", "2", "--segment-bytes", "262144", "--verify")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b", verified, 2 segments on 1 thread", run.stderr)
        empty = self.root / "empty.bin"
        empty.write_bytes(b"")
        run = self.run_codec("encode", empty, "-o", "-", "--verify")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(payload_stream(run.stdout), (VECTORS / "empty.adpt").read_bytes())
        self.assertIn(b"0 bytes -> 53 bytes (0.00%), verified", run.stderr)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        CADAPT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
