"""Hostile archive paths and bounded threaded input buffering.

Source: OpenAI GPT-6 via Codex. Fixtures exercise the compiled command,
including its filesystem sink and refusal cleanup, without a model encoder.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path

CADAPT = Path(__file__).resolve().parent.parent / "build/cadapt"
HAS_THREADS = os.environ.get("ADAPT_TEST_THREADS", "1") != "0"
HEADER = b"ADPT" + bytes((19, 10, 10, 16))
BLOCK = b"R" * (256 << 10)


def name(value):
    value = str(value).encode()
    return struct.pack("<H", len(value)) + value


def entry(kind, path, value=b"", *, size=4096, extents=()):
    prefix = bytes((kind,)) + name(path)
    if kind in (3, 4):
        prefix += name(value)
    elif kind == 2:
        prefix += struct.pack("<Q", len(value))
    elif kind == 5:
        prefix += struct.pack("<QI", size, len(extents))
    if kind == 4:
        return prefix
    mode = 0o777 if kind in (1, 3) else 0o600
    prefix += struct.pack("<IqIqI", mode, 1700000000, 0, 1700000000, 0)
    data = value if kind == 2 else b""
    if kind == 5:
        data = b"".join(struct.pack("<QQ", offset, len(chunk)) + chunk for offset, chunk in extents)
    return prefix + data + b"\0\0"


def archive(entries, corrupt=False):
    raw = b"ADPTARC3" + b"".join(entries) + b"\0"
    header = HEADER[:5] + bytes((HEADER[5] | 128,)) + HEADER[6:]
    return (header + struct.pack("<BII", 2, len(raw), len(raw)) + raw
            + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw) ^ corrupt))


def segmented_file(path, blocks):
    checksum = 0
    with path.open("wb") as out:
        out.write(HEADER[:7] + bytes((HEADER[7] | 64,)))
        for segment in range(2):
            if segment:
                out.write(b"\x04")
            for _ in range(blocks):
                out.write(struct.pack("<BII", 2, len(BLOCK), len(BLOCK)))
                out.write(BLOCK)
                checksum = zlib.crc32(BLOCK, checksum)
        raw, coded = blocks * len(BLOCK), blocks * (len(BLOCK) + 9)
        out.write(b"\x05" + struct.pack("<IIIII", raw, coded, raw, coded + 1, 2))
        out.write(struct.pack("<BQI", 3, 2 * raw, checksum))


class SafetyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def decode(self, source="-", data=None, *options, memory_limit=None):
        def constrain():
            import resource
            resource.setrlimit(resource.RLIMIT_AS, (memory_limit, memory_limit))

        return subprocess.run(
            [str(CADAPT), "decode", str(source), *map(str, options)], input=data,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=60,
            preexec_fn=constrain if memory_limit is not None else None,
        )

    @unittest.skipIf(os.name == "nt", "POSIX staging permissions")
    def test_private_file_stays_private_until_publication(self):
        raw = b"private decoded bytes\n" * 512
        header = b"ADPT" + bytes((22, 10, 10, 16))
        header += struct.pack("<IqIqI", 0o600, 1700000000, 0, 1700000000, 0)
        header += struct.pack("<I", zlib.crc32(header))
        for corrupt in (False, True):
            with self.subTest(corrupt=corrupt):
                output = self.root / f"private-{corrupt}"
                with subprocess.Popen(
                    [str(CADAPT), "decode", "-", "-o", str(output)],
                    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, umask=0o022,
                ) as process:
                    try:
                        process.stdin.write(header + struct.pack("<BII", 2, len(raw), len(raw)) + raw)
                        process.stdin.flush()
                        deadline = time.monotonic() + 10
                        while time.monotonic() < deadline:
                            staged = list(self.root.glob(f".cadapt-{process.pid}-*.part"))
                            if staged and staged[0].stat().st_size >= 4096:
                                break
                            self.assertIsNone(process.poll(), "decoder exited before trailer")
                            time.sleep(0.01)
                        else:
                            self.fail("decoder did not stage the data block")
                        self.assertEqual(staged[0].stat().st_mode & 0o777, 0o600)
                        self.assertFalse(output.exists())
                        process.stdin.write(struct.pack("<BQI", 3, len(raw), zlib.crc32(raw) ^ corrupt))
                        process.stdin.close()
                        process.wait(timeout=10)
                        self.assertEqual(process.returncode, int(corrupt), process.stderr.read())
                    finally:
                        if process.poll() is None:
                            process.stdin.close()
                            process.wait(timeout=10)
                if not corrupt:
                    self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                    self.assertEqual(output.read_bytes(), raw)
                else:
                    self.assertFalse(output.exists())
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux root xattrs and ACLs")
    def test_folder_root_attributes_and_acls_survive_verified_roundtrip(self):
        folder = self.root / "source"
        folder.mkdir()
        (folder / "child").write_bytes(b"child bytes")
        (folder / "sub").mkdir()
        # Linux POSIX ACL xattr version 2: owner, named user, group, mask, other.
        acl = struct.pack("<I", 2) + b"".join(
            struct.pack("<HHI", tag, permissions, uid)
            # The current uid is also mapped inside restricted user namespaces.
            for tag, permissions, uid in ((1, 7, 0xffffffff), (2, 5, os.getuid()),
                                          (4, 0, 0xffffffff), (16, 5, 0xffffffff),
                                          (32, 0, 0xffffffff))
        )
        attributes = {"user.review": b"root\x00value", "user.empty": b"",
                      "system.posix_acl_access": acl, "system.posix_acl_default": acl}
        for name, value in attributes.items():
            os.setxattr(folder, name, value)
        os.setxattr(folder / "child", "user.child", b"unchanged")
        os.setxattr(folder / "sub", "user.directory", b"nested")
        os.utime(folder, ns=(1700000000123456789, 1700000000987654321))
        expected = folder.stat()
        encoded = subprocess.run([str(CADAPT), "encode", str(folder), "--fast", "-3", "--verify",
                                  "-o", str(self.root / "folder.adpt")], capture_output=True, timeout=30)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        restored = self.root / "restored"
        decoded = self.decode(self.root / "folder.adpt", None, "-o", restored)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        actual = restored.stat()
        self.assertEqual((actual.st_mode, actual.st_atime_ns, actual.st_mtime_ns),
                         (expected.st_mode, expected.st_atime_ns, expected.st_mtime_ns))
        for name, value in attributes.items():
            self.assertEqual(os.getxattr(restored, name), value, name)
        self.assertEqual(os.getxattr(restored / "child", "user.child"), b"unchanged")
        self.assertEqual(os.getxattr(restored / "sub", "user.directory"), b"nested")
        # Applying the root default ACL before children would add permissions
        # which those children never had in the source tree.
        self.assertEqual(os.listxattr(restored / "child"), os.listxattr(folder / "child"))
        self.assertEqual(os.listxattr(restored / "sub"), os.listxattr(folder / "sub"))

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux root attributes")
    def test_root_attributes_on_empty_and_segmented_folders(self):
        folder = self.root / "source"
        folder.mkdir()
        os.setxattr(folder, "user.root", b"preserved")
        for segmented in (False, True):
            with self.subTest(segmented=segmented):
                if segmented:
                    (folder / "data").write_bytes(bytes(range(256)) * 2048)
                output = self.root / f"folder-{segmented}.adpt"
                args = [str(CADAPT), "encode", str(folder), "--fast", "-5", "--verify", "-o", str(output)]
                if segmented:
                    args += ["--threads", "2", "--segment-bytes", str(256 << 10)]
                encoded = subprocess.run(args, capture_output=True, timeout=30)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                restored = self.root / f"restored-{segmented}"
                decoded = self.decode(output, None, "-o", restored, "--threads", "2")
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertEqual(os.getxattr(restored, "user.root"), b"preserved")
                self.assertEqual(sorted(p.name for p in restored.iterdir()), ["data"] if segmented else [])
                if segmented:
                    self.assertEqual((restored / "data").read_bytes(), (folder / "data").read_bytes())

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux attribute refusal")
    def test_root_attribute_failure_discards_staging(self):
        for corrupt, attr_name in ((True, b"user.root"), (False, b"invalid.root")):
            with self.subTest(corrupt=corrupt, attribute=attr_name):
                attribute = struct.pack("<H", len(attr_name)) + attr_name + struct.pack("<I", 1) + b"x"
                raw = b"ADPTARC4\x01\x00" + attribute + b"\x00"
                header = HEADER[:5] + bytes((HEADER[5] | 128,)) + HEADER[6:]
                data = (header + struct.pack("<BII", 2, len(raw), len(raw)) + raw
                        + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw) ^ corrupt))
                output = self.root / "existing"
                output.write_bytes(b"keep this file")
                decoded = self.decode("-", data, "-o", output, "--force")
                self.assertEqual(decoded.returncode, 1, decoded.stderr)
                self.assertIn(b"checksum" if corrupt else b"cannot restore root extended attribute", decoded.stderr)
                self.assertEqual(output.read_bytes(), b"keep this file")
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX symlinks and hard links")
    def test_archive_refuses_symlink_parents_for_every_entry_kind(self):
        for kind in (1, 2, 3, 4, 5):
            for corrupt in (False, True):
                with self.subTest(kind=kind, corrupt=corrupt):
                    case = self.root / f"{kind}-{corrupt}"
                    case.mkdir()
                    outside = case / "outside"
                    outside.mkdir()
                    payload = b"test bytes" if kind == 2 else "safe"
                    entries = [entry(2, "safe", b"source"), entry(3, "escape", outside),
                               entry(kind, "escape/created", payload)]
                    run = self.decode("-", archive(entries, corrupt), "-o", case / "restored")
                    self.assertNotEqual(run.returncode, 0)
                    self.assertEqual(list(outside.iterdir()), [], run.stderr)
                    self.assertFalse((case / "restored").exists())
                    self.assertEqual(list(case.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX symlinks and hard links")
    def test_hardlink_target_must_be_a_regular_file_inside_the_archive(self):
        outside = self.root / "outside"
        outside.mkdir()
        anchor = outside / "anchor"
        anchor.write_bytes(b"untouched")
        for target in ("escape/anchor", "escape"):
            with self.subTest(target=target):
                destination = self.root / target.replace("/", "-")
                run = self.decode("-", archive([entry(3, "escape", outside),
                                               entry(4, "alias", target)]), "-o", destination)
                self.assertNotEqual(run.returncode, 0)
                self.assertFalse(destination.exists())
                self.assertEqual(anchor.stat().st_nlink, 1)
                self.assertEqual(anchor.read_bytes(), b"untouched")
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX symlinks and hard links")
    def test_safe_nested_files_and_links_still_restore(self):
        destination = self.root / "restored"
        run = self.decode("-", archive([entry(1, "dir"), entry(2, "dir/data", b"source"),
                                       entry(4, "alias", "dir/data"), entry(3, "link", "dir/data"),
                                       entry(3, "outside-link", "../../elsewhere")]), "-o", destination)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual((destination / "dir/data").read_bytes(), b"source")
        self.assertEqual((destination / "dir/data").stat().st_ino, (destination / "alias").stat().st_ino)
        self.assertEqual(os.readlink(destination / "outside-link"), "../../elsewhere")

    @unittest.skipIf(os.name == "nt", "POSIX path-length and cleanup behavior")
    def test_paths_too_long_for_metadata_and_cleanup_leave_no_staging_tree(self):
        limit = os.pathconf(self.root, "PC_PATH_MAX")
        entries = []
        path = ""
        while len(path) < limit:
            path = path + "/" + "d" * 120 if path else "d" * 120
            entries.append(entry(1, path))
        destination = self.root / "deep"
        run = self.decode("-", archive(entries), "-o", destination)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux address-space limit")
    def test_large_segmented_file_falls_back_without_buffering_the_container(self):
        source = self.root / "large.adpt"
        segmented_file(source, 80)
        for piped in (False, True):
            with self.subTest(piped=piped):
                run = self.decode("-" if piped else source, source.read_bytes() if piped else None,
                                  "-o", "-", "--threads", "2", "--max-memory", 32 << 20,
                                  memory_limit=48 << 20)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn(b"checksum verified, 2 segments on 1 thread", run.stderr)

    def test_buffered_file_decodes_with_one_or_two_workers_within_budget(self):
        source = self.root / "small.adpt"
        segmented_file(source, 1)
        for budget, workers in ((32 << 20, 1), (96 << 20, 2)):
            with self.subTest(budget=budget):
                run = self.decode(source, None, "-o", "-", "--threads", "2", "--max-memory", budget)
                self.assertEqual(run.returncode, 0, run.stderr)
                expected = workers if HAS_THREADS else 1
                self.assertIn(f"checksum verified, 2 segments on {expected} thread".encode(), run.stderr)

    def test_memory_limit_is_checked_before_reading_segment_payload(self):
        data = HEADER[:7] + bytes((HEADER[7] | 64,)) + b"invalid payload"
        run = self.decode("-", data, "--threads", "2", "--max-memory", "1")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn(b"memory budget exceeded", run.stderr)

    def test_sparse_restoration_cannot_exceed_output_limit(self):
        for limit, size in ((1 << 30, (1 << 30) + 1), (1024, 1025), (1024, (1 << 64) - 1)):
            with self.subTest(limit=limit, size=size):
                destination = self.root / f"restored-{size}"
                run = self.decode("-", archive([entry(5, "holes", size=size)]),
                                  "-o", destination, "--max-output", limit)
                self.assertEqual(run.returncode, 1, run.stderr)
                self.assertIn(b"restored file sizes exceed --max-output", run.stderr)
                self.assertFalse(destination.exists())
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    def test_output_limit_applies_only_when_given(self):
        # FMT-6: a valid container decodes whatever it declares; the cap is
        # the caller's. A folder whose one file is a hole past 1 GiB is
        # restored to that apparent length without --max-output.
        destination = self.root / "restored-uncapped"
        run = self.decode("-", archive([entry(5, "holes", size=(1 << 30) + 1)]), "-o", destination)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual((destination / "holes").stat().st_size, (1 << 30) + 1)
        self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    def test_restoration_limit_counts_files_holes_and_hardlink_aliases_together(self):
        cases = [
            [entry(5, "a", size=512), entry(5, "b", size=513)],
            [entry(2, "a", b"a" * 256), entry(5, "b", size=769)],
            [entry(5, "a", size=900), entry(2, "b", b"b" * 125)],
            [entry(5, "a", size=512), entry(4, "b", "a"), entry(4, "c", "a")],
        ]
        for index, entries in enumerate(cases):
            with self.subTest(case=index):
                destination = self.root / "existing"
                destination.mkdir(exist_ok=True)
                sentinel = destination / "keep"
                sentinel.write_bytes(b"unchanged")
                run = self.decode("-", archive(entries), "-o", destination,
                                  "--force", "--max-output", 1024)
                self.assertEqual(run.returncode, 1, run.stderr)
                self.assertIn(b"restored file sizes exceed --max-output", run.stderr)
                self.assertEqual(list(destination.iterdir()), [sentinel])
                self.assertEqual(sentinel.read_bytes(), b"unchanged")
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    def test_restoration_accepts_exact_output_limit_without_counting_extents_twice(self):
        cases = [
            ([entry(2, "a", b"abc"), entry(5, "b", size=1021, extents=((1018, b"xyz"),))], 1021),
            ([entry(5, "a", size=512), entry(4, "b", "a")], 512),
            ([entry(5, "a", size=0), entry(5, "b", size=1024)], 1024),
        ]
        for index, (entries, length) in enumerate(cases):
            destination = self.root / str(index)
            run = self.decode("-", archive(entries), "-o", destination, "--max-output", 1024)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual((destination / "b").stat().st_size, length)
        self.assertEqual((self.root / "0/b").read_bytes(), bytes(1018) + b"xyz")


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        CADAPT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
