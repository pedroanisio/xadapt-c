"""Regression tests for measurement accounting, failure records and run history."""

from __future__ import annotations

import argparse
import contextlib
import csv
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
import zlib
from pathlib import Path

import telemetry

CADAPT = telemetry.ROOT / "adpat/c/build/cadapt"


class TelemetryTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.source = self.root / "source.bin"
        self.source.write_bytes(b"Telemetry should preserve failed runs.\n" * 128)

    def test_adaptive_versions_and_measurement_profiles(self) -> None:
        output = self.root / "adaptive-history"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = telemetry.main([
                "run", "--binary", str(CADAPT), "--corpus", str(self.source),
                "--output", str(output), "--profiles", "rates", "sparse", "retained", "auto", "--no-plots",
            ])
        self.assertEqual(rc, 0)
        records = telemetry.load_runs([output])
        self.assertEqual([r["codec"]["format_version"] for r in records[:3]], [28, 29, 30])
        self.assertEqual([r["codec"]["adaptive_profile"] for r in records[:3]], ["rates", "sparse", "retained"])
        self.assertTrue(all(r["status"] == "ok" for r in records))

    def test_search_trace_requires_every_candidate_and_correct_winner(self) -> None:
        import adaptive_experiment

        trace = (
            "adaptive trial max-normal: 105 bytes\n"
            "adaptive trial max-rates: 107 bytes\n"
            "adaptive trial max-sparse skipped: memory\n"
            "adaptive trial max-retained: 104 bytes\n"
            "adaptive selected max-retained: 104 bytes\n"
        )
        trials = adaptive_experiment.check_search_trace(trace, "max", False, 104)
        self.assertEqual(len(trials), 4)
        self.assertEqual(trials[2]["skipped"], "memory")
        for invalid in (
            trace.replace("adaptive trial max-rates: 107 bytes\n", ""),
            trace.replace("max-rates", "max-normal"),
            trace.replace("selected max-retained: 104", "selected max-rates: 104"),
            trace.replace("selected max-retained: 104", "selected max-retained: 108"),
        ):
            with self.subTest(trace=invalid), self.assertRaises(ValueError):
                adaptive_experiment.check_search_trace(invalid, "max", False, 104)


    def test_max_search_experiment_accounts_for_all_trials(self) -> None:
        import adaptive_experiment

        output = self.root / "max-search"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adaptive_experiment.main([
                "--baseline", str(CADAPT), "--binary", str(CADAPT),
                "--output", str(output), "--corpora", str(self.source),
                "--base-profile", "max", "--search-only", "--memory", str(128 << 20),
            ])
        result = json.loads((output / "results.json").read_text())
        self.assertEqual(rc, 0)
        self.assertEqual(result["expected_runs"], 2)
        self.assertEqual(result["base_profile"], "max")
        self.assertTrue(result["complete"])
        self.assertEqual(len(result["runs"][1]["candidate_trials"]), 4)
        self.assertTrue(result["comparisons"][0]["auto_equals_smallest"])
        self.assertTrue(all(r["codec"]["base_profile"] == "max" for r in result["runs"]))

    def test_external_corpus_experiment_preserves_complete_accounting(self) -> None:
        import adaptive_experiment

        output = self.root / "external-experiment"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = adaptive_experiment.main([
                "--baseline", str(CADAPT), "--binary", str(CADAPT),
                "--output", str(output), "--corpora", str(self.source),
            ])
        result = json.loads((output / "results.json").read_text())
        self.assertEqual(rc, 0)
        self.assertTrue(result["complete"])
        self.assertEqual(result["expected_runs"], 5)
        self.assertEqual(set(result["fixtures"]), {self.source.name})
        self.assertTrue(result["comparisons"][0]["auto_equals_smallest"])
        self.assertTrue(all(r["restoration"]["byte_exact"] for r in result["runs"]))
        other = self.root / "other"
        other.mkdir()
        duplicate = other / self.source.name
        duplicate.write_bytes(b"different")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failed:
            adaptive_experiment.main([
                "--baseline", str(CADAPT), "--binary", str(CADAPT),
                "--output", str(self.root / "duplicate-experiment"),
                "--corpora", str(self.source), str(duplicate),
            ])
        self.assertEqual(failed.exception.code, 2)

    def test_experiment_rejects_a_file_restored_in_place_of_an_empty_directory(self) -> None:
        import adaptive_experiment

        source, restored = self.root / "folder", self.root / "restored"
        (source / "empty").mkdir(parents=True)
        restored.mkdir()
        (restored / "empty").write_bytes(b"")
        expected = adaptive_experiment.manifest(source)
        adaptive_experiment.reset_metadata(restored, expected)
        result = adaptive_experiment.compare_restoration(source, restored, expected)
        self.assertFalse(all(result.values()))

    def test_byte_units_and_nonfinite_arguments(self) -> None:
        self.assertEqual(telemetry.byte_count("64MiB"), 64 << 20)
        self.assertEqual(telemetry.byte_count("262144"), 262144)
        for bad in ("0", "-2", "3MB", "1.5MiB"):
            with self.subTest(bad=bad), self.assertRaises(argparse.ArgumentTypeError):
                telemetry.byte_count(bad)
        for bad in ("nan", "inf", "0", "-1"):
            with self.subTest(bad=bad), self.assertRaises(argparse.ArgumentTypeError):
                telemetry.positive_float(bad)

    def test_missing_cpu_counter_does_not_become_zero(self) -> None:
        self.assertEqual(telemetry.sampled_cpu_seconds({"user_seconds": 1, "system_seconds": 0.5}), 1.5)
        self.assertIsNone(telemetry.sampled_cpu_seconds({"user_seconds": 1, "system_seconds": None}))

    def test_zip_member_is_copied_without_extracting_its_path(self) -> None:
        archive = self.root / "input.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("../outside.bin", b"0123456789")
        prepared = self.root / "prepared.bin"
        identity = telemetry.prepare_corpus(archive, prepared, "../outside.bin", 4)
        self.assertEqual(prepared.read_bytes(), b"0123")
        self.assertEqual(identity["bytes"], 4)
        self.assertEqual(identity["full_bytes"], 10)
        self.assertTrue(identity["is_prefix"])
        self.assertFalse((self.root.parent / "outside.bin").exists())
        with self.assertRaises(FileExistsError):
            telemetry.prepare_corpus(archive, prepared, "../outside.bin", None)

    def test_sample_reads_actual_process_and_input_position(self) -> None:
        with self.source.open("rb", buffering=0) as source:
            source.read(8)
            sample = telemetry.sample_process(os.getpid(), 1, self.source.resolve(), self.root / "out.adpt")
        self.assertEqual(sample["input_file_position_bytes"], 8)
        self.assertGreater(sample["rss_bytes"], 0)
        self.assertGreaterEqual(sample["threads"], 1)
        self.assertIsNone(sample["cpu_percent"])

    def measure(self, code: str, timeout: float = 5) -> dict:
        directory = self.root / "measurement"
        directory.mkdir()
        return telemetry.measure(
            [sys.executable, "-c", code],
            directory,
            self.source,
            directory / "encoded.adpt",
            interval=0.01,
            timeout=timeout,
            time_binary=Path("/usr/bin/time"),
        )

    def test_child_resource_usage_and_samples_are_recorded(self) -> None:
        result = self.measure("import time; data=bytearray(16*1024*1024); time.sleep(0.15)")
        self.assertEqual(result["status"], "ok", result)
        self.assertEqual(result["exit_code"], 0)
        self.assertGreater(result["sample_count"], 0)
        self.assertGreater(result["metrics"]["peak_rss_bytes"], 16 << 20)
        self.assertGreaterEqual(result["metrics"]["elapsed_seconds"], 0.1)
        with (self.root / "measurement/samples.csv").open() as source:
            samples = list(csv.DictReader(source))
        self.assertEqual(len(samples), result["sample_count"])
        self.assertTrue(any(float(row["rss_bytes"] or 0) > 16 << 20 for row in samples))

    def test_failed_process_keeps_exit_status_and_stderr(self) -> None:
        result = self.measure("import sys; sys.stderr.write('deliberate failure'); sys.exit(7)")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["exit_code"], 7)
        self.assertIn("deliberate failure", (self.root / "measurement/stderr.txt").read_text())

    def test_timeout_is_not_reported_as_success(self) -> None:
        result = self.measure("import time; time.sleep(60)", timeout=0.05)
        self.assertEqual(result["status"], "timeout")
        self.assertNotEqual(result["exit_code"], 0)
        self.assertLess(result["metrics"]["observed_wall_seconds"], 5)

    def test_interrupt_keeps_the_active_run_checkpoint(self) -> None:
        script = Path(telemetry.__file__)
        output = self.root / "interrupted"
        # Use a tiny memory budget only in other tests; this command stays busy
        # long enough to interrupt the measured child, without a synthetic codec.
        with self.source.open("wb") as source:
            source.write(bytes(range(256)) * (1 << 16))
        command = [
            sys.executable,
            "-B",
            str(script),
            "run",
            "--binary",
            str(CADAPT),
            "--corpus",
            str(self.source),
            "--output",
            str(output),
            "--no-plots",
            "--profiles",
            "normal",
        ]
        with subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as process:
            deadline = time.monotonic() + 10
            while (
                not any(path.stat().st_size for path in output.rglob("samples.csv"))
                and process.poll() is None
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            self.assertIsNone(process.poll())
            process.send_signal(signal.SIGINT)
            _, error = process.communicate(timeout=5)
        records = telemetry.load_runs([output])
        self.assertEqual(process.returncode, 1, error)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["status"], "interrupted")
        session = json.loads(next(output.glob("*/session.json")).read_text())
        self.assertFalse(session["complete"])

    def test_matrix_history_and_failed_configurations_remain_visible(self) -> None:
        output = self.root / "history"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = telemetry.main(
                [
                    "run",
                    "--binary",
                    str(CADAPT),
                    "--corpus",
                    str(self.source),
                    "--output",
                    str(output),
                    "--profiles",
                    "normal-bmp",
                    "fast",
                    "--memory",
                    "64MiB",
                    "--repeats",
                    "2",
                    "--no-plots",
                ]
            )
        # normal-bmp asks for the BMP transform on a text corpus, which the
        # encoder refuses: one configuration fails per repeat while the other
        # succeeds, and both stay in the history.
        self.assertEqual(rc, 1)
        records = telemetry.load_runs([output, output])
        self.assertEqual(len(records), 4)
        self.assertEqual([record["status"] for record in records], ["failed", "ok", "failed", "ok"])
        for record in records:
            if record["status"] == "failed":
                self.assertNotIn("compression_percent", record["metrics"])
            else:
                self.assertEqual(record["codec"]["declared_bytes"], self.source.stat().st_size)
                self.assertEqual(record["codec"]["base_profile"], "fast")
        session_path = next(output.glob("*/session.json"))
        session = json.loads(session_path.read_text())
        self.assertTrue(session["complete"])
        self.assertFalse(session["all_successful"])
        self.assertEqual(session["successful_runs"], 2)
        self.assertEqual(
            telemetry.fingerprint(session_path.parent / "collector.py")["sha256"], session["runner_sha256"]
        )
        self.assertEqual(len((session_path.parent / "runs.jsonl").read_text().splitlines()), 4)
        with (session_path.parent / "runs.csv").open() as source:
            self.assertEqual(len(list(csv.DictReader(source))), 4)

    def test_verified_segmented_matrix_and_all_profiles(self) -> None:
        self.source.write_bytes(b"segment telemetry\n" * 18000)
        output = self.root / "verified-history"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = telemetry.main(
                [
                    "run",
                    "--binary",
                    str(CADAPT),
                    "--corpus",
                    str(self.source),
                    "--output",
                    str(output),
                    "--verify",
                    "--memory",
                    str(256 << 20),
                    "--threads",
                    "2",
                    "--segment-bytes",
                    "262144",
                    "--no-plots",
                ]
            )
        self.assertEqual(rc, 0)
        records = telemetry.load_runs([output])
        # The profile is the invariant; the version number moves whenever a
        # profile gains a family, as it did at 80-85.
        self.assertEqual([record["codec"]["base_profile"] for record in records], ["normal", "fast", "max"])
        self.assertTrue(all(v in telemetry.METADATA_VERSIONS
                            for v in (r["codec"]["format_version"] for r in records)))
        for record in records:
            self.assertTrue(record["config"]["verify"])
            self.assertEqual(record["codec"]["segments"], 2)
            self.assertIn("verified", (Path(record["_directory"]) / "stderr.txt").read_text())

    def test_a_memory_budget_below_the_smallest_preset_is_refused(self) -> None:
        """encode's budget is a preset, so a byte budget no preset fits is a
        usage error rather than a session of runs recorded at a budget the
        encoder never honoured."""
        with contextlib.redirect_stderr(io.StringIO()) as error, contextlib.redirect_stdout(io.StringIO()):
            rc = telemetry.main([
                "run", "--binary", str(CADAPT), "--corpus", str(self.source),
                "--output", str(self.root / "tiny"), "--memory", "1", "--no-plots",
            ])
        self.assertEqual(rc, 2)
        self.assertIn("below the smallest memory preset", error.getvalue())

    def test_a_byte_budget_snaps_down_to_a_preset(self) -> None:
        """The preset chosen is the largest that fits, and the plan records
        that preset's own budget rather than the one asked for."""
        self.assertEqual(telemetry.memory_tier(674 * telemetry.MIB), ("-9", 674 * telemetry.MIB))
        self.assertEqual(telemetry.memory_tier(673 * telemetry.MIB), ("-8", 370 * telemetry.MIB))
        self.assertEqual(telemetry.memory_tier(4 << 30), ("-11", 2 << 30))
        with self.assertRaises(ValueError):
            telemetry.memory_tier(0)

    def test_every_version_the_encoder_writes_is_read_back(self) -> None:
        """The reader's version map comes from adapt.h, so a profile's new
        version family is known without editing this file."""
        self.assertIn(telemetry._C["ADAPT_NORMAL_KEYS_VERSION"], telemetry.KNOWN_VERSIONS)
        self.assertIn(telemetry._C["ADAPT_MAX_KEYS_BINARY_METADATA_VERSION"], telemetry.KNOWN_VERSIONS)
        self.assertIn(telemetry._C["ADAPT_TEXT_MID_METADATA_VERSION"], telemetry.KNOWN_VERSIONS)
        self.assertEqual(telemetry.SIZED_VERSIONS & telemetry.POLICY_VERSIONS, frozenset())
        with self.assertRaises(ValueError):
            telemetry.adapt_constants(self.root / "absent.h")

    def test_cli_reports_bad_input_without_traceback(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()) as error:
            rc = telemetry.main(["plot", str(self.root), "--output", str(self.root / "plots")])
        self.assertEqual(rc, 2)
        self.assertIn("no run.json records", error.getvalue())

    def test_profiles_and_segment_framing(self) -> None:
        """Every profile the CLI writes is read back by profile, not by a
        version number: a container written to a file carries metadata, so its
        version is whatever family that profile currently writes."""
        for profile, name in (([], "normal"), (["--fast"], "fast"), (["--mid"], "mid"), (["--max"], "max")):
            with self.subTest(profile=name):
                output = self.root / f"{name}.adpt"
                subprocess.run(
                    [str(CADAPT), "encode", str(self.source), "-o", str(output), *profile],
                    check=True,
                    capture_output=True,
                )
                metadata = telemetry.inspect_container(output)
                self.assertEqual(metadata["base_profile"], name)
                self.assertIn(metadata["format_version"], telemetry.METADATA_VERSIONS)
                self.assertIn(metadata["key_policy"], ("classic", "text", "binary"))
                self.assertEqual(metadata["metadata_bytes"], 32)
                self.assertEqual(metadata["filesystem_metadata"]["mode"], self.source.stat().st_mode & 0o7777)
                self.assertFalse(metadata["x86_transform"])
                self.assertFalse(metadata["archive"])
                self.assertEqual(metadata["crc32"], telemetry.fingerprint(self.source)["crc32"])
        self.source.write_bytes(b"segment example\n" * 18000)
        output = self.root / "segmented.adpt"
        subprocess.run(
            [str(CADAPT), "encode", str(self.source), "-o", str(output), "--threads", "2", "--segment-bytes", "262144"],
            check=True,
            capture_output=True,
        )
        self.assertEqual(telemetry.inspect_container(output)["segments"], 2)
        output.write_bytes(output.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            telemetry.inspect_container(output)

    def test_metadata_header_corruption_and_truncation_fail_closed(self) -> None:
        output = self.root / "metadata.adpt"
        subprocess.run([str(CADAPT), "encode", str(self.source), "-o", str(output)],
                       check=True, capture_output=True)
        encoded = output.read_bytes()
        base = 8 + (2 if encoded[7] & 0x80 else 0)
        damaged = bytearray(encoded)
        damaged[base] ^= 1
        output.write_bytes(damaged)
        with self.assertRaisesRegex(ValueError, "checksum"):
            telemetry.inspect_container(output)
        for offset, value in ((0, 0o10000), (12, 1_000_000_000), (24, 1_000_000_000)):
            with self.subTest(offset=offset):
                damaged = bytearray(encoded)
                damaged[base + offset:base + offset + 4] = value.to_bytes(4, "little")
                damaged[base + 28:base + 32] = zlib.crc32(damaged[:base + 28]).to_bytes(4, "little")
                output.write_bytes(damaged)
                with self.assertRaisesRegex(ValueError, "invalid filesystem metadata"):
                    telemetry.inspect_container(output)
        for end in range(base, base + 32):
            with self.subTest(end=end):
                output.write_bytes(encoded[:end])
                with self.assertRaisesRegex(ValueError, "truncated"):
                    telemetry.inspect_container(output)

    def test_unknown_history_schema_fails_closed(self) -> None:
        bad = self.root / "run.json"
        bad.write_text('{"schema":"something-else"}')
        with self.assertRaises(ValueError):
            telemetry.load_runs([bad])


if __name__ == "__main__":
    if len(sys.argv) > 1:
        CADAPT = Path(sys.argv.pop(1)).resolve()
    unittest.main()
