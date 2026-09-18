"""tools/provenance.py: the SBOM names the codec, its licence, its sources by
hash and its dependencies; the build manifest records a build so that
`reproduce` can rebuild and confirm it, and refuses a manifest it cannot.

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14,
with Claude Opus 5 (Claude Code).
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import provenance

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "provenance.py"
CODEC = ROOT / "adpat" / "c"
SOURCES = ["adapt_adaptive.h", "adapt_bmp.h", "adapt.h", "adapt_model.h", "adapt_text.h",
           "adapt_constants.h", "adapt_states.h", "adapt_coder.h", "adapt_tables.h", "adapt_model_state.h",
           "adapt_model_byte.h", "adapt_model_bit.h", "adapt_model_new.h", "adapt_blocks.h", "adapt_crc32.h",
           "adapt_decode.c", "adapt_encode.c", "adapt_archive.c", "cadapt.c", "cadapt_folder.c", "cadapt_folder.h",
           "cadapt_fs.c", "cadapt_fs.h",
           "cadapt_platform.h"]


def run(*args, **kwargs):
    return subprocess.run([sys.executable, "-B", str(TOOL), *map(str, args)], capture_output=True, text=True, cwd=ROOT, **kwargs)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class MakeVariablesTest(unittest.TestCase):
    def test_directory_logging_does_not_corrupt_variable_capture(self):
        """Recursive make exports its directory logging flag into child probes."""
        with patch.dict(os.environ, {"MAKEFLAGS": "w", "MAKELEVEL": "1"}):
            variables = provenance.make_variables()
        # CC ?= cc: a `make check CC=gcc` exports CC into this process, and
        # the probe must report that compiler, not the default
        self.assertEqual(variables["CC"], os.environ.get("CC", "cc"))
        self.assertIn("-std=c99", variables["CFLAGS"])
        self.assertEqual(variables["LDFLAGS"], "")


class SbomTest(unittest.TestCase):
    def test_sbom_describes_the_codec_its_licence_sources_and_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            out = Path(scratch) / "sbom.spdx.json"
            result = run("sbom", "--output", out)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(doc["spdxVersion"], "SPDX-2.3")
        self.assertEqual(doc["dataLicense"], "CC0-1.0")
        packages = {p["name"]: p for p in doc["packages"]}
        codec = packages["adapt"]
        self.assertEqual(codec["licenseDeclared"], "MIT")
        self.assertEqual(codec["licenseConcluded"], "MIT")
        self.assertRegex(codec["versionInfo"], r"^\d+\.\d+\.\d+$")
        self.assertIn("Pedro Anisio de Luna e Silva", codec["copyrightText"])
        files = {f["fileName"]: f for f in doc["files"]}
        for name in SOURCES:
            entry = files[f"./adpat/c/{name}"]
            digests = {c["algorithm"]: c["checksumValue"] for c in entry["checksums"]}
            self.assertEqual(digests["SHA256"], sha256(CODEC / name), name)
            self.assertIn("SHA1", digests)
            self.assertEqual(entry["licenseConcluded"], "MIT")
        # the dependencies: the C library at run time, a compiler to build,
        # Python and the corpus to test and measure
        kinds = {(r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]) for r in doc["relationships"]}
        self.assertIn(("SPDXRef-DOCUMENT", "DESCRIBES", codec["SPDXID"]), kinds)
        self.assertIn((codec["SPDXID"], "DEPENDS_ON", packages["libc"]["SPDXID"]), kinds)
        self.assertIn((packages["c-compiler"]["SPDXID"], "BUILD_DEPENDENCY_OF", codec["SPDXID"]), kinds)
        self.assertIn((packages["python3"]["SPDXID"], "TEST_DEPENDENCY_OF", codec["SPDXID"]), kinds)
        self.assertIn((packages["enwik8"]["SPDXID"], "TEST_DEPENDENCY_OF", codec["SPDXID"]), kinds)
        self.assertEqual(packages["enwik8"]["licenseConcluded"], "CC-BY-SA-3.0")
        self.assertEqual(packages["corpus-scripts"]["licenseConcluded"], "CC-BY-SA-4.0")
        for name in SOURCES:
            self.assertIn((codec["SPDXID"], "CONTAINS", files[f"./adpat/c/{name}"]["SPDXID"]), kinds)
        # the committed SBOM is the tool's output for the tree as it is,
        # apart from the creation time
        committed = json.loads((ROOT / "sbom.spdx.json").read_text(encoding="utf-8"))
        for d in (doc, committed):
            d["creationInfo"].pop("created")
        self.assertEqual(committed, doc, "sbom.spdx.json is stale: run `make sbom`")


class ManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        built = subprocess.run(["make", "-s", "-C", str(CODEC), "all"], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr

    def test_manifest_records_the_build_and_reproduce_confirms_or_refutes_it(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            manifest = Path(scratch) / "build-manifest.json"
            result = run("manifest", "--output", manifest)
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(record["schema"], "cadapt.build-manifest.v1")
            self.assertRegex(record["version"], r"^\d+\.\d+\.\d+$")
            self.assertTrue(record["toolchain"]["cc"])
            self.assertIn("-std=c99", record["toolchain"]["cflags"])
            for name in SOURCES:
                self.assertEqual(record["sources"][f"adpat/c/{name}"], sha256(CODEC / name))
            outputs = record["outputs"]
            for path in ("adpat/c/build/cadapt", "adpat/c/build/libadapt.a"):
                self.assertEqual(outputs[path]["sha256"], sha256(ROOT / path))
                self.assertEqual(outputs[path]["bytes"], (ROOT / path).stat().st_size)
            # a fresh build in another directory gives the recorded bytes
            confirmed = run("reproduce", "--manifest", manifest, "--build", "build-reproduce-test")
            self.assertEqual(confirmed.returncode, 0, confirmed.stdout + confirmed.stderr)
            self.assertIn("reproduced", confirmed.stdout)
            self.assertFalse((CODEC / "build-reproduce-test").exists(), "the check leaves no build behind")
            # a manifest that names other bytes is refuted, naming the output
            genuine = outputs["adpat/c/build/cadapt"]["sha256"]
            record["outputs"]["adpat/c/build/cadapt"]["sha256"] = "0" * 64
            manifest.write_text(json.dumps(record), encoding="utf-8")
            refuted = run("reproduce", "--manifest", manifest, "--build", "build-reproduce-test")
            self.assertEqual(refuted.returncode, 1, refuted.stdout + refuted.stderr)
            self.assertIn("adpat/c/build/cadapt", refuted.stdout + refuted.stderr)
            self.assertIn("differs", refuted.stdout + refuted.stderr)
            # a manifest from another toolchain says so rather than blaming the bytes
            record["outputs"]["adpat/c/build/cadapt"]["sha256"] = genuine
            record["toolchain"]["cc"] = "someone else's compiler 1.0"
            manifest.write_text(json.dumps(record), encoding="utf-8")
            other = run("reproduce", "--manifest", manifest, "--build", "build-reproduce-test")
            self.assertEqual(other.returncode, 0, other.stdout + other.stderr)
            self.assertIn("toolchain differs", other.stdout + other.stderr)


if __name__ == "__main__":
    unittest.main()
