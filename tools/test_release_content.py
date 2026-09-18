"""Release-boundary regressions against actual staged blobs and archive bytes."""

import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

import release_content as content


def inventory(files):
    return {
        "schema": "cadapt.release-assets.v1",
        "source": {p: {"classification": "production"} for p in files},
        "binary": [],
    }


class ContentTest(unittest.TestCase):
    def test_unknown_and_missing_assets_fail(self):
        errors = content.check_files({"notes.md": b"plan"}, inventory(["main.c"]))
        self.assertTrue(any("UNCLASSIFIED notes.md" in e for e in errors))
        self.assertTrue(any("MISSING main.c" in e for e in errors))

    def test_private_path_and_secret_are_redacted(self):
        private = b"/home/" + b"example/private/file"
        errors = content.content_errors(
            "data.json", private, {"classification": "production"}
        )
        self.assertTrue(errors)
        self.assertNotIn(private.decode(), "\n".join(errors))
        self.assertTrue(
            content.content_errors("key", b"-----BEGIN " + b"PRIVATE KEY-----", {})
        )

    def test_local_uris_and_shared_working_memory_links_are_rejected(self):
        for data in (
            b"file:" + b"///tmp/notes",
            b"https://example.test/Workspace/" + b"Ai-Memory/project",
        ):
            with self.subTest(data=data):
                self.assertTrue(content.content_errors("guide.md", data, {}))

    def test_scoped_dated_report_reference_is_rejected(self):
        reference = b"docs/performance-" + b"audit-128mib-2026-09-15.md"
        self.assertTrue(content.content_errors("manual.1", reference, {}))

    def test_invalid_inventory_type_is_an_actionable_error(self):
        for data in (b"[]", b"null", b"42"):
            with self.assertRaises(content.ContentError):
                content.load_inventory(data)

    def test_fixture_words_are_not_semantic_failures_and_bytes_are_pinned(self):
        fixture = b"agent session /home/" + b"example/test"
        entry = {"content": "fixture", "sha256": hashlib.sha256(fixture).hexdigest()}
        self.assertEqual(content.content_errors("input.bin", fixture, entry), [])
        self.assertTrue(content.content_errors("input.bin", fixture + b"!", entry))

    def test_exception_is_limited_to_one_reviewed_line_and_rule(self):
        line = b"/home/" + b"example/negative-test"
        entry = {
            "exceptions": [
                {
                    "rule": "HOST_PATH",
                    "line_sha256": hashlib.sha256(line).hexdigest(),
                    "reason": "negative path fixture",
                }
            ]
        }
        self.assertEqual(content.content_errors("test.py", line, entry), [])
        self.assertTrue(content.content_errors("test.py", line + b"/other", entry))

    def test_agent_instruction_requires_evidence_of_product_use(self):
        with self.assertRaises(content.ContentError):
            content.load_inventory(json.dumps(inventory(["CLAUDE.md"])).encode())

    def test_product_prompt_requires_reviewed_consumer_and_rationale(self):
        assets = inventory(["SKILL.md", "main.c"])
        use = {"consumer": "main.c", "reason": "Loaded as the runtime task template"}
        assets["source"]["SKILL.md"]["product_use"] = use
        loaded = content.load_inventory(json.dumps(assets).encode())
        files = {"SKILL.md": b"Runtime task template", "main.c": b"load_template();"}
        self.assertEqual(content.check_files(files, loaded), [])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "SKILL.md").write_bytes(files["SKILL.md"])
            self.assertEqual(content.context_errors(root, loaded), [])
        for invalid in (
            {"consumer": "missing.c", "reason": "runtime"},
            {"consumer": "main.c", "reason": " "},
            {"consumer": "SKILL.md", "reason": "runtime"},
            {"consumer": "main.c"},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(content.ContentError):
                assets["source"]["SKILL.md"]["product_use"] = invalid
                content.load_inventory(json.dumps(assets).encode())
        assets["source"]["SKILL.md"]["product_use"] = use
        assets["source"]["main.c"]["classification"] = "documentation"
        with self.assertRaises(content.ContentError):
            content.load_inventory(json.dumps(assets).encode())

    def test_product_directory_exception_does_not_hide_extra_state(self):
        name = str(Path(".repo") / "template.txt")
        assets = inventory([name, "main.c"])
        assets["source"][name]["product_use"] = {
            "consumer": "main.c",
            "reason": "Runtime template lookup",
        }
        loaded = content.load_inventory(json.dumps(assets).encode())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            product = root / ".repo"
            product.mkdir()
            (root / name).write_text("template")
            self.assertEqual(content.context_errors(root, loaded), [])
            extra = product / "session.log"
            extra.write_text("working state")
            self.assertTrue(content.context_errors(root, loaded))
            extra.unlink()
            extra.symlink_to(root / name)
            self.assertTrue(content.context_errors(root, loaded))

    def test_source_hashes_detect_payload_changes(self):
        files = {"main.c": b"int x;"}
        files[content.SOURCE_RECORD] = json.dumps(
            {"files": {"main.c": "0" * 64}}
        ).encode()
        self.assertTrue(
            content.check_files(files, inventory(["main.c"]), source_record=True)
        )

    def test_index_reads_staged_content_not_clean_working_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "file.txt").write_bytes(b"/home/" + b"example/private")
            subprocess.run(["git", "add", "file.txt"], cwd=root, check=True)
            (root / "file.txt").write_text("clean")
            files = content.repository_files(root, staged=True)
            self.assertTrue(content.check_files(files, inventory(files)))

    def test_ignored_agent_state_is_a_workspace_boundary_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".repo").mkdir()
            self.assertTrue(content.context_errors(root))
            (root / ".repo").rmdir()
            self.assertEqual(content.context_errors(root), [])

    def test_force_added_ignored_record_is_inspected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("notes.json\n")
            (root / "notes.json").write_text("{}")
            subprocess.run(["git", "add", "-f", "notes.json"], cwd=root, check=True)
            self.assertIn("notes.json", content.repository_files(root, staged=True))


class ArchiveTest(unittest.TestCase):
    def archive(self, root, entries):
        path = root / "package.tar.gz"
        with tarfile.open(path, "w:gz") as tf:
            for name, data, kind in entries:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = len(data) if kind == tarfile.REGTYPE else 0
                member.linkname = "../outside" if kind == tarfile.SYMTYPE else ""
                tf.addfile(member, io.BytesIO(data))
        return path

    def test_ltcb_zip_has_only_runtime_assets_and_no_private_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "entry.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("cadapt", b"binary")
                archive.writestr("README-ltcb.txt", b"usage")
                archive.writestr("LICENSE", b"MIT")
            self.assertEqual(content.check_archive(path, inventory([]), "ltcb"), [])
            with zipfile.ZipFile(path, "a") as archive:
                archive.writestr("notes.json", b"private development record")
            self.assertTrue(content.check_archive(path, inventory([]), "ltcb"))

    def test_traversal_links_and_duplicate_members_fail(self):
        for entries in [
            [("pkg/../outside", b"x", tarfile.REGTYPE)],
            [("pkg/link", b"", tarfile.SYMTYPE)],
            [("pkg/file", b"x", tarfile.REGTYPE)] * 2,
        ]:
            with (
                self.subTest(entries=entries),
                tempfile.TemporaryDirectory() as directory,
            ):
                with self.assertRaises(content.ContentError):
                    content.archive_files(self.archive(Path(directory), entries))

    def test_post_strip_payload_is_verified_and_nested_archive_is_not_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "pkg"
            package.mkdir()
            (package / "cadapt").write_bytes(b"stripped")
            record = content.payload_record(package, "a" * 40)
            assets = inventory([])
            assets["binary"] = ["cadapt", "payload-manifest.json"]
            entries = [
                ("pkg/cadapt", b"stripped", tarfile.REGTYPE),
                (
                    "pkg/payload-manifest.json",
                    json.dumps(record).encode(),
                    tarfile.REGTYPE,
                ),
            ]
            self.assertEqual(
                content.check_archive(self.archive(root, entries), assets, "binary"), []
            )
            entries[0] = ("pkg/cadapt", b"changed", tarfile.REGTYPE)
            self.assertTrue(
                content.check_archive(self.archive(root, entries), assets, "binary")
            )
            entries.append(("pkg/notes.zip", b"PK", tarfile.REGTYPE))
            self.assertTrue(
                any(
                    "unexpected installed file" in e
                    for e in content.check_archive(
                        self.archive(root, entries), assets, "binary"
                    )
                )
            )


class PublicationTest(unittest.TestCase):
    def fixture(self, root):
        assets = inventory(["main.c", content.INVENTORY])
        assets["binary"] = ["bin/cadapt", "payload-manifest.json"]
        source = {
            "main.c": b"int main(void) { return 0; }",
            content.INVENTORY: json.dumps(assets).encode(),
        }
        source[content.SOURCE_RECORD] = json.dumps(
            {
                "schema": "cadapt.release-source.v1",
                "commit": "a" * 40,
                "files": {p: hashlib.sha256(b).hexdigest() for p, b in source.items()},
            }
        ).encode()
        for index, platform in enumerate(("linux-x86_64", "darwin-arm64")):
            directory = root / platform
            directory.mkdir()
            binary = platform.encode()
            payload = {
                "bin/cadapt": binary,
                "payload-manifest.json": json.dumps(
                    {
                        "schema": "cadapt.installed-payload.v1",
                        "commit": "a" * 40,
                        "stage": "installed-after-strip",
                        "files": {
                            "bin/cadapt": {
                                "sha256": hashlib.sha256(binary).hexdigest(),
                                "bytes": len(binary),
                            }
                        },
                    }
                ).encode(),
            }
            for name, files in (
                ("cadapt-1.0-src.tar.gz", source),
                (f"cadapt-1.0-{platform}.tar.gz", payload),
            ):
                with tarfile.open(directory / name, "w:gz") as archive:
                    for member, data in files.items():
                        info = tarfile.TarInfo("cadapt-1.0/" + member)
                        info.size, info.mtime = len(data), index
                        archive.addfile(info, io.BytesIO(data))
            (directory / "sbom.spdx.json").write_text(
                json.dumps({"platform": platform})
            )
            (directory / "build-manifest.json").write_text(
                json.dumps({"commit": "a" * 40})
            )
            (directory / "NOTICE").write_text("Licence: MIT")
            (directory / "SHA256SUMS").write_text("producer checksums")
            manifest = {
                "schema": "cadapt.release-manifest.v1",
                "version": "1.0",
                "platform": platform,
                "commit": "a" * 40,
                "artifacts": {
                    p.name: {
                        "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                        "bytes": p.stat().st_size,
                    }
                    for p in directory.iterdir()
                },
            }
            (directory / "release-manifest.json").write_text(json.dumps(manifest))
        return assets

    def test_published_manifests_resolve_after_platform_renames_and_source_deduplication(
        self,
    ):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            incoming = root / "incoming"
            incoming.mkdir()
            assets = self.fixture(incoming)
            archives = list(incoming.glob("*/cadapt-1.0-src.tar.gz"))
            self.assertNotEqual(archives[0].read_bytes(), archives[1].read_bytes())
            destination = root / "release"
            content.assemble(incoming, destination, assets)
            self.assertEqual(len(list(destination.glob("*-src.tar.gz"))), 1)
            for path in destination.glob("release-manifest-*.json"):
                manifest = json.loads(path.read_text())
                self.assertEqual(manifest["stage"], "published-assets")
                self.assertNotIn("SHA256SUMS", manifest["artifacts"])
                for name, expected in manifest["artifacts"].items():
                    data = (destination / name).read_bytes()
                    self.assertEqual(
                        expected,
                        {
                            "sha256": hashlib.sha256(data).hexdigest(),
                            "bytes": len(data),
                        },
                    )
            for line in (destination / "SHA256SUMS").read_text().splitlines():
                expected, name = line.split("  ")
                self.assertEqual(
                    expected,
                    hashlib.sha256((destination / name).read_bytes()).hexdigest(),
                )

    def test_different_source_payloads_cannot_share_one_published_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            incoming = root / "incoming"
            incoming.mkdir()
            assets = self.fixture(incoming)
            directory = incoming / "linux-x86_64"
            archive_path = directory / "cadapt-1.0-src.tar.gz"
            files = content.archive_files(archive_path)
            files["main.c"] = b"int main(void) { return 1; }"
            record = json.loads(files[content.SOURCE_RECORD])
            record["files"]["main.c"] = hashlib.sha256(files["main.c"]).hexdigest()
            files[content.SOURCE_RECORD] = json.dumps(record).encode()
            with tarfile.open(archive_path, "w:gz") as archive:
                for name, data in files.items():
                    member = tarfile.TarInfo("cadapt-1.0/" + name)
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
            manifest_path = directory / "release-manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["artifacts"][archive_path.name] = {
                "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                "bytes": archive_path.stat().st_size,
            }
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(content.ContentError, "source payloads differ"):
                content.assemble(incoming, root / "release", assets)
            self.assertFalse((root / "release").exists())

    def test_changed_or_unexpected_downloaded_artifacts_block_publication(self):
        for name in ("NOTICE", "session.log"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                incoming = root / "incoming"
                incoming.mkdir()
                assets = self.fixture(incoming)
                (incoming / "linux-x86_64" / name).write_text("changed")
                with self.assertRaises(content.ContentError):
                    content.assemble(incoming, root / "release", assets)
                self.assertFalse((root / "release").exists())


if __name__ == "__main__":
    unittest.main()
