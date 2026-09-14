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
                    "fast",
                    "--memory",
                    "1",
                    "64MiB",
                    "--repeats",
                    "2",
                    "--no-plots",
                ]
            )
        self.assertEqual(rc, 1)
        records = telemetry.load_runs([output, output])
        self.assertEqual(len(records), 4)
        self.assertEqual([record["status"] for record in records], ["failed", "ok", "failed", "ok"])
        for record in records:
            if record["status"] == "failed":
                self.assertNotIn("compression_percent", record["metrics"])
            else:
                self.assertEqual(record["codec"]["declared_bytes"], self.source.stat().st_size)
                self.assertEqual(record["codec"]["format_version"], 23)
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
                    "--threads",
                    "2",
                    "--segment-bytes",
                    "262144",
                    "--no-plots",
                ]
            )
        self.assertEqual(rc, 0)
        records = telemetry.load_runs([output])
        self.assertEqual([record["codec"]["format_version"] for record in records], [22, 23, 24])
        for record in records:
            self.assertTrue(record["config"]["verify"])
            self.assertEqual(record["codec"]["segments"], 2)
            self.assertIn("verified", (Path(record["_directory"]) / "stderr.txt").read_text())

    def test_cli_reports_bad_input_without_traceback(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()) as error:
            rc = telemetry.main(["plot", str(self.root), "--output", str(self.root / "plots")])
        self.assertEqual(rc, 2)
        self.assertIn("no run.json records", error.getvalue())

    def test_profiles_and_segment_framing(self) -> None:
        for profile, version in (([], 22), (["--fast"], 23), (["--max"], 24)):
            with self.subTest(version=version):
                output = self.root / f"{version}.adpt"
                subprocess.run(
                    [str(CADAPT), "encode", str(self.source), "-o", str(output), *profile],
                    check=True,
                    capture_output=True,
                )
                metadata = telemetry.inspect_container(output)
                self.assertEqual(metadata["format_version"], version)
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
