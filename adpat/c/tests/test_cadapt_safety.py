"""Hostile archive paths and bounded threaded input buffering.

Source: OpenAI GPT-6 via Codex. Fixtures exercise the compiled command,
including its filesystem sink and refusal cleanup, without a model encoder.
"""

import os
import random
import select
import signal
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


def entry(kind, path, value=b"", *, size=4096, extents=(), mode=None):
    prefix = bytes((kind,)) + name(path)
    if kind in (3, 4):
        prefix += name(value)
    elif kind == 2:
        prefix += struct.pack("<Q", len(value))
    elif kind == 5:
        prefix += struct.pack("<QI", size, len(extents))
    if kind == 4:
        return prefix
    if mode is None:
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

    def wait_for_stage(self, process, child=None):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            staged = list(self.root.glob(f".cadapt-{process.pid}-*.part"))
            if staged and (child is None or (staged[0] / child).exists()
                           or (staged[0] / "contents" / child).exists()):
                return staged[0]
            self.assertIsNone(process.poll(), "codec exited before staging")
            time.sleep(0.01)
        self.fail("codec did not create staging")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux publication fault injection")
    def test_forced_folder_publication_preserves_target_on_rename_failure(self):
        source = self.root / "fail-rename.c"
        library = self.root / "fail-rename.so"
        source.write_text("""#include <errno.h>
#include <stdlib.h>
int renameat(int a, const char *b, int c, const char *d) {
    (void)a; (void)b; (void)c; (void)d;
    errno = getenv("CADAPT_TEST_EXCHANGE_FAILURE") ? ENOTDIR : EIO;
    return -1;
}
int renameat2(int a, const char *b, int c, const char *d, unsigned int flags) {
    (void)a; (void)b; (void)c; (void)d; (void)flags;
    errno = getenv("CADAPT_TEST_EXCHANGE_UNSUPPORTED") ? ENOSYS : EIO;
    return -1;
}
""")
        subprocess.run(["cc", "-shared", "-fPIC", str(source), "-o", str(library)],
                       check=True, capture_output=True, timeout=30)
        for exchange_failure in (False, True, "unsupported"):
            for kind in ("file", "directory", "symlink"):
                with self.subTest(kind=kind, exchange_failure=exchange_failure):
                    target = self.root / f"target-{kind}-{exchange_failure}"
                    if kind == "directory":
                        target.mkdir()
                    elif kind == "symlink":
                        target.symlink_to("unresolved-original")
                    else:
                        target.write_bytes(b"original valuable content")
                    before = target.lstat()
                    env = dict(os.environ, LD_PRELOAD=str(library))
                    if exchange_failure:
                        env["CADAPT_TEST_EXCHANGE_FAILURE"] = "1"
                    if exchange_failure == "unsupported":
                        env["CADAPT_TEST_EXCHANGE_UNSUPPORTED"] = "1"
                    run = subprocess.run([str(CADAPT), "decode", "-", "-o", str(target), "--force"],
                                         input=archive([entry(2, "new", b"replacement")]),
                                         capture_output=True, env=env, timeout=30)
                    self.assertEqual(run.returncode, 1, run.stderr)
                    self.assertTrue(os.path.lexists(target), run.stderr)
                    self.assertEqual(target.lstat().st_ino, before.st_ino, run.stderr)
                    if kind == "file":
                        self.assertEqual(target.read_bytes(), b"original valuable content")
                    elif kind == "symlink":
                        self.assertEqual(os.readlink(target), "unresolved-original")
                    else:
                        self.assertEqual(list(target.iterdir()), [])
                    self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith(("linux", "darwin")), "atomic POSIX exchange")
    def test_forced_folder_publication_replaces_files_links_and_empty_directories(self):
        victim = self.root / "victim"
        victim.write_bytes(b"unrelated")
        for kind in ("file", "directory", "symlink"):
            with self.subTest(kind=kind):
                target = self.root / f"target-{kind}"
                if kind == "directory":
                    target.mkdir()
                elif kind == "symlink":
                    target.symlink_to(victim)
                else:
                    target.write_bytes(b"old")
                decoded = self.decode("-", archive([entry(2, "new", b"replacement")]),
                                      "-o", target, "--force")
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                self.assertFalse(target.is_symlink())
                self.assertEqual((target / "new").read_bytes(), b"replacement")
                self.assertEqual(victim.read_bytes(), b"unrelated")
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux access ACLs and user attributes")
    def test_read_only_acls_and_user_attributes_roundtrip_together(self):
        for kind in ("file", "sparse", "directory", "root"):
            with self.subTest(kind=kind):
                source = self.root / f"source-{kind}"
                source.mkdir()
                item = source if kind == "root" else source / "child"
                if kind == "file":
                    item.write_bytes(b"payload")
                elif kind == "sparse":
                    with item.open("wb") as out:
                        out.seek(1 << 20)
                        out.write(b"payload")
                elif kind == "directory":
                    item.mkdir()
                owner = 4 if kind in ("file", "sparse") else 5
                acl = struct.pack("<I", 2) + b"".join(
                    struct.pack("<HHI", tag, permissions, uid)
                    for tag, permissions, uid in ((1, owner, 0xffffffff), (2, 4, os.getuid()),
                                                  (4, 0, 0xffffffff), (16, 4, 0xffffffff),
                                                  (32, 0, 0xffffffff))
                )
                attributes = {"user.keep": b"important metadata", "user.empty": b""}
                if kind in ("directory", "root"):
                    attributes["system.posix_acl_default"] = acl
                for key, value in attributes.items():
                    os.setxattr(item, key, value)
                os.setxattr(item, "system.posix_acl_access", acl)
                mode = item.stat().st_mode & 0o777
                self.addCleanup(item.chmod, 0o700)
                container = self.root / f"{kind}.adpt"
                encoded = subprocess.run([str(CADAPT), "encode", str(source), "-o", str(container)],
                                         capture_output=True, timeout=30)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                destination = self.root / f"restored-{kind}"
                decoded = self.decode(container, None, "-o", destination)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                restored = destination if kind == "root" else destination / "child"
                self.addCleanup(restored.chmod, 0o700)
                self.assertEqual(restored.stat().st_mode & 0o777, mode)
                self.assertEqual(os.getxattr(restored, "system.posix_acl_access"), acl)
                for key, value in attributes.items():
                    self.assertEqual(os.getxattr(restored, key), value)
                if kind in ("file", "sparse"):
                    self.assertEqual(restored.read_bytes(), item.read_bytes())

    @unittest.skipUnless(HAS_THREADS, "threaded encoder")
    def test_empty_threaded_container_decodes_with_either_thread_count(self):
        source = self.root / "empty"
        source.write_bytes(b"")
        for profile in ((), ("--fast",), ("--max",), ("--transform", "text")):
            with self.subTest(profile=profile):
                container = self.root / "empty.adpt"
                encoded = subprocess.run([str(CADAPT), "encode", str(source), "-o", str(container),
                                          "--force", "--threads", "2", *profile],
                                         capture_output=True, timeout=30)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                for threads in (1, 2):
                    destination = self.root / f"empty-{threads}"
                    decoded = self.decode(container, None, "-o", destination, "--force", "--threads", threads)
                    self.assertEqual(decoded.returncode, 0, decoded.stderr)
                    self.assertEqual(destination.read_bytes(), b"")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux resource limits")
    def test_deferred_root_and_directory_attributes_use_bounded_memory(self):
        # The checksum is deliberately wrong: no attribute should be applied,
        # but the entire metadata stream must fit without retaining its values.
        for root_attributes in (False, True):
            with self.subTest(root_attributes=root_attributes):
                raw = bytearray(b"ADPTARC4" if root_attributes else b"ADPTARC3")
                if root_attributes:
                    raw += struct.pack("<H", 1024)
                for i in range(1024):
                    if not root_attributes:
                        raw += entry(1, f"d{i:04}")[:-2] + b"\x01\x00"
                    raw += name(f"user.a{i:04}") + struct.pack("<I", 65536) + b"x" * 65536
                raw += b"\0"
                source = self.root / "metadata.adpt"
                with source.open("wb") as out:
                    out.write(HEADER[:5] + bytes((HEADER[5] | 128,)) + HEADER[6:])
                    for at in range(0, len(raw), 256 << 10):
                        chunk = raw[at:at + (256 << 10)]
                        out.write(struct.pack("<BII", 2, len(chunk), len(chunk)) + chunk)
                    out.write(struct.pack("<BQI", 3, len(raw), zlib.crc32(raw) ^ 1))
                del raw
                destination = self.root / "output"
                run = self.decode(source, None, "-o", destination, "--max-memory", 32 << 20,
                                  memory_limit=64 << 20)
                self.assertEqual(run.returncode, 1, run.stderr)
                self.assertIn(b"checksum mismatch", run.stderr)
                self.assertFalse(destination.exists())
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux file-size limits")
    def test_metadata_journal_storage_failure_cleans_staging(self):
        import resource

        def constrain():
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        attribute = name("user.large") + struct.pack("<I", 32768) + b"x" * 32768
        destination = self.root / "output"
        run = subprocess.run([str(CADAPT), "decode", "-", "-o", str(destination)],
                             input=archive([entry(1, "dir")[:-2] + b"\x01\x00" + attribute]),
                             capture_output=True, preexec_fn=constrain, timeout=30)
        self.assertEqual(run.returncode, 128 + signal.SIGXFSZ, run.stderr)
        self.assertIn(b"cannot defer directory metadata", run.stderr)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux file-size limits")
    def test_legacy_compatibility_storage_failure_emits_nothing(self):
        import resource

        def constrain():
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        fixture = Path(__file__).with_name("vectors") / "legacy-max2-always.adpt"
        run = subprocess.run([str(CADAPT), "decode", str(fixture), "-o", "-"],
                             capture_output=True, preexec_fn=constrain, timeout=30)
        self.assertEqual(run.returncode, 128 + signal.SIGXFSZ, run.stderr)
        self.assertEqual(run.stdout, b"")

    @unittest.skipIf(os.name == "nt", "POSIX staging identities")
    def test_file_staging_replacement_cannot_redirect_publication_or_metadata(self):
        raw = b"verified file contents"
        for corrupt in (False, True):
            for force in (False, True):
                with self.subTest(corrupt=corrupt, force=force):
                    case = self.root / f"case-{corrupt}-{force}"
                    case.mkdir()
                    victim = case / "victim"
                    victim.write_bytes(b"unrelated")
                    victim.chmod(0o600)
                    os.utime(victim, ns=(1600000000000000000, 1600000000000000000))
                    expected = victim.stat()
                    output = case / "output"
                    if force:
                        output.write_bytes(b"existing")
                    header = b"ADPT" + bytes((22, 10, 10, 16))
                    header += struct.pack("<IqIqI", 0o644, 1700000000, 0, 1700000000, 0)
                    header += struct.pack("<I", zlib.crc32(header))
                    command = [str(CADAPT), "decode", "-", "-o", str(output)]
                    if force:
                        command.append("--force")
                    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.addCleanup(lambda p=process: p.poll() is None and p.kill())
                    process.stdin.write(header + struct.pack("<BII", 2, len(raw), len(raw)) + raw)
                    process.stdin.flush()
                    deadline = time.monotonic() + 10
                    while True:
                        stages = list(case.glob(".cadapt-*.part"))
                        if stages:
                            stage = stages[0]
                            content = stage / "contents" if stage.is_dir() else stage
                            if content.is_file() and content.stat().st_size == len(raw):
                                break
                        self.assertIsNone(process.poll())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
                    moved = case / "moved-stage"
                    stage.rename(moved)
                    stage.symlink_to(victim)
                    _, stderr = process.communicate(struct.pack("<BQI", 3, len(raw), zlib.crc32(raw) ^ corrupt),
                                                    timeout=30)
                    actual = victim.stat()
                    self.assertEqual((actual.st_mode, actual.st_mtime_ns),
                                     (expected.st_mode, expected.st_mtime_ns))
                    self.assertEqual(victim.read_bytes(), b"unrelated")
                    self.assertTrue(stage.is_symlink(), "cleanup removed an entry it did not create")
                    if corrupt:
                        self.assertNotEqual(process.returncode, 0, stderr)
                        self.assertEqual(output.read_bytes() if force else output.exists(),
                                         b"existing" if force else False)
                    else:
                        self.assertEqual(process.returncode, 0, stderr)
                        self.assertFalse(output.is_symlink())
                        self.assertEqual(output.read_bytes(), raw)
                    if moved.is_dir():
                        self.assertEqual(list(moved.iterdir()), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "POSIX staging identity and Linux xattrs")
    def test_replaced_staging_name_cannot_redirect_restore_cleanup_or_publication(self):
        attribute = name("user.identity") + struct.pack("<I", 8) + b"archived"
        first = (b"ADPTARC4\x01\x00" + attribute + entry(1, "nested")
                 + entry(2, "nested/first", b"first"))
        last_file = entry(2, "nested/last", b"last")[:-2] + b"\x01\x00" + attribute
        last = last_file + b"\x00"
        header = b"ADPT" + bytes((22, 10 | 128, 10, 16))
        header += struct.pack("<IqIqI", 0o500, 1700000000, 0, 1700000000, 0)
        header += struct.pack("<I", zlib.crc32(header))
        for corrupt in (False, True):
            for replacement in ("symlink", "directory"):
                with self.subTest(corrupt=corrupt, replacement=replacement):
                    victim = self.root / f"victim-{corrupt}-{replacement}"
                    victim.mkdir(mode=0o750)
                    (victim / "keep").write_bytes(b"untouched")
                    (victim / "nested").mkdir()
                    (victim / "nested/last").write_bytes(b"also untouched")
                    os.setxattr(victim, "user.identity", b"original")
                    before = victim.stat()
                    target = self.root / f"restored-{corrupt}-{replacement}"
                    options = ["--force"] if replacement == "directory" else []
                    if options:
                        target.write_bytes(b"existing target")
                    with subprocess.Popen([str(CADAPT), "decode", "-", "-o", str(target), *options],
                                          stdin=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                        process.stdin.write(header + struct.pack("<BII", 2, len(first), len(first)) + first)
                        process.stdin.flush()
                        stage = self.wait_for_stage(process, "nested/first")
                        self.assertEqual(stage.stat().st_mode & 0o777, 0o700)
                        moved = stage.with_name(stage.name + ".moved")
                        stage.rename(moved)
                        if replacement == "symlink":
                            stage.symlink_to(victim, target_is_directory=True)
                        else:
                            victim.rename(stage)
                            victim = stage
                        process.stdin.write(struct.pack("<BII", 2, len(last), len(last)) + last)
                        process.stdin.write(struct.pack("<BQI", 3, len(first + last),
                                                        zlib.crc32(first + last) ^ corrupt))
                        process.stdin.close()
                        process.wait(timeout=10)
                        diagnostic = process.stderr.read()
                    self.assertEqual((victim / "keep").read_bytes(), b"untouched", diagnostic)
                    self.assertEqual((victim / "nested/last").read_bytes(), b"also untouched", diagnostic)
                    self.assertEqual(victim.stat().st_mode, before.st_mode)
                    self.assertEqual(victim.stat().st_mtime_ns, before.st_mtime_ns)
                    self.assertEqual(os.getxattr(victim, "user.identity"), b"original")
                    self.assertEqual(process.returncode, int(corrupt), diagnostic)
                    if corrupt:
                        if options:
                            self.assertEqual(target.read_bytes(), b"existing target")
                        else:
                            self.assertFalse(target.exists())
                    else:
                        self.assertEqual(target.stat().st_mode & 0o777, 0o500)
                        self.assertEqual((target / "nested/first").read_bytes(), b"first")
                        self.assertEqual((target / "nested/last").read_bytes(), b"last")
                        self.assertEqual(os.getxattr(target, "user.identity"), b"archived")
                        self.assertEqual(os.getxattr(target / "nested/last", "user.identity"), b"archived")
                    # The renamed private wrapper may remain, but its contents
                    # must have been removed or published via the retained handle.
                    self.assertEqual(list(moved.iterdir()), [])

    @unittest.skipIf(os.name == "nt", "POSIX restrictive metadata and cleanup")
    def test_failed_publication_cleans_directories_after_permissions_are_applied(self):
        raw = b"ADPTARC3" + entry(1, "locked", mode=0) + entry(2, "locked/file", b"data", mode=0) + b"\0"
        header = b"ADPT" + bytes((22, 10 | 128, 10, 16))
        header += struct.pack("<IqIqI", 0, 1700000000, 0, 1700000000, 0)
        header += struct.pack("<I", zlib.crc32(header))
        target = self.root / "existing"
        with subprocess.Popen([str(CADAPT), "decode", "-", "-o", str(target), "--force"],
                              stdin=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            process.stdin.write(header + struct.pack("<BII", 2, len(raw), len(raw)) + raw)
            process.stdin.flush()
            self.wait_for_stage(process, "locked/file")
            target.mkdir()
            (target / "keep").write_bytes(b"keep")
            process.stdin.write(struct.pack("<BQI", 3, len(raw), zlib.crc32(raw)))
            process.stdin.close()
            process.wait(timeout=10)
            self.assertEqual(process.returncode, 1, process.stderr.read())
        self.assertEqual((target / "keep").read_bytes(), b"keep")
        self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux inherited ACLs")
    def test_destination_default_acl_does_not_add_restored_permissions(self):
        parent = self.root / "acl-parent"
        parent.mkdir()
        acl = struct.pack("<I", 2) + b"".join(
            struct.pack("<HHI", tag, permissions, uid)
            for tag, permissions, uid in ((1, 7, 0xffffffff), (2, 7, os.getuid()),
                                          (4, 5, 0xffffffff), (16, 7, 0xffffffff),
                                          (32, 0, 0xffffffff))
        )
        os.setxattr(parent, "system.posix_acl_default", acl)
        for kind in ("file", "folder", "empty-folder"):
            with self.subTest(kind=kind):
                source = self.root / kind
                if kind == "file":
                    source.write_bytes(b"private contents")
                    source.chmod(0o640)
                    paths = [""]
                else:
                    source.mkdir(mode=0o750)
                    paths = [""]
                    if kind == "folder":
                        (source / "nested").mkdir(mode=0o750)
                        (source / "nested/secret").write_bytes(b"private contents")
                        (source / "nested/secret").chmod(0o640)
                        paths += ["nested", "nested/secret"]
                encoded_path = self.root / f"{kind}.adpt"
                encoded = subprocess.run([str(CADAPT), "encode", str(source), "--fast", "-3", "--verify",
                                          "-o", str(encoded_path)], capture_output=True, timeout=30)
                self.assertEqual(encoded.returncode, 0, encoded.stderr)
                output = parent / kind
                decoded = self.decode(encoded_path, None, "-o", output)
                self.assertEqual(decoded.returncode, 0, decoded.stderr)
                for path in paths:
                    original = source / path if path else source
                    restored = output / path if path else output
                    self.assertEqual(restored.stat().st_mode, original.stat().st_mode)
                    self.assertEqual(os.listxattr(restored), os.listxattr(original), str(restored))
                self.assertEqual(os.getxattr(parent, "system.posix_acl_default"), acl)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux access ACLs")
    def test_entry_acl_can_remove_owner_read_access(self):
        acl = struct.pack("<I", 2) + b"".join(
            struct.pack("<HHI", tag, permissions, uid)
            for tag, permissions, uid in ((1, 0, 0xffffffff), (2, 5, os.getuid()),
                                          (4, 0, 0xffffffff), (16, 5, 0xffffffff),
                                          (32, 0, 0xffffffff))
        )
        attribute = name("system.posix_acl_access") + struct.pack("<I", len(acl)) + acl
        for kind in (0, 1, 2):
            with self.subTest(kind=kind):
                output = self.root / str(kind)
                restored = output / "restricted" if kind else output
                if kind:
                    item = entry(kind, "restricted", b"bytes", mode=0o050)[:-2] + b"\x01\x00" + attribute
                    data = archive([item])
                else:
                    raw = b"ADPTARC4\x01\x00" + attribute + b"\x00"
                    header = b"ADPT" + bytes((22, 10 | 128, 10, 16))
                    header += struct.pack("<IqIqI", 0o050, 1700000000, 0, 1700000000, 0)
                    header += struct.pack("<I", zlib.crc32(header))
                    data = (header + struct.pack("<BII", 2, len(raw), len(raw)) + raw
                            + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw)))
                try:
                    result = self.decode("-", data, "-o", output)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(restored.stat().st_mode & 0o777, 0o050)
                    self.assertEqual(os.getxattr(restored, "system.posix_acl_access"), acl)
                finally:
                    if restored.exists():
                        restored.chmod(0o700)

    @unittest.skipIf(os.name == "nt", "POSIX umask and restored modes")
    def test_restrictive_umask_preserves_recorded_and_legacy_folder_modes(self):
        for legacy, root_mode in ((False, 0o750), (False, 0o500), (False, 0), (True, None)):
            for mask in (0o022, 0o777):
                with self.subTest(legacy=legacy, root_mode=root_mode, umask=oct(mask)):
                    if legacy:
                        raw = (b"ADPTARC1" + b"\x01" + name("nested")
                               + b"\x02" + name("nested/file") + struct.pack("<Q", 5) + b"first"
                               + b"\x02" + name("second") + struct.pack("<Q", 6) + b"second"
                               + b"\x03" + name("link") + name("nested/file") + b"\x00")
                        header = HEADER[:5] + bytes((HEADER[5] | 128,)) + HEADER[6:]
                    else:
                        raw = (b"ADPTARC3" + entry(1, "nested", mode=0o750)
                               + entry(2, "nested/file", b"first", mode=0o640)
                               + entry(2, "second", b"second", mode=0o640)
                               + entry(4, "alias", "nested/file")
                               + entry(5, "nested/sparse", size=8192, extents=((8190, b"ok"),), mode=0o640)
                               + entry(3, "link", "nested/file") + b"\x00")
                        header = b"ADPT" + bytes((22, 10 | 128, 10, 16))
                        header += struct.pack("<IqIqI", root_mode, 1700000000, 0, 1700000000, 0)
                        header += struct.pack("<I", zlib.crc32(header))
                    data = (header + struct.pack("<BII", 2, len(raw), len(raw)) + raw
                            + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw)))
                    target = self.root / f"restored-{legacy}-{root_mode}-{mask}"
                    result = subprocess.run([str(CADAPT), "decode", "-", "-o", str(target)], input=data,
                                            capture_output=True, timeout=30, umask=mask)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    wanted_dir = 0o777 & ~mask if legacy else 0o750
                    wanted_file = 0o666 & ~mask if legacy else 0o640
                    # Inspect each recorded mode, then grant the test process
                    # access to examine descendants and clean up a mode-000 tree.
                    for rel, wanted in (("", wanted_dir if legacy else root_mode), ("nested", wanted_dir),
                                        ("nested/file", wanted_file), ("second", wanted_file)):
                        path = target / rel
                        self.assertEqual(path.stat().st_mode & 0o777, wanted, rel)
                        path.chmod(0o700)
                    self.assertEqual((target / "nested/file").read_bytes(), b"first")
                    self.assertEqual((target / "second").read_bytes(), b"second")
                    if legacy and os.chmod in os.supports_follow_symlinks:
                        # a legacy link's mode is the umask's, and a platform
                        # that keeps link modes (macOS) refuses readlink at 000
                        os.chmod(target / "link", 0o700, follow_symlinks=False)
                    self.assertEqual(os.readlink(target / "link"), "nested/file")
                    if not legacy:
                        self.assertEqual((target / "alias").stat().st_ino, (target / "nested/file").stat().st_ino)
                        self.assertEqual((target / "nested/sparse").stat().st_mode & 0o777, wanted_file)
                        self.assertEqual((target / "nested/sparse").read_bytes(), bytes(8190) + b"ok")
                    self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipUnless(hasattr(signal, "SIGXFSZ"), "POSIX file-size limit signal")
    def test_file_size_limit_discards_encode_and_decode_staging(self):
        def limit_file_size():
            import resource
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        raw = random.Random(7).randbytes(32 << 10)
        file_data = (HEADER + struct.pack("<BII", 2, len(raw), len(raw)) + raw
                     + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw)))
        folder = self.root / "source-folder"
        folder.mkdir()
        (folder / "data").write_bytes(raw)
        cases = (("encode-file", ["encode", "-", "--fast", "-3"], raw),
                 ("encode-folder", ["encode", str(folder), "--fast", "-3"], None),
                 ("decode-file", ["decode", "-"], file_data),
                 ("decode-folder", ["decode", "-"], archive([entry(2, "data", raw)])),
                 ("decode-sparse", ["decode", "-"], archive([entry(5, "hole", size=8192)])))
        for label, args, data in cases:
            with self.subTest(case=label):
                target = self.root / label
                target.write_bytes(b"keep target")
                result = subprocess.run([str(CADAPT), *args, "-o", str(target), "--force"], input=data,
                                        capture_output=True, timeout=30, preexec_fn=limit_file_size)
                self.assertEqual(result.returncode, 128 + signal.SIGXFSZ, result.stderr)
                self.assertEqual(target.read_bytes(), b"keep target")
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX directory publication")
    def test_folder_publication_refuses_a_destination_created_during_decode(self):
        data = archive([entry(2, "child", b"contents" * 1024)])
        for kind in ("directory", "file", "symlink"):
            with self.subTest(kind=kind):
                target = self.root / kind
                with subprocess.Popen([str(CADAPT), "decode", "-", "-o", str(target)],
                                      stdin=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                    process.stdin.write(data[:-13])
                    process.stdin.flush()
                    self.wait_for_stage(process, "child")
                    if kind == "directory":
                        target.mkdir()
                    elif kind == "file":
                        target.write_bytes(b"keep")
                    else:
                        target.symlink_to("missing")
                    expected = target.lstat()
                    process.stdin.write(data[-13:])
                    process.stdin.close()
                    process.wait(timeout=10)
                    self.assertNotEqual(process.returncode, 0, process.stderr.read())
                actual = target.lstat()
                self.assertEqual((actual.st_dev, actual.st_ino, actual.st_mode),
                                 (expected.st_dev, expected.st_ino, expected.st_mode))
                self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX signal cancellation")
    def test_signals_discard_file_and_folder_staging_with_input_still_open(self):
        raw = b"private bytes" * 1024
        file_data = (HEADER + struct.pack("<BII", 2, len(raw), len(raw)) + raw
                     + struct.pack("<BQI", 3, len(raw), zlib.crc32(raw)))
        folder_data = archive([entry(1, "nested"), entry(2, "nested/child", raw)])
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            for command, data, child in (("encode", b"unfinished input", None),
                                         ("decode", file_data[:-13], None),
                                         ("decode", folder_data[:-13], "nested/child")):
                with self.subTest(signal=sig, command=command, child=child):
                    output = self.root / "existing"
                    output.write_bytes(b"keep destination")
                    with subprocess.Popen([str(CADAPT), command, "-", "-o", str(output), "--force"],
                                          stdin=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                        process.stdin.write(data)
                        process.stdin.flush()
                        self.wait_for_stage(process, child)
                        process.send_signal(sig)
                        process.wait(timeout=10)
                        self.assertEqual(process.returncode, 128 + sig, process.stderr.read())
                    self.assertEqual(output.read_bytes(), b"keep destination")
                    self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

    @unittest.skipIf(os.name == "nt", "POSIX pipe cancellation")
    def test_signal_wakes_decode_with_unread_stdout(self):
        source = self.root / "source.adpt"
        segmented_file(source, 2)
        with subprocess.Popen([str(CADAPT), "decode", str(source), "-o", "-", "--threads", "2"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            self.assertTrue(select.select([process.stdout], [], [], 10)[0], "decoder produced no output")
            process.send_signal(signal.SIGTERM)
            # No one drains stdout: cancellation must wake the blocked writer.
            process.wait(timeout=10)
            self.assertEqual(process.returncode, 128 + signal.SIGTERM, process.stderr.read())

    @unittest.skipIf(os.name == "nt", "POSIX descriptor ownership")
    def test_closed_stdin_is_not_reused_as_the_signal_pipe(self):
        output = self.root / "closed-input"
        result = subprocess.run([str(CADAPT), "decode", "-", "-o", str(output)],
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=10,
                                preexec_fn=lambda: os.close(0))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob(".cadapt-*.part")), [])

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
                            if staged and (staged[0] / "contents").exists() and (staged[0] / "contents").stat().st_size >= 4096:
                                break
                            self.assertIsNone(process.poll(), "decoder exited before trailer")
                            time.sleep(0.01)
                        else:
                            self.fail("decoder did not stage the data block")
                        self.assertEqual(staged[0].stat().st_mode & 0o777, 0o700)
                        self.assertEqual((staged[0] / "contents").stat().st_mode & 0o777, 0o600)
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
        parent = self.root / "different-acl"
        parent.mkdir()
        inherited = bytearray(acl)
        struct.pack_into("<H", inherited, 14, 7)  # different named-user permissions
        os.setxattr(parent, "system.posix_acl_default", inherited)
        restored = parent / "restored"
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
