"""Tests for the mixed-corpus fetcher.

Source: Claude Opus 5 via Claude Code. Local archives behind file:// URLs
stand in for the corpora's homes, so every outcome — a fetch that matches
the manifest, an archive or a file that does not, a missing file on
--check, a malformed manifest, an unknown corpus name — runs in
milliseconds without the network.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

import fetch_corpus


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FetchCorpusTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.files = {"alpha": b"alpha " * 100, "beta": b"\x00\x01\x02" * 50}
        zipped = io.BytesIO()
        with zipfile.ZipFile(zipped, "w") as archive:
            for name, data in self.files.items():
                archive.writestr(f"corpus/{name}", data)
        self.zip_path = self.root / "small.zip"
        self.zip_path.write_bytes(zipped.getvalue())
        tarred = io.BytesIO()
        with tarfile.open(fileobj=tarred, mode="w:gz") as archive:
            for name, data in self.files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        self.tar_path = self.root / "small.tar.gz"
        self.tar_path.write_bytes(tarred.getvalue())
        self.destination = self.root / "mixed"
        self.manifest = self.root / "manifest.json"
        self.write_manifest()

    def corpus_entry(self, name: str, archive: Path) -> dict:
        data = archive.read_bytes()
        return {"name": name, "url": archive.as_uri(), "archive_bytes": len(data), "archive_sha256": sha(data),
                "licence": "test", "files": [{"name": n, "bytes": len(d), "sha256": sha(d)} for n, d in self.files.items()]}

    def write_manifest(self, **override) -> None:
        doc = {"schema": fetch_corpus.SCHEMA,
               "corpora": [self.corpus_entry("zipped", self.zip_path), self.corpus_entry("tarred", self.tar_path)]}
        doc.update(override)
        self.manifest.write_text(json.dumps(doc))

    def run_main(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = fetch_corpus.main(["--manifest", str(self.manifest), "--destination", str(self.destination), *args])
        return code, out.getvalue()

    def test_fetch_unpacks_both_archive_kinds_and_check_agrees(self) -> None:
        code, text = self.run_main()
        self.assertEqual(code, 0, text)
        for corpus in ("zipped", "tarred"):
            for name, data in self.files.items():
                self.assertEqual((self.destination / corpus / name).read_bytes(), data)
        code, text = self.run_main("--check")
        self.assertEqual(code, 0, text)
        self.assertEqual(text.count("matches"), 4)

    def test_only_selects_a_corpus_and_rejects_an_unknown_name(self) -> None:
        code, text = self.run_main("--only", "tarred")
        self.assertEqual(code, 0, text)
        self.assertTrue((self.destination / "tarred" / "alpha").exists())
        self.assertFalse((self.destination / "zipped").exists())
        code, text = self.run_main("--only", "other")
        self.assertEqual(code, 1)
        self.assertIn("unknown corpus", text)

    def test_archive_that_differs_from_the_manifest_is_refused(self) -> None:
        entry = self.corpus_entry("zipped", self.zip_path)
        entry["archive_sha256"] = "0" * 64
        self.write_manifest(corpora=[entry])
        code, text = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("the manifest pins", text)
        self.assertFalse((self.destination / "zipped" / "alpha").exists())

    def test_file_that_differs_from_the_manifest_is_refused(self) -> None:
        entry = self.corpus_entry("zipped", self.zip_path)
        entry["files"][0]["sha256"] = "0" * 64
        self.write_manifest(corpora=[entry])
        code, text = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("zipped/alpha", text)
        entry["files"][0]["sha256"] = sha(self.files["alpha"])
        entry["files"].append({"name": "gamma", "bytes": 1, "sha256": sha(b"x")})
        self.write_manifest(corpora=[entry])
        code, text = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("holds no 'gamma'", text)

    def test_check_reports_a_missing_or_changed_file(self) -> None:
        code, text = self.run_main()
        self.assertEqual(code, 0, text)
        (self.destination / "zipped" / "beta").write_bytes(b"changed")
        code, text = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("zipped/beta", text)
        (self.destination / "zipped" / "beta").unlink()
        code, text = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("missing", text)

    def test_malformed_manifests_are_typed_errors(self) -> None:
        cases = [dict(schema="other"), dict(corpora=[]), dict(corpora=[{"name": "x"}]),
                 dict(corpora=[dict(self.corpus_entry("zipped", self.zip_path), files=[])]),
                 dict(corpora=[dict(self.corpus_entry("zipped", self.zip_path), files=[{"name": "a"}])])]
        for case in cases:
            self.write_manifest(**case)
            with self.assertRaises(fetch_corpus.FetchError, msg=str(case)):
                fetch_corpus.load_manifest(self.manifest)
        self.manifest.write_text("{not json")
        with self.assertRaises(fetch_corpus.FetchError):
            fetch_corpus.load_manifest(self.manifest)
        code, text = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("error:", text)

    def test_repository_manifest_is_valid_and_names_both_corpora(self) -> None:
        manifest = fetch_corpus.load_manifest(fetch_corpus.MANIFEST)
        names = {c["name"]: c for c in manifest["corpora"]}
        self.assertEqual(sorted(names), ["canterbury", "silesia"])
        self.assertEqual(len(names["silesia"]["files"]), 12)
        self.assertEqual(len(names["canterbury"]["files"]), 11)
        self.assertEqual(sum(f["bytes"] for f in names["silesia"]["files"]), 211_938_580)
        self.assertEqual(sum(f["bytes"] for f in names["canterbury"]["files"]), 2_810_784)
        for corpus in names.values():
            self.assertTrue(corpus["url"].startswith("https://"))
            self.assertIn("not redistributed", corpus["licence"])


if __name__ == "__main__":
    unittest.main()
