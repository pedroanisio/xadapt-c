"""The compiled CLI: encoding's optional verification, folder coding, the
whole command surface driven once, and — on the committed vectors — the
exact messages and reports each command gives, the bytes `info` describes,
and the file a pipe and a file encode to."""

import os
import random
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

CADAPT = Path(__file__).resolve().parent.parent / "build" / "cadapt"
VECTORS = Path(__file__).resolve().parent / "vectors"
LOW_MEMORY = str(32 << 20)  # decode caps only; encode uses the preset -3 (32 MiB)
# Seconds one codec run may take: a bound against a hang, wide enough for
# the coverage gate's unoptimised build on a slow host.
TIMEOUT = 600
# The cases that encode megabytes under --max (the sizer's choices for a
# named input of 4 MiB, the text policy's 8 MiB, the tiers above xz on a
# pipe) run only with ADAPT_LONG_TESTS=1 (`make check-long`): under the
# coverage build's instrumentation each costs a minute, and the fast
# suite pins the same formats and policies on one block. The library's C
# tests hold the sizer's rules at every budget in milliseconds.
LONG = unittest.skipUnless(os.environ.get("ADAPT_LONG_TESTS") == "1", "a long case: ADAPT_LONG_TESTS=1 (make check-long)")
SOURCE = b"optional verification\n" * 128


def payload_stream(container: bytes) -> bytes:
    """Remove only validated filesystem metadata for legacy stream comparisons."""
    if container[4] not in (22, 23, 24, 28, 29, 30, 35, 36, 37, 38, 42, 43, 44, 49, 50, 51, 52, 56, 57, 67, 68, 69, 70, 71):
        return container
    base = 8 + (2 if container[7] & 0x80 else 0) + (12 if (35 <= container[4] <= 38 or 49 <= container[4] <= 52) else 0)
    assert zlib.crc32(container[:base + 28]) == int.from_bytes(container[base + 28:base + 32], "little")
    offset = 4 if (35 <= container[4] <= 38 or 49 <= container[4] <= 52) else 5 if 67 <= container[4] <= 71 else 3
    return container[:4] + bytes([container[4] - offset]) + container[5:base] + container[base + 32:]


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
            timeout=TIMEOUT,
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


class AllocationTest(CodecTest):
    def test_explicit_text_composes_with_fractional_context_allocation(self):
        # A pipe at this tier gets a fractional table. The explicit text
        # option must preserve both the fraction and the transform bit.
        for options in ((), ("--threads", "2")):
            with self.subTest(options=options):
                encoded = self.run_codec("encode", "-", "--max", "-3", "--allocation", "context",
                                         "--transform", "text", "-o", "-", *options, data=SOURCE)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                header = encoded.stdout
                self.assertEqual(header[4], 58)  # sized max profile
                sizing = header[8 + (2 if header[7] & 0x80 else 0)]
                self.assertNotEqual((sizing >> 2) & 7, 0)
                self.assertTrue(sizing & 32)  # text transform
                decoded = self.run_codec("decode", "-", "-o", "-", data=encoded.stdout)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, SOURCE)

    def test_context_policy_roundtrips_and_search(self):
        for profile in ((), ("--fast",), ("--max",), ("--max", "--adaptive", "rates"),
                        ("--adaptive", "auto"), ("--threads", "2")):
            with self.subTest(profile=profile):
                encoded = self.run_codec("encode", self.source, "-o", "-", "--allocation", "context", *profile)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                self.assertIn(b"not verified", encoded.stderr)
                decoded = self.run_codec("decode", "-", data=encoded.stdout)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, SOURCE)
        pipe = self.run_codec("encode", "-", "--allocation", "context", "--verify", data=SOURCE)
        self.assertEqual(pipe.returncode, 0, pipe.stderr)
        self.assertNotIn(b"not verified", pipe.stderr)
        decoded = self.run_codec("decode", "-", data=pipe.stdout)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout, SOURCE)

    def test_context_policy_bmp_candidates_and_folder(self):
        width, height = 17, 13
        stride = (width * 3 + 3) & ~3
        pixels = bytes((i // 3 + i // stride) & 255 for i in range(stride * height))
        data = (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 7, 11, 54) +
                struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
        self.source.write_bytes(data)
        sizes = []
        for mode in ((), ("--adaptive", "rates"), ("--adaptive", "sparse"), ("--adaptive", "retained")):
            for transform in ((), ("--transform", "bmp")):
                run = self.run_codec("encode", self.source, "-o", "-", "--max", "--allocation", "context", *mode, *transform)
                self.assertEqual(run.returncode, 0, run.stderr)
                sizes.append(len(run.stdout))
                decoded = self.run_codec("decode", "-", data=run.stdout)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, data)
        # The compact side tables with the lean context set: a max budget
        # that stops the context table short of the target under
        # --allocation context writes version 54 (57 with metadata), the
        # same reader decodes it, and the default budget still writes
        # version 21 for this input. A pipe's length is unknown, so its
        # sizes are the tier's alone, whatever it holds: one block of the
        # text is enough, and -7 (186 MiB, which the sizer shares with the
        # proof it may be asked for) stops at 2^21 blocks with no slack for
        # a fraction.
        block = (SOURCE * ((1 << 18) // len(SOURCE) + 1))[: 1 << 18]
        compact = self.run_codec("encode", "-", "--max", "--allocation", "context", "-7", data=block)
        self.assertEqual(compact.returncode, 0, compact.stderr)
        self.assertEqual(compact.stdout[4], 54)
        back = self.run_codec("decode", "-", data=compact.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, block)
        # A budget with slack past the power of two the table reaches sizes
        # the table in eighths: the pipe at -5 (94 MiB) takes the compact
        # tables, the lean set and a fraction, written as version 58 (61
        # with metadata) whose sizing byte names all three, and the
        # container decodes through both instruction-set paths.
        sized = self.run_codec("encode", "-", "--max", "--allocation", "context", "-5", data=block)
        self.assertEqual(sized.returncode, 0, sized.stderr)
        self.assertEqual(sized.stdout[4], 58)
        self.assertEqual(sized.stdout[8] & 3, 3, "the compact tables and the lean set were not both taken at -5")
        self.assertNotEqual(sized.stdout[8] >> 2 & 7, 0, "no fraction was taken at -5")
        self.assertLess(len(sized.stdout), len(compact.stdout) + 64)
        back = self.run_codec("decode", "-", data=sized.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, block)
        env = dict(os.environ)
        env["ADAPT_ISA"] = "baseline"
        back = subprocess.run([str(CADAPT), "decode", "-"], input=sized.stdout, capture_output=True, timeout=120, env=env)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, block)
        full = self.run_codec("encode", "-", "--max", "--allocation", "context", data=block)
        self.assertEqual(full.returncode, 0, full.stderr)
        self.assertEqual(full.stdout[4], 21)
        auto = self.run_codec("encode", self.source, "-o", "-", "--max", "--allocation", "context", "--adaptive", "auto", "--verify")
        self.assertEqual(auto.returncode, 0, auto.stderr)
        self.assertEqual(len(auto.stdout), min(sizes))
        folder = self.root / "folder"
        (folder / "empty").mkdir(parents=True)
        (folder / "file").write_bytes(SOURCE)
        archive, restored = self.root / "folder.adpt", self.root / "restored"
        run = self.run_codec("encode", folder, "-o", archive, "--allocation", "context", "--threads", "2", "--verify")
        self.assertEqual(run.returncode, 0, run.stderr)
        run = self.run_codec("decode", archive, "-o", restored)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual((restored / "file").read_bytes(), SOURCE)
        self.assertTrue((restored / "empty").is_dir())

    @LONG
    def test_named_input_sizes_stop_at_its_target_and_spend_the_slack(self):
        # The named file's length is known, so its table stops at the input's
        # target (2^21 from -5 up, no fraction: version 57, compact and
        # lean with metadata); at -4 (48 MiB, raised to the max profile's
        # floor) the table is short of it and the slack past the 4 MiB
        # window is spent in eighths, version 61 with metadata, which info
        # names and which decodes. 4 MiB is the shortest input whose target
        # is 2^21.
        big = self.root / "big.bin"
        big.write_bytes((SOURCE * ((4 << 20) // len(SOURCE) + 1))[: 4 << 20])
        named_sized = self.root / "big-sized.adpt"
        run = self.run_codec("encode", big, "-o", named_sized, "--max", "--allocation", "context", "-4")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(named_sized.read_bytes()[4], 61)
        self.assertEqual(named_sized.read_bytes()[8] & 3, 3)
        self.assertNotEqual(named_sized.read_bytes()[8] >> 2 & 7, 0, "no fraction was taken at -4")
        run = self.run_codec("info", named_sized)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"adapt container version 61", run.stdout)
        self.assertIn(b"/8 past 2^", run.stdout)
        self.assertIn(b"profile         max (compact side tables, lean contexts)\n", run.stdout)
        restored_sized = self.root / "big-sized.restored"
        run = self.run_codec("decode", named_sized, "-o", restored_sized)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(restored_sized.read_bytes(), big.read_bytes())
        named = self.root / "big.adpt"
        run = self.run_codec("encode", big, "-o", named, "--max", "--allocation", "context", "-5")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(named.read_bytes()[4], 57)

    def test_invalid_policy_and_explicit_default(self):
        bad = self.run_codec("encode", self.source, "-o", "-", "--allocation", "unknown")
        self.assert_usage_error(bad, b"--allocation")
        default = self.run_codec("encode", self.source, "-o", "-")
        explicit = self.run_codec("encode", self.source, "-o", "-", "--allocation", "window")
        self.assertEqual(default.returncode, 0, default.stderr)
        self.assertEqual(explicit.returncode, 0, explicit.stderr)
        # the named input's metadata carries its access time, which the
        # first run's read may move; the streams are what the policy decides
        self.assertEqual(payload_stream(default.stdout), payload_stream(explicit.stdout))


class AdaptiveTest(CodecTest):
    @LONG
    def test_text_transform_is_taken_for_long_text_by_itself_or_refused(self):
        # A long text (8 MiB of the vector's prose, repeated) is coded under
        # the text transform by itself: the fast profile's text version 63
        # (68 with metadata), decoded to the bytes (what it saves is measured
        # on enwik8, not on a repetition the recall model already owns);
        # --transform none keeps the plain version.
        prose = (VECTORS / "text.bin").read_bytes()
        long_text = (prose * ((8 << 20) // len(prose) + 1))[: 8 << 20]
        big = self.root / "long.txt"
        big.write_bytes(long_text)
        auto = self.run_codec("encode", big, "-o", "-", "--fast")
        self.assertEqual(auto.returncode, 0, auto.stderr)
        self.assertEqual(auto.stdout[4], 68)
        none = self.run_codec("encode", big, "-o", "-", "--fast", "--transform", "none")
        self.assertEqual(none.returncode, 0, none.stderr)
        self.assertEqual(none.stdout[4], 23)
        back = self.run_codec("decode", "-", data=auto.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, long_text)

    def test_text_transform_on_a_pipe_asked_for_named_and_refused(self):
        # A pipe's length is unknown, so it keeps the plain version unless
        # asked; --transform text takes the transform for a short file too,
        # where the policy would not, and info names it; it composes with
        # neither --adaptive nor the BMP transform; a damaged text stream
        # is refused as such.
        prose = (VECTORS / "text.bin").read_bytes()
        piped = self.run_codec("encode", "-", "--fast", data=prose)
        self.assertEqual(piped.returncode, 0, piped.stderr)
        self.assertEqual(piped.stdout[4], 20) # a pipe's length is unknown: not by itself
        piped = self.run_codec("encode", "-", "--fast", "--transform", "text", data=prose)
        self.assertEqual(piped.returncode, 0, piped.stderr)
        self.assertEqual(piped.stdout[4], 63)
        back = self.run_codec("decode", "-", data=piped.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, prose)
        short = self.run_codec("encode", self.source, "-o", "-")
        self.assertEqual(short.returncode, 0, short.stderr)
        self.assertEqual(short.stdout[4], 22)
        asked = self.run_codec("encode", self.source, "-o", "-", "--transform", "text", "--verify")
        self.assertEqual(asked.returncode, 0, asked.stderr)
        self.assertEqual(asked.stdout[4], 67)
        back = self.run_codec("decode", "-", data=asked.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, SOURCE)
        named = self.root / "asked.adpt"
        named.write_bytes(asked.stdout)
        info = self.run_codec("info", named)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn(b"format          adapt container version 67", info.stdout)
        self.assertIn(b"text transform  yes", info.stdout)
        self.assertIn(b"arbiter start   1/4", info.stdout)
        # under --max the transform is the sized version 58 (61 with
        # metadata), its sizing byte naming the transform and the arbiters'
        # 1/32 starting weight; the transform's own max versions 64 to 66
        # are no longer written
        asked = self.run_codec("encode", self.source, "-o", "-", "--max", "--transform", "text", "--verify")
        self.assertEqual(asked.returncode, 0, asked.stderr)
        self.assertEqual(asked.stdout[4], 61)
        self.assertEqual(asked.stdout[8], 0x60)
        back = self.run_codec("decode", "-", data=asked.stdout)
        self.assertEqual(back.returncode, 0, back.stderr)
        self.assertEqual(back.stdout, SOURCE)
        named.write_bytes(asked.stdout)
        info = self.run_codec("info", named)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn(b"format          adapt container version 61", info.stdout)
        self.assertIn(b"profile         max\n", info.stdout)
        self.assertIn(b"text transform  yes", info.stdout)
        self.assertIn(b"arbiter start   1/32", info.stdout)
        self.assert_usage_error(self.run_codec("encode", self.source, "--transform", "text", "--adaptive", "rates"),
                                b"--transform text")
        self.assert_usage_error(self.run_codec("encode", self.source, "--transform", "words"), b"--transform")
        # a damaged text stream is refused as such, not written
        damaged = bytearray(asked.stdout)
        damaged[len(damaged) // 2] ^= 0x55
        run = self.run_codec("decode", "-", data=bytes(damaged))
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(run.stdout, b"")

    def test_bmp_transform_preserves_padding_headers_and_partial_pixels(self):
        width, height = 125, 49
        stride = (width * 3 + 3) & ~3
        pixels = bytearray(random.Random(917).randbytes(stride * height))
        for y in range(height):
            for x in range(width):
                value = (x + y) & 255
                pixels[y * stride + x * 3:y * stride + x * 3 + 3] = bytes((value, value, value))
        data = (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 19, 37, 54) +
                struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels + b"trailing bytes")
        self.source.write_bytes(data)
        sizes = []
        for profile in (None, "rates", "sparse", "retained"):
            args = () if profile is None else ("--adaptive", profile)
            for transform in ((), ("--transform", "bmp")):
                run = self.run_codec("encode", self.source, "-o", "-", "--verify", *args, *transform)
                self.assertEqual(run.returncode, 0, run.stderr)
                sizes.append(len(run.stdout))
                if transform:
                    self.assertIn(run.stdout[4], (35, 36, 37, 38))
                decoded = self.run_codec("decode", "-", data=run.stdout)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, data)
        auto = self.run_codec("encode", self.source, "-o", "-", "--adaptive", "auto")
        self.assertEqual(auto.returncode, 0, auto.stderr)
        self.assertEqual(len(auto.stdout), min(sizes))
        self.assertIn(b"bmp", auto.stderr)
        self.source.write_bytes(b"not a BMP")
        self.assert_failure(self.run_codec("encode", self.source, "--transform", "bmp"), b"invalid header")
        self.assert_usage_error(self.run_codec("encode", self.source, "--transform", "png"), b"--transform")

    def test_max_adaptive_and_bmp_search(self):
        width, height = 17, 13
        stride = (width * 3 + 3) & ~3
        pixels = bytes((i // 3 + i // stride) & 255 for i in range(stride * height))
        data = (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 7, 11, 54) +
                struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
        self.source.write_bytes(data)
        sizes = []
        for mode in (None, "rates", "sparse", "retained"):
            args = () if mode is None else ("--adaptive", mode)
            for transform in ((), ("--transform", "bmp")):
                run = self.run_codec("encode", self.source, "-o", "-", "--max", "--verify", *args, *transform)
                self.assertEqual(run.returncode, 0, run.stderr)
                sizes.append(len(run.stdout))
                if transform:
                    self.assertIn(run.stdout[4], range(49, 53))
                elif mode:
                    self.assertIn(run.stdout[4], range(42, 45))
                decoded = self.run_codec("decode", "-", data=run.stdout)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, data)
                raw = self.run_codec("decode", "-", data=payload_stream(run.stdout))
                self.assertEqual(raw.returncode, 0, raw.stderr)
                self.assertEqual(raw.stdout, data)
        auto = self.run_codec("encode", self.source, "-o", "-", "--max", "--adaptive", "auto", "--verify")
        self.assertEqual(auto.returncode, 0, auto.stderr)
        self.assertEqual(len(auto.stdout), min(sizes))
        self.assertIn(b"max", auto.stderr)
        segmented = self.run_codec("encode", "-", "--max", "--adaptive", "retained", "--threads", "2", data=SOURCE)
        self.assertEqual(segmented.returncode, 0, segmented.stderr)
        decoded = self.run_codec("decode", "-", data=segmented.stdout)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout, SOURCE)

    def test_profiles_metadata_and_determinism(self):
        for mode, version in (("rates", 28), ("sparse", 29), ("retained", 30)):
            output = self.root / "coded.adpt"
            run = self.run_codec("encode", self.source, "-o", output, "--force", "--adaptive", mode)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn(b"not verified", run.stderr)
            encoded = output.read_bytes()
            self.assertEqual(encoded[4], version)
            decoded = self.run_codec("decode", output, "-o", "-")
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            self.assertEqual(decoded.stdout, SOURCE)
            info = self.run_codec("info", output)
            self.assertIn(mode.encode(), info.stdout)
            verified = self.run_codec("encode", self.source, "-o", output, "--force", "--adaptive", mode, "--verify")
            self.assertEqual(verified.returncode, 0, verified.stderr)
            self.assertIn(b", verified", verified.stderr)
            self.assertEqual(payload_stream(encoded), payload_stream(output.read_bytes()))

    def test_auto_selects_smallest_complete_container_and_pipe_verifies(self):
        lengths = []
        for mode in (None, "rates", "sparse", "retained", "auto"):
            args = () if mode is None else ("--adaptive", mode)
            run = self.run_codec("encode", self.source, "-o", "-", "-4", *args)
            self.assertEqual(run.returncode, 0, run.stderr)
            lengths.append(len(run.stdout))
        self.assertEqual(lengths[-1], min(lengths[:-1]))
        # -2 is raised to the normal floor, which the retained state's
        # 2.8 MB no longer fits beside
        run = self.run_codec("encode", "-", "--adaptive", "auto", "--verify", "-2", data=SOURCE)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b", verified", run.stderr)
        self.assertIn(b"skipped: memory", run.stderr)
        decoded = self.run_codec("decode", "-", data=run.stdout)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout, SOURCE)

    def test_invalid_profiles_and_memory(self):
        for extra in (("--adaptive", "bad"), ("--adaptive", "rates", "--fast")):
            self.assert_usage_error(self.run_codec("encode", self.source, *extra), b"--adaptive")
        self.assert_failure(self.run_codec("encode", self.source, "--adaptive", "retained", "-0"), b"memory")

    def test_segment_reset_stored_bytes_and_folder(self):
        data = random.Random(23).randbytes(262144) + SOURCE * 100
        for mode in ("rates", "sparse", "retained"):
            run = self.run_codec("encode", "-", "--adaptive", mode, "--threads", "2", "--segment-bytes", "262144", data=data)
            self.assertEqual(run.returncode, 0, run.stderr)
            decoded = self.run_codec("decode", "-", data=run.stdout)
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            self.assertEqual(decoded.stdout, data)
        folder = self.root / "folder"
        (folder / "nested").mkdir(parents=True)
        (folder / "nested" / "bytes").write_bytes(bytes(range(256)) * 10)
        os.chmod(folder / "nested" / "bytes", 0o641)
        os.utime(folder / "nested" / "bytes", ns=(1000000123, 2000000456))
        output = self.root / "folder.adpt"
        run = self.run_codec("encode", folder, "-o", output, "--adaptive", "auto", "--verify")
        self.assertEqual(run.returncode, 0, run.stderr)
        restored = self.root / "restored"
        decoded = self.run_codec("decode", output, "-o", restored)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        entry = restored / "nested" / "bytes"
        self.assertEqual(entry.read_bytes(), bytes(range(256)) * 10)
        self.assertEqual(stat.S_IMODE(entry.stat().st_mode), 0o641)
        self.assertEqual(entry.stat().st_mtime_ns, 2000000456)


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
        encoded = self.run_codec("encode", "-", "-3", data=SOURCE)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b"not verified", encoded.stderr)
        self.assert_roundtrip(encoded.stdout)

    def test_verified_pipe_succeeds_with_two_model_budget(self) -> None:
        encoded = self.run_codec("encode", "-", "--verify", data=SOURCE)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b", verified", encoded.stderr)
        self.assert_roundtrip(encoded.stdout)

    def test_verified_pipe_refuses_insufficient_memory_before_output(self) -> None:
        encoded = self.run_codec("encode", "-", "--verify", "-3", data=SOURCE)
        self.assertEqual(encoded.returncode, 1, encoded.stderr)
        self.assertIn(b"two models", encoded.stderr)
        self.assertEqual(encoded.stdout, b"")

    def test_verified_file_uses_second_pass_with_one_model_budget(self) -> None:
        output = self.root / "verified.adpt"
        encoded = self.run_codec(
            "encode", self.source, "-o", output, "--verify", "-3"
        )
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        self.assertIn(b"verified after encoding", encoded.stderr)
        self.assert_roundtrip(output.read_bytes())

    def test_no_verify_remains_compatible_with_default(self) -> None:
        args = ("encode", self.source, "-o", "-", "-3")
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

    def test_decode_allocates_what_the_header_declares(self) -> None:
        """FMT-6: a valid container decodes by default whatever model its
        header names, above any default budget; only an explicit
        --max-memory can refuse it. A stored block reproduces its bytes
        under any model sizes, so a header rewritten to larger tables and
        window still checks out."""
        def renamed(name, table_log, window_log):
            original = (VECTORS / name).read_bytes()
            header = bytearray(original[:8])
            header[5] = (header[5] & 0xE0) | table_log
            header[7] = (header[7] & 0xE0) | window_log
            path = self.root / f"{name}-{table_log}-{window_log}.adpt"
            path.write_bytes(bytes(header) + original[8:])
            return path

        # above the 674 MiB default: the largest tables and window the format
        # allows, on the empty container, whose decode allocates no model at
        # all — the budget check on the header is what the rule is about
        largest = renamed("empty.adpt", 23, 30)
        info = self.run_codec("info", largest)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn(b"context table   2^23 blocks", info.stdout)
        self.assertIn(b"window          2^30 bytes", info.stdout)
        run = self.run_codec("decode", largest, "-o", "-")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, b"")
        capped = self.run_codec("decode", largest, "-o", "-", "--max-memory", str(674 << 20))
        self.assertEqual(capped.returncode, 1, capped.stderr)
        self.assertIn(b"memory budget exceeded: need ", capped.stderr)
        # a real decode of a model the encoder's old default did not hold
        want = (VECTORS / "hello.bin").read_bytes()
        large = renamed("hello.adpt", 22, 26)
        info = self.run_codec("info", large)
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn(b"context table   2^22 blocks", info.stdout)
        self.assertIn(b"window          2^26 bytes", info.stdout)
        run = self.run_codec("decode", large, "-o", "-")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, want)
        run = self.run_codec("decode", "-", data=large.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, want)
        capped = self.run_codec("decode", large, "-o", "-", "--max-memory", str(64 << 20))
        self.assertEqual(capped.returncode, 1, capped.stderr)
        self.assertIn(b"memory budget exceeded: need ", capped.stderr)
        self.assertIn(b", limit 67108864", capped.stderr)
        self.assertEqual(capped.stdout, b"")
        # the cap that holds it decodes; the original still decodes under the default
        run = self.run_codec("decode", large, "-o", "-", "--max-memory", str(256 << 20))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, want)

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
        encoded = self.run_codec("encode", folder, "-o", container, "--verify", "-3")
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
        self.assertIn(encoded.stdout[4], (22, 23, 24))
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
    """The preset is encode's only memory setting: a preset below the
    profile's smallest model is raised to it, never refused, so -0 writes
    the smallest model of each profile; --help states the two floors; and
    decode --max-memory, the cap, refuses that container one byte below
    its floor with the bytes it needs."""

    def test_lowest_preset_writes_the_floor_and_decode_cap_pins_it(self):
        help_run = self.run_codec("--help")
        self.assertEqual(help_run.returncode, 0, help_run.stderr)
        help_text = help_run.stdout + help_run.stderr
        advertised = re.search(rb"smallest normal model takes (\d+)\s+bytes, the max profile's (\d+)", help_text)
        self.assertIsNotNone(advertised, help_text)
        floors = {(): int(advertised[1]), ("--fast",): None, ("--max",): int(advertised[2])}
        for profile, advertised_floor in floors.items():
            with self.subTest(profile=profile):
                output = self.root / "floor.adpt"
                run = self.run_codec("encode", self.source, "-o", output, "--force", "-0", *profile)
                self.assertEqual(run.returncode, 0, run.stderr)
                # the cap at one byte names what the file needs; the normal
                # and max floors are the advertised ones, the fast profile's
                # smaller model needs less than the floor its budget shares
                probe = self.run_codec("decode", output, "-o", "-", "--max-memory", "1")
                self.assert_failure(probe, b"memory budget exceeded")
                match = re.search(rb"need (\d+) bytes, limit 1\b", probe.stderr)
                self.assertIsNotNone(match, probe.stderr)
                floor = int(match[1])
                if advertised_floor is not None:
                    self.assertEqual(floor, advertised_floor)
                else:
                    self.assertLess(floor, int(advertised[1]))
                decoded = self.run_codec("decode", output, "-o", "-", "--max-memory", str(floor))
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(decoded.stdout, SOURCE)
                refused = self.run_codec("decode", output, "-o", "-", "--max-memory", str(floor - 1))
                self.assert_failure(refused, b"memory budget exceeded")
                match = re.search(rb"need (\d+) bytes, limit (\d+)", refused.stderr)
                self.assertIsNotNone(match, refused.stderr)
                self.assertEqual(int(match[1]), floor)
                self.assertEqual(int(match[2]), floor - 1)
                self.assertEqual(refused.stdout, b"")

    def test_encode_has_no_byte_budget(self):
        for value in ("1000", str(64 << 20), "lots"):
            run = self.run_codec("encode", self.source, "-o", "-", "--max-memory", value)
            self.assert_usage_error(run, b"--max-memory is a decode option; encode sets its budget with a preset, -0 .. -11")
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


class InstructionSetTest(unittest.TestCase):
    """The per-bit path is built twice, for the baseline instruction set and
    for x86-64-v3, and the second runs the model's memories as vectors: the
    two must write the same bytes. At the smallest budget the context table
    is a few hundred lines, so two contexts claiming one block — the case
    the vector path hands back to the scalar one — happens on most bytes."""

    def encode(self, source: Path, out: Path, *args, baseline: bool) -> bytes:
        env = dict(os.environ)
        env.pop("ADAPT_ISA", None)
        if baseline:
            env["ADAPT_ISA"] = "baseline"
        run = subprocess.run(
            [str(CADAPT), "encode", str(source), "-o", str(out), "--force", "--no-verify", *map(str, args)],
            capture_output=True,
            timeout=120,
            env=env,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        return out.read_bytes()

    def test_vector_and_baseline_paths_write_the_same_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            for name in ("text.bin", "records.bin"):
                source = VECTORS / name
                for profile in (["--max"], [], ["--fast"]):
                    args = [*profile, "-4", "--allocation", "context"]
                    vector = self.encode(source, root / "vector.adpt", *args, baseline=False)
                    scalar = self.encode(source, root / "scalar.adpt", *args, baseline=True)
                    self.assertEqual(vector, scalar, f"{name} {profile}: the two instruction-set paths differ")
                    self.assertLess(len(vector), source.stat().st_size)
                    # each path decodes what the other wrote
                    for written, decoder in ((root / "vector.adpt", True), (root / "scalar.adpt", False)):
                        env = dict(os.environ)
                        env["ADAPT_ISA"] = "baseline" if decoder else ""
                        if not decoder:
                            env.pop("ADAPT_ISA")
                        run = subprocess.run(
                            [str(CADAPT), "decode", str(written), "-o", str(root / "back.bin"), "--force"],
                            capture_output=True,
                            timeout=120,
                            env=env,
                        )
                        self.assertEqual(run.returncode, 0, run.stderr)
                        self.assertEqual((root / "back.bin").read_bytes(), source.read_bytes())

    def test_the_knob_is_read_and_anything_else_means_auto(self) -> None:
        """ADAPT_ISA=baseline is the only value that changes the path; the
        bytes are the same either way, so what is checked is that both
        values are accepted and encode."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            written = []
            for value in ("baseline", "anything", ""):
                env = dict(os.environ)
                env["ADAPT_ISA"] = value
                run = subprocess.run(
                    [str(CADAPT), "encode", str(VECTORS / "text.bin"), "-o", str(root / "o.adpt"), "--force",
                     "--no-verify", "--max"],
                    capture_output=True,
                    timeout=120,
                    env=env,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                written.append(payload_stream((root / "o.adpt").read_bytes()))
            self.assertEqual(written[0], written[1])
            self.assertEqual(written[1], written[2])
            self.assertEqual(written[0], (VECTORS / "text-max.adpt").read_bytes())


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
        return subprocess.run([str(CADAPT), *map(str, args)], input=data, capture_output=True, timeout=TIMEOUT)

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
        alongside = self.run_codec("encode", self.folder, "-o", container, "--verify", "-8")
        self.assertEqual(alongside.returncode, 0, alongside.stderr)
        self.assertIn(b", verified\n", alongside.stderr)
        after = self.run_codec("encode", self.folder, "-o", container, "--force", "--verify", "-3")
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

    @unittest.skipIf(os.name == "nt", "hard links, holes and attributes are POSIX things here")
    def test_folder_keeps_hard_links_holes_attributes_and_acls(self) -> None:
        """FMT-7: two names of one inode come back as two names of one
        inode; a file's holes stay holes; user attributes and, where the
        platform stores them as attributes, POSIX ACLs — including a
        directory's default ACL — come back verified; and the container is
        the same bytes for the same folder."""
        folder = self.folder
        os.link(folder / "a.txt", folder / "sub" / "a-again.txt")
        os.link(folder / "a.txt", folder / "a-third.txt")
        sparse = folder / "sub" / "holes.bin"
        with open(sparse, "wb") as f:
            f.truncate(3 << 20)
            f.seek(1 << 20)
            f.write(b"middle")
            f.seek((3 << 20) - 3)
            f.write(b"end")
        with open(folder / "sub" / "allhole.bin", "wb") as f:
            f.truncate(1 << 20)
        try:
            os.setxattr(folder / "a.txt", "user.note", b"hello \x00 world")
            os.setxattr(folder / "a.txt", "user.empty", b"")
            os.setxattr(folder / "sub", "user.dir", b"yes")
            xattrs = True
        except OSError:
            xattrs = False
        acls = shutil.which("setfacl") is not None and shutil.which("getfacl") is not None
        if acls:
            acls = subprocess.run(["setfacl", "-m", "u:nobody:r", folder / "a.txt"], capture_output=True).returncode == 0
            acls = acls and subprocess.run(["setfacl", "-d", "-m", "u:nobody:rx", folder / "sub"], capture_output=True).returncode == 0
        container = self.root / "tree.adpt"
        paths = [folder, *folder.rglob("*")]
        stamps = [(path, path.lstat()) for path in paths]
        encoded = self.run_codec("encode", folder, "-o", container, "--verify")
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        # a hole is not stored: the folder is a few KiB of bytes plus 4 MiB of holes
        self.assertLess(container.stat().st_size, 1 << 20)
        restored = self.root / "back"
        decoded = self.run_codec("decode", container, "-o", restored)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(snapshot(restored), snapshot(folder))
        # hard links: one inode, three names
        a, again, third = (restored / "a.txt").stat(), (restored / "sub" / "a-again.txt").stat(), (restored / "a-third.txt").stat()
        self.assertEqual((again.st_ino, third.st_ino), (a.st_ino, a.st_ino))
        self.assertEqual(a.st_nlink, 3)
        # holes: the apparent length, the bytes, and far fewer blocks than bytes
        for name in ("holes.bin", "allhole.bin"):
            st = (restored / "sub" / name).stat()
            self.assertEqual(st.st_size, (folder / "sub" / name).stat().st_size)
            self.assertLess(st.st_blocks * 512, st.st_size // 4, name)
        self.assertEqual((restored / "sub" / "holes.bin").read_bytes(), sparse.read_bytes())
        if xattrs:
            self.assertEqual(sorted(os.listxattr(restored / "a.txt")), sorted(os.listxattr(folder / "a.txt")))
            self.assertEqual(os.getxattr(restored / "a.txt", "user.note"), b"hello \x00 world")
            self.assertEqual(os.getxattr(restored / "a.txt", "user.empty"), b"")
            self.assertEqual(os.getxattr(restored / "sub", "user.dir"), b"yes")
            self.assertEqual(os.getxattr(restored / "sub" / "a-again.txt", "user.note"), b"hello \x00 world")
        if acls:
            for rel in ("a.txt", "sub"):
                want = subprocess.run(["getfacl", "-c", "-p", folder / rel], capture_output=True, text=True).stdout
                got = subprocess.run(["getfacl", "-c", "-p", restored / rel], capture_output=True, text=True).stdout
                self.assertEqual(got, want, rel)
            self.assertIn("default:user:nobody:r-x", subprocess.run(["getfacl", "-c", restored / "sub"], capture_output=True, text=True).stdout)
        # the same folder, the same bytes; the source's access times restored first
        first = container.read_bytes()
        for path, st in stamps:
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=False)
        second = self.run_codec("encode", folder, "-o", self.root / "again.adpt", "--no-verify")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual((self.root / "again.adpt").read_bytes(), first)

    @unittest.skipIf(os.name == "nt", "a fifo is a POSIX thing")
    def test_an_entry_of_another_kind_refuses_the_folder(self) -> None:
        os.mkfifo(self.folder / "pipe")
        encoded = self.run_codec("encode", self.folder, "-o", self.root / "x.adpt", "--no-verify")
        self.assertEqual(encoded.returncode, 1, encoded.stderr)
        self.assertIn(b"not a directory, a regular file or a symbolic link", encoded.stderr)
        self.assertFalse((self.root / "x.adpt").exists())


class PresetTest(unittest.TestCase):
    """-0 .. -9 name xz's compression memory, -10 and -11 the 1 and 2 GiB
    tiers above it; -9 is the default; the last preset given wins."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.txt"
        # text with enough variety that the table size shows in the bytes
        rng = random.Random(7)
        words = [f"w{rng.randrange(3000)}" for _ in range(40000)]
        self.source.write_bytes(" ".join(words).encode())

    def run_codec(self, *args, data=None):
        return subprocess.run([str(CADAPT), *map(str, args)], input=data, capture_output=True, timeout=TIMEOUT)

    def table_log(self, container: Path) -> int:
        info = self.run_codec("info", container)
        self.assertEqual(info.returncode, 0, info.stderr)
        line = next(row for row in info.stdout.decode().splitlines() if row.startswith("context table"))
        return int(line.split("2^")[1].split()[0])

    def test_default_is_minus_nine_and_the_last_preset_wins(self) -> None:
        plain = self.root / "plain.adpt"
        nine = self.root / "nine.adpt"
        small = self.root / "small.adpt"
        for out, flags in ((plain, ()), (nine, ("-9",)), (small, ("-9", "-3"))):
            r = self.run_codec("encode", self.source, "-o", out, "--no-verify", *flags)
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(payload_stream(plain.read_bytes()), payload_stream(nine.read_bytes()))
        self.assertLess(self.table_log(small), self.table_log(nine))
        self.assertGreater(small.stat().st_size, nine.stat().st_size)

    def test_tiers_above_xz_are_accepted_and_change_nothing_a_small_input_cannot_use(self) -> None:
        # A named input's table stops at its target, which -9 already
        # reaches for this one: -10 and -11 are accepted and write the same
        # bytes; --help names them.
        nine = self.root / "nine.adpt"
        r = self.run_codec("encode", self.source, "-o", nine, "--no-verify", "-9")
        self.assertEqual(r.returncode, 0, r.stderr)
        for preset in ("-10", "-11"):
            out = self.root / f"p{preset[1:]}.adpt"
            r = self.run_codec("encode", self.source, "-o", out, "--no-verify", preset)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(payload_stream(out.read_bytes()), payload_stream(nine.read_bytes()))
        help_run = self.run_codec("--help")
        self.assertIn(b"-10 and -11 are\n1 and 2 GiB", help_run.stdout + help_run.stderr)

    @LONG
    def test_tiers_above_xz_grow_a_pipes_model(self) -> None:
        # A pipe's length is unknown, so its target is the largest table,
        # and the two tiers buy a bigger model than -9: the max profile at
        # -9 holds 2^24 blocks, at -10 and -11 more.
        blocks = {}
        for preset in ("-9", "-10", "-11"):
            r = self.run_codec("encode", "-", "--max", "--allocation", "context", preset, data=self.source.read_bytes())
            self.assertEqual(r.returncode, 0, r.stderr)
            out = self.root / f"pipe{preset[1:]}.adpt"
            out.write_bytes(r.stdout)
            info = self.run_codec("info", out)
            self.assertEqual(info.returncode, 0, info.stderr)
            line = next(row for row in info.stdout.decode().splitlines() if row.startswith("context table"))
            blocks[preset] = self.blocks_of(line)
            decoded = self.run_codec("decode", "-", data=r.stdout)
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            self.assertEqual(decoded.stdout, self.source.read_bytes())
        self.assertLess(blocks["-9"], blocks["-10"])
        self.assertLess(blocks["-10"], blocks["-11"])

    @staticmethod
    def blocks_of(line: str) -> int:
        """The block count on an info line: '2^N blocks' or 'M blocks (...)'."""
        field = line.split()[2]
        return 1 << int(field[2:]) if field.startswith("2^") else int(field)

    def test_presets_order_the_model_and_the_lowest_clamp_to_the_floor(self) -> None:
        logs = {}
        for preset in ("-0", "-1", "-3", "-6", "-9"):
            out = self.root / f"p{preset[1]}.adpt"
            r = self.run_codec("encode", self.source, "-o", out, "--no-verify", preset)
            self.assertEqual(r.returncode, 0, r.stderr)
            logs[preset] = self.table_log(out)
            decoded = self.run_codec("decode", out, "-o", "-")
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            self.assertEqual(decoded.stdout, self.source.read_bytes())
        self.assertEqual(logs["-0"], logs["-1"])  # both below the floor: the smallest model
        self.assertLessEqual(logs["-1"], logs["-3"])
        self.assertLessEqual(logs["-3"], logs["-6"])
        self.assertLessEqual(logs["-6"], logs["-9"])
        self.assertLess(logs["-0"], logs["-9"])
        # the last preset given wins
        out = self.root / "last.adpt"
        r = self.run_codec("encode", self.source, "-o", out, "--no-verify", "-9", "-0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.table_log(out), logs["-0"])
        # a preset below the max profile's larger floor is raised to that
        out = self.root / "max0.adpt"
        r = self.run_codec("encode", self.source, "-o", out, "--no-verify", "-0", "--max")
        self.assertEqual(r.returncode, 0, r.stderr)
        decoded = self.run_codec("decode", out, "-o", "-")
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout, self.source.read_bytes())
        # a preset is an encode option only
        r = self.run_codec("decode", out, "-o", "-", "-9")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn(b"-9", r.stderr)


class SurfaceTest(unittest.TestCase):
    """Every command, option and refusal the CLI has, driven once."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.bin"
        self.source.write_bytes(b"surface of the command line, every option once\n" * 300)

    def run_codec(self, *args, data=None):
        return subprocess.run([str(CADAPT), *map(str, args)], input=data, capture_output=True, timeout=TIMEOUT)

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
            (("encode", self.source, "--max-memory", str(64 << 20)), b"--max-memory is a decode option"),
            (("encode", self.source, "-12"), b"unknown option -12"),
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
        # one segment: one worker, however many threads were offered
        self.assertIn(b"1 segments on 1 thread\n", r.stderr)
        # decode strips .adpt, here back onto the source; a name without it gains .out
        r = self.run_codec("decode", container, "--force", "--threads", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"1 segments on 1 thread\n", r.stderr)
        self.assertEqual(self.source.read_bytes(), original)
        other = self.root / "renamed"
        other.write_bytes(container.read_bytes())
        r = self.run_codec("decode", other)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.root / "renamed.out").read_bytes(), original)
        self.assertEqual(self.run_codec("decode", other).returncode, 1)
        # pipes both ways, with the thread report for a pipe
        r = self.run_codec("decode", "-", "--threads", "2", data=container.read_bytes())
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"1 segments on 1 thread\n", r.stderr)
        self.assertEqual(r.stdout, original)
        r = self.run_codec("encode", "-", "--threads", "2", "--no-verify", data=original)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"1 segments on 1 thread\n", r.stderr)
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
            (("encode", self.source, "--max-memory", "64M"), b"--max-memory is a decode option; encode sets its budget with a preset, -0 .. -11"),
            (("decode", self.source, "--max-memory", ""), b"--max-memory: not a byte count"),
            (("decode", self.source, "--max-memory", "99999999999999999999999"), b"--max-memory: not a byte count"),
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
        # --threads is reported: two segments take two workers of the four
        run = self.run_codec("decode", VECTORS / "segmented.adpt", "-o", "-", "--threads", "4")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"2 segments on 2 threads", run.stderr)
        self.assertEqual(run.stdout, (VECTORS / "segmented.bin").read_bytes())
        run = self.run_codec("decode", "-", "--threads", "4", data=encoded.read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"1 segments on 1 thread\n", run.stderr)

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
            "text.adpt": (b"format          adapt container version 19", b"declared bytes  49152", b"class history   8 bytes", b"contexts        all but field, record", b"profile         normal"),
            "text-fast.adpt": (b"format          adapt container version 20", b"contexts        o2, o4", b"profile         fast"),
            "text-max.adpt": (b"format          adapt container version 21", b"contexts        all but word, field\n", b"profile         max\n", b"arbiter start   1/4\n"),
            "text-compact.adpt": (b"format          adapt container version 53", b"contexts        all but word, field\n", b"profile         max (compact side tables)\n"),
            "text-lean.adpt": (b"format          adapt container version 54", b"contexts        all but word, field, o8, word3, wordc\n", b"profile         max (compact side tables, lean contexts)\n"),
            "text-words.adpt": (b"format          adapt container version 62", b"profile         normal\n", b"text transform  yes\n"),
            "records.adpt": (b"record period   192 bytes", b"class history   21 bytes", b"contexts        all"),
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
        for flags, version in (((), 22), (("--fast",), 23), (("--max",), 24)):
            with self.subTest(flags=flags):
                run = self.run_codec("encode", text, "-o", "-", *flags)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stdout[:4], b"ADPT")
                self.assertEqual(run.stdout[4], version)
                self.assertRegex(run.stderr, rb"^49152 bytes -> \d+ bytes \(\d+\.\d\d%\), not verified")
        # segments from a file and from a pipe, with their reports
        run = self.run_codec("encode", VECTORS / "segmented.bin", "-o", "-", "--threads", "2", "--segment-bytes", "262144")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b", 2 segments on 2 threads\n", run.stderr)
        self.assertEqual(payload_stream(run.stdout), (VECTORS / "segmented.adpt").read_bytes())
        run = self.run_codec("encode", "-", "--threads", "2", "--segment-bytes", "262144", data=(VECTORS / "segmented.bin").read_bytes())
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b"2 segments on 2 threads", run.stderr)
        decoded = self.run_codec("decode", "-", data=run.stdout)
        self.assertEqual(decoded.stdout, (VECTORS / "segmented.bin").read_bytes())
        # the lowest preset is raised to the smallest model, never refused
        run = self.run_codec("encode", text, "-o", self.root / "tiny.adpt", "-0")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue((self.root / "tiny.adpt").exists())
        # the proof alongside with segments, and the empty input
        run = self.run_codec("encode", VECTORS / "segmented.bin", "-o", self.root / "seg.adpt", "--threads", "2", "--segment-bytes", "262144", "--verify")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(b", verified, 2 segments on 2 threads", run.stderr)
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
