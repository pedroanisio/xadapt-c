#!/usr/bin/env python3
"""Record cadapt corpus runs on Linux and regenerate plots from their history."""

from __future__ import annotations

import argparse
import contextlib
import csv
import hashlib
import itertools
import json
import math
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
import zipfile
import zlib
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "cadapt.telemetry.run.v1"
MIB = 1 << 20

# encode's budget is a preset, not a byte count: -0 .. -9 are what xz takes at
# the same preset and -10, -11 are the two tiers above its ladder. --max-memory
# is decode's cap alone and the encoder refuses it, so a byte budget asked for
# here is snapped down to the preset that fits inside it.
MEMORY_TIERS = (
    ("-0", 3 * MIB), ("-1", 9 * MIB), ("-2", 17 * MIB), ("-3", 32 * MIB),
    ("-4", 48 * MIB), ("-5", 94 * MIB), ("-6", 94 * MIB), ("-7", 186 * MIB),
    ("-8", 370 * MIB), ("-9", 674 * MIB), ("-10", 1 << 30), ("-11", 2 << 30),
)


def memory_tier(budget: int) -> tuple[str, int]:
    """The largest preset that fits in `budget`, as (flag, its bytes).

    A budget under the smallest preset is refused rather than rounded up: a
    run recorded at a budget the encoder never honoured is a false record.
    """
    fitting = [(flag, size) for flag, size in MEMORY_TIERS if size <= budget]
    if not fitting:
        raise ValueError(
            f"{budget} bytes is below the smallest memory preset "
            f"({MEMORY_TIERS[0][1]} bytes, {MEMORY_TIERS[0][0]})")
    return max(fitting, key=lambda pair: pair[1])
TIME_FIELDS = {
    "elapsed_seconds": "%e",
    "user_seconds": "%U",
    "system_seconds": "%S",
    "peak_rss_kib": "%M",
    "major_faults": "%F",
    "minor_faults": "%R",
    "voluntary_context_switches": "%w",
    "involuntary_context_switches": "%c",
    "filesystem_input_blocks": "%I",
    "filesystem_output_blocks": "%O",
    "signals_received": "%k",
    "swaps": "%W",
}
TIME_FORMAT = "{" + ",".join(f'"{name}":{code}' for name, code in TIME_FIELDS.items()) + "}"
SAMPLE_FIELDS = [
    "elapsed_seconds",
    "cpu_percent",
    "user_seconds",
    "system_seconds",
    "rss_bytes",
    "virtual_bytes",
    "peak_rss_bytes",
    "swap_bytes",
    "threads",
    "minor_faults",
    "major_faults",
    "voluntary_context_switches",
    "involuntary_context_switches",
    "read_chars",
    "write_chars",
    "read_calls",
    "write_calls",
    "disk_read_bytes",
    "disk_write_bytes",
    "cancelled_write_bytes",
    "input_file_position_bytes",
    "output_file_bytes",
]


def utc_now() -> str:
    """Return a timestamp suitable for provenance records."""
    return datetime.now(timezone.utc).isoformat()


def atomic_json(path: Path, value: Any) -> None:
    """Replace a checkpoint without exposing a partially written JSON document."""
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def fingerprint(path: Path) -> dict[str, Any]:
    """Hash the actual bytes, also computing the codec's CRC-32."""
    digest, checksum, size = hashlib.sha256(), 0, 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(MIB), b""):
            digest.update(chunk)
            checksum = zlib.crc32(chunk, checksum)
            size += len(chunk)
    return {"bytes": size, "sha256": digest.hexdigest(), "crc32": f"{checksum:08x}"}


def byte_count(value: str) -> int:
    """Parse decimal bytes or explicit binary units such as 64MiB."""
    match = re.fullmatch(r"([0-9]+)(B|KiB|MiB|GiB)?", value, re.IGNORECASE)
    if not match:
        raise argparse.ArgumentTypeError("use bytes or KiB, MiB, GiB (for example 64MiB)")
    amount = int(match[1]) * {"b": 1, "kib": 1 << 10, "mib": MIB, "gib": 1 << 30}.get((match[2] or "B").lower(), 1)
    if amount <= 0:
        raise argparse.ArgumentTypeError("byte count must be positive")
    return amount


def positive_int(value: str) -> int:
    """Parse a positive count."""
    amount = int(value)
    if amount <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return amount


def positive_float(value: str) -> float:
    """Reject negative, zero and non-finite sampling periods/timeouts."""
    amount = float(value)
    if not math.isfinite(amount) or amount <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return amount


def prepare_corpus(source: Path, destination: Path, member: str, maximum: int | None) -> dict[str, Any]:
    """Copy an immutable run input; ZIP members are read without extracting paths."""
    with contextlib.ExitStack() as stack:
        if source.suffix.lower() == ".zip":
            archive = stack.enter_context(zipfile.ZipFile(source))
            info = archive.getinfo(member)
            if info.is_dir():
                raise ValueError("the selected ZIP member is a directory")
            stream = stack.enter_context(archive.open(info))
            full_bytes, name = info.file_size, info.filename
        else:
            stream = stack.enter_context(source.open("rb"))
            full_bytes, name = source.stat().st_size, source.name
        wanted = min(full_bytes, maximum) if maximum else full_bytes
        remaining = wanted
        with destination.open("xb") as output:
            while remaining:
                chunk = stream.read(min(MIB, remaining))
                if not chunk:
                    raise ValueError("corpus ended before its declared length")
                output.write(chunk)
                remaining -= len(chunk)
    return {
        "source": str(source),
        "name": name,
        "full_bytes": full_bytes,
        "is_prefix": wanted < full_bytes,
        **fingerprint(destination),
    }


def read_proc_pairs(path: Path) -> dict[str, str]:
    """Read colon-delimited proc fields, preserving their units until conversion."""
    return dict(line.split(":", 1) for line in path.read_text().splitlines() if ":" in line)


def sample_process(pid: int, elapsed: float, corpus: Path, output: Path) -> dict[str, float | int | None]:
    """Sample the codec, including all its threads; unavailable fields remain null."""
    sample: dict[str, float | int | None] = dict.fromkeys(SAMPLE_FIELDS)
    sample["elapsed_seconds"] = elapsed
    proc = Path(f"/proc/{pid}")
    # stat's command name may contain spaces and parentheses.
    stat = proc.joinpath("stat").read_text().rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    sample.update(
        user_seconds=int(stat[11]) / ticks,
        system_seconds=int(stat[12]) / ticks,
        minor_faults=int(stat[7]),
        major_faults=int(stat[9]),
        threads=int(stat[17]),
    )
    with contextlib.suppress(OSError, ValueError):
        fields = read_proc_pairs(proc / "status")
        for source, target in (
            ("VmRSS", "rss_bytes"),
            ("VmSize", "virtual_bytes"),
            ("VmHWM", "peak_rss_bytes"),
            ("VmSwap", "swap_bytes"),
        ):
            if source in fields:
                sample[target] = int(fields[source].split()[0]) * 1024
        for source, target in (
            ("voluntary_ctxt_switches", "voluntary_context_switches"),
            ("nonvoluntary_ctxt_switches", "involuntary_context_switches"),
        ):
            if source in fields:
                sample[target] = int(fields[source])
    with contextlib.suppress(OSError, ValueError):
        fields = read_proc_pairs(proc / "io")
        for source, target in (
            ("rchar", "read_chars"),
            ("wchar", "write_chars"),
            ("syscr", "read_calls"),
            ("syscw", "write_calls"),
            ("read_bytes", "disk_read_bytes"),
            ("write_bytes", "disk_write_bytes"),
            ("cancelled_write_bytes", "cancelled_write_bytes"),
        ):
            if source in fields:
                sample[target] = int(fields[source])
    with contextlib.suppress(OSError, ValueError):
        for descriptor in (proc / "fd").iterdir():
            if descriptor.resolve() == corpus:
                sample["input_file_position_bytes"] = int(read_proc_pairs(proc / "fdinfo" / descriptor.name)["pos"])
                break
    with contextlib.suppress(OSError):
        paths = [output] if output.exists() else list(output.parent.glob(".cadapt-*.part"))
        sample["output_file_bytes"] = sum(path.stat().st_size for path in paths)
    return sample


def stop_process(process: subprocess.Popen) -> None:
    """Stop the private process group and reap the timing wrapper."""
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def sampled_cpu_seconds(sample: dict[str, float | int | None]) -> float | None:
    """Return CPU time only when both mandatory CPU counters were observed."""
    user, system = sample["user_seconds"], sample["system_seconds"]
    return user + system if user is not None and system is not None else None


def measure(
    command: list[str], directory: Path, corpus: Path, output: Path, interval: float, timeout: float, time_binary: Path
) -> dict[str, Any]:
    """Run one command, recording raw stderr, GNU time totals and proc samples."""
    resource_path = directory / "resources.txt"
    wrapped = [str(time_binary), "-f", TIME_FORMAT, "-o", str(resource_path), "--", *command]
    started = time.perf_counter()
    samples, last_sample, child_pid = 0, None, None
    status, error, exit_code = "ok", None, None
    with (
        (directory / "stderr.txt").open("wb") as stderr,
        (directory / "samples.csv").open("w", newline="") as sample_file,
    ):
        writer = csv.DictWriter(sample_file, fieldnames=SAMPLE_FIELDS)
        writer.writeheader()
        process = subprocess.Popen(
            wrapped,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            env={**os.environ, "LC_ALL": "C"},
        )
        try:
            while process.poll() is None:
                elapsed = time.perf_counter() - started
                if elapsed >= timeout:
                    status, error = "timeout", f"exceeded {timeout:g} seconds"
                    stop_process(process)
                    break
                with contextlib.suppress(OSError, ValueError, IndexError, KeyError):
                    if child_pid is None:
                        children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
                        child_pid = int(children[0]) if children else None
                    if child_pid is not None:
                        sample = sample_process(child_pid, elapsed, corpus, output)
                        if last_sample is not None:
                            previous_time = last_sample["elapsed_seconds"]
                            current_cpu, previous_cpu = sampled_cpu_seconds(sample), sampled_cpu_seconds(last_sample)
                            if previous_time is not None and current_cpu is not None and previous_cpu is not None:
                                delta = elapsed - previous_time
                                sample["cpu_percent"] = max(0.0, 100 * (current_cpu - previous_cpu) / delta)
                        writer.writerow(sample)
                        sample_file.flush()
                        samples += 1
                        last_sample = sample
                try:
                    process.wait(timeout=min(interval, max(0.001, timeout - elapsed)))
                except subprocess.TimeoutExpired:
                    pass
        except KeyboardInterrupt:
            status, error = "interrupted", "interrupted by operator"
            stop_process(process)
        finally:
            if process.poll() is None:
                stop_process(process)
            exit_code = process.returncode
    observed = time.perf_counter() - started
    if status == "ok" and exit_code != 0:
        status, error = "failed", f"encoder exited with status {exit_code}"
    metrics: dict[str, Any] = {"observed_wall_seconds": observed}
    try:
        totals = json.loads(next(line for line in resource_path.read_text().splitlines() if line.startswith("{")))
        if any(
            not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0 for value in totals.values()
        ):
            raise ValueError("invalid resource totals")
        metrics.update(totals)
        metrics["peak_rss_bytes"] = metrics.pop("peak_rss_kib") * 1024
        wall = metrics["elapsed_seconds"]
        metrics["cpu_percent"] = 100 * (metrics["user_seconds"] + metrics["system_seconds"]) / wall if wall else None
    except (OSError, ValueError, StopIteration, KeyError) as exception:
        if status == "ok":
            status, error = "telemetry_error", f"missing or invalid GNU time totals: {exception}"
    return {
        "status": status,
        "error": error,
        "exit_code": exit_code,
        "metrics": metrics,
        "sample_count": samples,
        "measurement": "GNU time totals; Linux proc samples",
        "sampling_interval_seconds": interval,
    }


# The container's version map, read from the codec's own header rather than
# transcribed. Transcribing it is what let this reader fall behind: it accepted
# versions 19 to 52 while the encoder had reached 85, and every telemetry run
# that inspected a container failed. adapt.h is the one place a new version is
# declared, so a new version reaches this reader by existing.
ADAPT_H = ROOT / "adpat/c/adapt.h"


def adapt_constants(path: Path = ADAPT_H) -> dict[str, int]:
    """Every `#define ADAPT_<NAME> <integer>` in the codec's header."""
    try:
        text = path.read_text()
    except OSError as failure:
        raise ValueError(f"cannot read the codec header at {path}: {failure}") from failure
    found = dict(re.findall(r"^#define\s+(ADAPT_[A-Z0-9_]+)\s+(\d+)u?\s*$", text, re.M))
    if "ADAPT_FORMAT_VERSION" not in found:
        raise ValueError(f"{path} declares no ADAPT_FORMAT_VERSION; it is not the codec header")
    return {name: int(value) for name, value in found.items()}


_C = adapt_constants()


def _span(first: str, last: str) -> range:
    return range(_C[first], _C[last] + 1)


ADAPTIVE_VERSION = _C["ADAPT_ADAPTIVE_VERSION"]
MAX_ADAPTIVE_VERSION = _C["ADAPT_MAX_ADAPTIVE_VERSION"]
ADAPTIVE_VERSIONS = frozenset(_span("ADAPT_ADAPTIVE_VERSION", "ADAPT_ADAPTIVE_LAST_VERSION")) | \
    frozenset(_span("ADAPT_ADAPTIVE_METADATA_VERSION", "ADAPT_ADAPTIVE_METADATA_LAST_VERSION"))
MAX_ADAPTIVE_VERSIONS = frozenset(_span("ADAPT_MAX_ADAPTIVE_VERSION", "ADAPT_MAX_ADAPTIVE_LAST_VERSION")) | \
    frozenset(_span("ADAPT_MAX_ADAPTIVE_METADATA_VERSION", "ADAPT_MAX_ADAPTIVE_METADATA_LAST_VERSION"))
BMP_VERSIONS = frozenset(_span("ADAPT_BMP_VERSION", "ADAPT_BMP_METADATA_LAST_VERSION")) | \
    frozenset(_span("ADAPT_MAX_BMP_VERSION", "ADAPT_MAX_BMP_METADATA_LAST_VERSION"))
# The sized max families carry the sizing byte; header_is_sized in adapt_decode.c.
SIZED_VERSIONS = frozenset({
    _C["ADAPT_MAX_SIZED_VERSION"], _C["ADAPT_MAX_SIZED_METADATA_VERSION"],
    _C["ADAPT_MAX_PRESSURE_VERSION"], _C["ADAPT_MAX_PRESSURE_METADATA_VERSION"],
}) | frozenset(_span("ADAPT_MAX_KEYS_VERSION", "ADAPT_MAX_KEYS_BINARY_METADATA_VERSION")) | \
    frozenset(_span("ADAPT_MAX_PRECISE_VERSION", "ADAPT_MAX_PRECISE_KEYS_BINARY_METADATA_VERSION"))
# The keyed normal profile carries the policy byte instead; header_has_policy.
POLICY_VERSIONS = frozenset({_C["ADAPT_NORMAL_KEYS_VERSION"], _C["ADAPT_NORMAL_KEYS_METADATA_VERSION"]})
KEYED_MAX_VERSIONS = frozenset(_span("ADAPT_MAX_KEYS_VERSION", "ADAPT_MAX_KEYS_BINARY_METADATA_VERSION")) | frozenset({
    _C["ADAPT_MAX_PRECISE_KEYS_VERSION"], _C["ADAPT_MAX_PRECISE_KEYS_BINARY_VERSION"],
    _C["ADAPT_MAX_PRECISE_KEYS_METADATA_VERSION"], _C["ADAPT_MAX_PRECISE_KEYS_BINARY_METADATA_VERSION"]})
BINARY_KEYED_MAX_VERSIONS = frozenset({
    _C["ADAPT_MAX_KEYS_BINARY_VERSION"], _C["ADAPT_MAX_KEYS_BINARY_METADATA_VERSION"],
    _C["ADAPT_MAX_PRECISE_KEYS_BINARY_VERSION"], _C["ADAPT_MAX_PRECISE_KEYS_BINARY_METADATA_VERSION"]})
MID_VERSIONS = frozenset(_span("ADAPT_MID_VERSION", "ADAPT_TEXT_MID_METADATA_VERSION"))
FAST_VERSIONS = frozenset({_C["ADAPT_FAST_VERSION"], _C["ADAPT_METADATA_FAST_VERSION"],
                           _C["ADAPT_TEXT_FAST_VERSION"], _C["ADAPT_TEXT_FAST_VERSION"] + _C["ADAPT_TEXT_METADATA_OFFSET"]})
TEXT_VERSIONS = frozenset(_span("ADAPT_TEXT_VERSION", "ADAPT_TEXT_METADATA_LAST_VERSION"))
MAX_VERSIONS = frozenset({_C["ADAPT_MAX_VERSION"], _C["ADAPT_METADATA_MAX_VERSION"],
                          _C["ADAPT_MAX_COMPACT_VERSION"], _C["ADAPT_MAX_COMPACT_METADATA_VERSION"],
                          _C["ADAPT_MAX_LEAN_VERSION"], _C["ADAPT_MAX_LEAN_METADATA_VERSION"],
                          _C["ADAPT_TEXT_MAX_VERSION"], _C["ADAPT_TEXT_MAX_COMPACT_VERSION"],
                          _C["ADAPT_TEXT_MAX_LEAN_VERSION"]}) | \
    frozenset(t + _C["ADAPT_TEXT_METADATA_OFFSET"] for t in
              (_C["ADAPT_TEXT_MAX_VERSION"], _C["ADAPT_TEXT_MAX_COMPACT_VERSION"], _C["ADAPT_TEXT_MAX_LEAN_VERSION"])) | \
    SIZED_VERSIONS | MAX_ADAPTIVE_VERSIONS | frozenset(_span("ADAPT_MAX_BMP_VERSION", "ADAPT_MAX_BMP_METADATA_LAST_VERSION"))
METADATA_VERSIONS = frozenset(_span("ADAPT_METADATA_VERSION", "ADAPT_METADATA_MAX_VERSION")) | frozenset({
    _C["ADAPT_MAX_COMPACT_METADATA_VERSION"], _C["ADAPT_MAX_LEAN_METADATA_VERSION"],
    _C["ADAPT_MAX_SIZED_METADATA_VERSION"], _C["ADAPT_MAX_PRESSURE_METADATA_VERSION"],
    _C["ADAPT_MID_METADATA_VERSION"], _C["ADAPT_TEXT_MID_METADATA_VERSION"],
    _C["ADAPT_MAX_KEYS_METADATA_VERSION"], _C["ADAPT_MAX_KEYS_BINARY_METADATA_VERSION"],
    _C["ADAPT_NORMAL_KEYS_METADATA_VERSION"],
}) | frozenset(_span("ADAPT_MAX_PRECISE_METADATA_VERSION", "ADAPT_MAX_PRECISE_KEYS_BINARY_METADATA_VERSION")) | \
    frozenset(_span("ADAPT_TEXT_METADATA_VERSION", "ADAPT_TEXT_METADATA_LAST_VERSION")) | \
    frozenset(_span("ADAPT_ADAPTIVE_METADATA_VERSION", "ADAPT_ADAPTIVE_METADATA_LAST_VERSION")) | \
    frozenset(_span("ADAPT_BMP_METADATA_VERSION", "ADAPT_BMP_METADATA_LAST_VERSION")) | \
    frozenset(_span("ADAPT_MAX_ADAPTIVE_METADATA_VERSION", "ADAPT_MAX_ADAPTIVE_METADATA_LAST_VERSION")) | \
    frozenset(_span("ADAPT_MAX_BMP_METADATA_VERSION", "ADAPT_MAX_BMP_METADATA_LAST_VERSION"))
KNOWN_VERSIONS = frozenset({_C["ADAPT_FORMAT_VERSION"]}) | FAST_VERSIONS | MAX_VERSIONS | MID_VERSIONS | \
    TEXT_VERSIONS | ADAPTIVE_VERSIONS | BMP_VERSIONS | METADATA_VERSIONS | POLICY_VERSIONS

SIZING_COMPACT = _C["ADAPT_SIZING_COMPACT"]
SIZING_LEAN = _C["ADAPT_SIZING_LEAN"]
SIZING_TEXT = _C["ADAPT_SIZING_TEXT"]
SIZING_LOW_START = _C["ADAPT_SIZING_LOW_START"]
SIZING_MODEL2 = _C["ADAPT_SIZING_MODEL2"]
SIZING_FRAC_SHIFT = _C["ADAPT_SIZING_FRAC_SHIFT"]
SIZING_FRAC_MAX = _C["ADAPT_SIZING_FRAC_MAX"]
POLICY_KEYS_MASK = _C["ADAPT_POLICY_KEYS_MASK"]
POLICY_TEXT = _C["ADAPT_POLICY_TEXT"]
KEYS_CLASSIC = _C["ADAPT_KEYS_CLASSIC"]
KEYS_TEXT = _C["ADAPT_KEYS_TEXT"]
KEYS_BINARY = _C["ADAPT_KEYS_BINARY"]


def base_profile(version: int) -> str:
    """Which model the version names: fast, mid, max or normal."""
    if version in FAST_VERSIONS:
        return "fast"
    if version in MID_VERSIONS:
        return "mid"
    if version in MAX_VERSIONS:
        return "max"
    return "normal"


def key_policy(version: int, sizing: int | None, policy: int | None) -> str:
    """The arbiters' key policy: classic, text or binary.

    A keyed max version names its policy in the version itself; the keyed
    normal profile names it in the policy byte. Every other version is classic.
    """
    if policy is not None:
        return {KEYS_TEXT: "text", KEYS_BINARY: "binary"}[policy & POLICY_KEYS_MASK]
    if version in KEYED_MAX_VERSIONS:
        return "binary" if version in BINARY_KEYED_MAX_VERSIONS else "text"
    return "classic"


def text_transform(version: int, sizing: int | None, policy: int | None) -> bool:
    """Whether the text transform is on: named by the version, the sizing byte
    or the policy byte, depending on the family."""
    if policy is not None:
        return bool(policy & POLICY_TEXT)
    if sizing is not None:
        return bool(sizing & SIZING_TEXT)
    return version in TEXT_VERSIONS or version in {_C["ADAPT_TEXT_MID_VERSION"], _C["ADAPT_TEXT_MID_METADATA_VERSION"]}


def context_table_bytes(shift: int, sizing: int | None) -> int:
    """The context table's size, with the sized versions' eighths applied."""
    base = 16 << shift
    if sizing is None:
        return base
    fraction = (sizing >> SIZING_FRAC_SHIFT) & SIZING_FRAC_MAX
    return base + (base * fraction) // 8


def inspect_container(path: Path) -> dict[str, Any]:
    """Summarize framing and block methods, without claiming payload verification."""
    size = path.stat().st_size
    with path.open("rb") as source:

        def read(size: int) -> bytes:
            data = source.read(size)
            if len(data) != size:
                raise ValueError("truncated container")
            return data

        header = read(8)
        version = header[4]
        if header[:4] != b"ADPT" or version not in KNOWN_VERSIONS:
            raise ValueError("unsupported container header")
        period = int.from_bytes(read(2), "little") if header[7] & 0x80 else 0
        # The sizing byte and the policy byte both follow the period, and no
        # version carries both: the sized max families carry the first, the
        # keyed normal profile the second.
        sizing = read(1)[0] if version in SIZED_VERSIONS else None
        policy = read(1)[0] if version in POLICY_VERSIONS else None
        if policy is not None and (policy & ~(POLICY_KEYS_MASK | POLICY_TEXT) or
                                   policy & POLICY_KEYS_MASK not in (KEYS_TEXT, KEYS_BINARY)):
            raise ValueError("invalid key policy byte")
        if sizing is not None and sizing & ~(SIZING_COMPACT | SIZING_LEAN | SIZING_TEXT |
                                             SIZING_LOW_START | SIZING_MODEL2 |
                                             (SIZING_FRAC_MAX << SIZING_FRAC_SHIFT)):
            raise ValueError("invalid sizing byte")
        bmp_descriptor = read(12) if version in BMP_VERSIONS else b""
        bmp = None
        if bmp_descriptor:
            bmp = dict(zip(("offset", "width", "rows"), (int.from_bytes(bmp_descriptor[i:i + 4], "little") for i in (0, 4, 8))))
            if bmp["offset"] < 54 or not (0 < bmp["width"] <= 0x7fffffff and 0 < bmp["rows"] <= 0x7fffffff):
                raise ValueError("invalid BMP descriptor")
        filesystem_metadata = None
        if version in METADATA_VERSIONS:
            metadata = read(32)
            checksum_input = (header +
                              (period.to_bytes(2, "little") if header[7] & 0x80 else b"") +
                              (bytes([sizing]) if sizing is not None else b"") +
                              (bytes([policy]) if policy is not None else b"") +
                              bmp_descriptor + metadata[:28])
            if zlib.crc32(checksum_input) != int.from_bytes(metadata[28:], "little"):
                raise ValueError("metadata header checksum mismatch")
            filesystem_metadata = dict(
                mode=int.from_bytes(metadata[:4], "little"),
                atime_seconds=int.from_bytes(metadata[4:12], "little", signed=True),
                atime_nanoseconds=int.from_bytes(metadata[12:16], "little"),
                mtime_seconds=int.from_bytes(metadata[16:24], "little", signed=True),
                mtime_nanoseconds=int.from_bytes(metadata[24:28], "little"),
            )
            if (filesystem_metadata["mode"] & ~0o7777 or
                    filesystem_metadata["atime_nanoseconds"] >= 1_000_000_000 or
                    filesystem_metadata["mtime_nanoseconds"] >= 1_000_000_000):
                raise ValueError("invalid filesystem metadata")
        if bmp:
            normalized = version - 14 if version >= MAX_ADAPTIVE_VERSION else version
            adaptive = ("normal", "rates", "sparse", "retained")[(normalized - 31) % 4]
        elif version in ADAPTIVE_VERSIONS:
            adaptive = ("rates", "sparse", "retained")[(version - ADAPTIVE_VERSION) % 3]
        elif version in MAX_ADAPTIVE_VERSIONS:
            adaptive = ("rates", "sparse", "retained")[(version - MAX_ADAPTIVE_VERSION) % 3]
        else:
            adaptive = None
        result: dict[str, Any] = dict(
            format_version=version,
            base_profile=base_profile(version),
            adaptive_profile=adaptive,
            key_policy=key_policy(version, sizing, policy),
            text_transform=text_transform(version, sizing, policy),
            sizing_byte=sizing,
            policy_byte=policy,
            bmp_transform=bmp,
            transform_metadata_bytes=len(bmp_descriptor),
            filesystem_metadata=filesystem_metadata,
            metadata_bytes=32 if filesystem_metadata is not None else 0,
            # A sized version's table fraction grows the table an eighth of its
            # next doubling at a time, so the header byte alone understates it.
            context_table_bytes=context_table_bytes(header[5] & 31, sizing),
            recall_table_bytes=4 << (header[6] & 31),
            window_bytes=1 << (header[7] & 31),
            x86_transform=bool(header[7] & 0x20),
            class_history_bytes=(0, 8, 21)[(header[5] >> 5) & 3],
            archive=bool(header[5] & 0x80),
            context_set=header[6] >> 5,
            record_period_bytes=period,
            segments=1,
            modeled_blocks=0,
            stored_observed_blocks=0,
            stored_skipped_blocks=0,
            payload_bytes=0,
        )
        total, table_seen = 0, False
        while True:
            method = read(1)[0]
            if method == 3:
                trailer = read(12)
                result["declared_bytes"] = int.from_bytes(trailer[:8], "little")
                result["crc32"] = f"{int.from_bytes(trailer[8:], 'little'):08x}"
                if result["declared_bytes"] != total or source.tell() != size or bool(header[7] & 0x40) != table_seen:
                    raise ValueError("inconsistent container framing")
                result["framing_bytes"] = size - result["payload_bytes"]
                return result
            if method == 4 and header[7] & 0x40 and not table_seen:
                result["segments"] += 1
            elif method == 5 and header[7] & 0x40 and not table_seen:
                source.seek(result["segments"] * 8, 1)
                if int.from_bytes(read(4), "little") != result["segments"]:
                    raise ValueError("inconsistent segment count")
                table_seen = True
            elif method in (0, 1, 2) and not table_seen:
                block = read(8)
                raw, payload = int.from_bytes(block[:4], "little"), int.from_bytes(block[4:], "little")
                if not 0 < raw <= 262144 or not 0 < payload <= 262144 or source.tell() + payload > size:
                    raise ValueError("invalid block lengths")
                result[("stored_observed_blocks", "modeled_blocks", "stored_skipped_blocks")[method]] += 1
                result["payload_bytes"] += payload
                total += raw
                source.seek(payload, 1)
            else:
                raise ValueError(f"unexpected block method {method}")


def run_one(
    binary: Path,
    corpus: Path,
    fixture: dict[str, Any],
    config: dict[str, Any],
    directory: Path,
    session: dict[str, Any],
    interval: float,
    timeout: float,
    time_binary: Path,
) -> dict[str, Any]:
    """Checkpoint one planned run before starting, then retain success or failure."""
    directory.mkdir()
    output = directory / "encoded.adpt"
    command = [str(binary), "encode", str(corpus), "-o", str(output),
               memory_tier(config["memory_bytes"])[0]]
    profile = config["profile"]
    if profile.endswith("-bmp"):
        command += ["--transform", "bmp"]
        profile = profile[:-4]
    if profile in ("rates", "sparse", "retained", "auto"):
        command += ["--adaptive", profile]
    elif profile != "normal":
        command.append("--" + profile)
    if config["verify"]:
        command.append("--verify")
    if config["threads"] > 1:
        command += ["--threads", str(config["threads"]), "--segment-bytes", str(config["segment_bytes"])]
    record = {
        "schema": SCHEMA,
        "run_id": session["session_id"] + "/" + directory.name,
        "started_at": utc_now(),
        "status": "running",
        "config": config,
        "fixture": fixture,
        "binary": session["binary"],
        "host": session["host"],
        "command": command,
        "metrics": {},
    }
    atomic_json(directory / "run.json", record)
    try:
        record.update(measure(command, directory, corpus, output, interval, timeout, time_binary))
        if record["status"] == "ok":
            identity = fingerprint(output)
            codec = inspect_container(output)
            if codec["declared_bytes"] != fixture["bytes"] or codec["crc32"] != fixture["crc32"]:
                raise ValueError("container metadata disagrees with input identity")
            record["output"] = {"path": output.name, **identity}
            record["codec"] = codec
            metrics = record["metrics"]
            metrics.update({key: value for key, value in codec.items() if isinstance(value, int)})
            metrics.update(input_bytes=fixture["bytes"], output_bytes=identity["bytes"])
            metrics["compression_percent"] = 100 * identity["bytes"] / fixture["bytes"] if fixture["bytes"] else None
            metrics["bits_per_input_byte"] = 8 * identity["bytes"] / fixture["bytes"] if fixture["bytes"] else None
            wall = metrics["elapsed_seconds"]
            metrics["throughput_mib_s"] = fixture["bytes"] / MIB / wall if wall else None
    except (OSError, ValueError, IndexError) as exception:
        record.update(status="failed", error=str(exception))
    record["finished_at"] = utc_now()
    atomic_json(directory / "run.json", record)
    return record


def run_benchmark(args: argparse.Namespace) -> int:
    """Run a sequential configuration matrix and append durable per-run evidence."""
    if sys.platform != "linux":
        raise ValueError("run telemetry requires Linux /proc and GNU time; plots can be generated elsewhere")
    binary, source, time_binary = args.binary.resolve(), args.corpus.resolve(), args.time_binary.resolve()
    timer = subprocess.run([str(time_binary), "--version"], check=True, capture_output=True, text=True, timeout=10)
    if "GNU" not in timer.stdout + timer.stderr:
        raise ValueError("--time-binary must be GNU time")
    if args.segment_bytes % 262144 or args.segment_bytes > 0xFFFFFFFF:
        raise ValueError("--segment-bytes must be whole 262144-byte blocks and fit in u32")
    for budget in args.memory:
        # Caught here rather than when the plan is built, so a budget the
        # encoder cannot honour is a usage error and not a half-written session.
        memory_tier(budget)
    if not args.no_plots:
        import importlib.util

        if importlib.util.find_spec("matplotlib") is None:
            raise ValueError("install bench/requirements.txt for plots, or use --no-plots")
    session_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
    directory = args.output.resolve() / session_id
    directory.mkdir(parents=True)
    (directory / "runs").mkdir()
    shutil.copy2(Path(__file__), directory / "collector.py")
    shutil.copy2(Path(__file__).with_name("plots.py"), directory / "plotter.py")
    original_binary = binary
    binary = directory / "cadapt"
    shutil.copy2(original_binary, binary)
    version = subprocess.run([str(binary), "--version"], check=True, capture_output=True, text=True, timeout=10)
    corpus = directory / "corpus.bin"
    fixture = prepare_corpus(source, corpus, args.member, args.max_bytes)
    configs = [
        dict(
            repeat=repeat,
            profile=profile,
            memory_bytes=memory_tier(memory)[1],
            threads=threads,
            segment_bytes=args.segment_bytes,
            verify=args.verify,
        )
        for repeat, (profile, memory, threads) in itertools.product(
            range(1, args.repeats + 1), itertools.product(args.profiles, args.memory, args.threads)
        )
    ]
    session = {
        "schema": "cadapt.telemetry.session.v1",
        "session_id": session_id,
        "started_at": utc_now(),
        "fixture": fixture,
        "binary": {
            "path": str(binary),
            "source": str(original_binary),
            "version": version.stdout.strip(),
            **fingerprint(binary),
        },
        "runner_sha256": fingerprint(Path(__file__))["sha256"],
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "logical_cpus": os.cpu_count(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)),
            "load_average": list(os.getloadavg()),
        },
        "expected_runs": len(configs),
        "finished_runs": 0,
        "successful_runs": 0,
        "complete": False,
        "configurations": configs,
    }
    atomic_json(directory / "session.json", session)
    print(f"Recording {len(configs)} runs on {fixture['name']} ({fixture['bytes']:,} bytes) in {directory}", flush=True)
    with (directory / "runs.jsonl").open("a", encoding="utf-8") as ledger:
        for index, config in enumerate(configs, 1):
            run_dir = directory / "runs" / f"{index:04d}-{config['profile']}-r{config['repeat']}"
            print(
                f"[{index}/{len(configs)}] {config['profile']} memory={config['memory_bytes'] // MIB} MiB "
                f"threads={config['threads']} verify={config['verify']}",
                flush=True,
            )
            record = run_one(
                binary, corpus, fixture, config, run_dir, session, args.interval, args.timeout, time_binary
            )
            ledger.write(json.dumps(record, allow_nan=False) + "\n")
            ledger.flush()
            session["finished_runs"] += 1
            session["successful_runs"] += record["status"] == "ok"
            session["complete"] = session["finished_runs"] == len(configs) and record["status"] != "interrupted"
            atomic_json(directory / "session.json", session)
            elapsed = record["metrics"].get("observed_wall_seconds", 0)
            print(
                f"  {record['status']}: {elapsed:.3f} s" + (f" — {record['error']}" if record.get("error") else ""),
                flush=True,
            )
            if record["status"] == "interrupted":
                break
    session["finished_at"] = utc_now()
    session["all_successful"] = session["complete"] and session["successful_runs"] == len(configs)
    atomic_json(directory / "session.json", session)
    export_csv(load_runs([directory]), directory / "runs.csv")
    if not args.no_plots:
        from plots import plot_runs

        plot_runs([directory], directory / "plots")
    print(f"Session: {directory}", flush=True)
    return 0 if session["all_successful"] else 1


def load_runs(paths: list[Path]) -> list[dict[str, Any]]:
    """Load every run, including failures and interrupted/running checkpoints."""
    records: dict[str, dict[str, Any]] = {}
    for path in paths:
        candidates = [path] if path.is_file() else sorted(path.rglob("run.json"))
        for candidate in candidates:
            record = json.loads(candidate.read_text(encoding="utf-8"))
            if record.get("schema") != SCHEMA:
                raise ValueError(f"unsupported run schema in {candidate}")
            record["_directory"] = str(candidate.parent.resolve())
            previous = records.get(record["run_id"])
            if previous and previous != record:
                raise ValueError(f"conflicting duplicate run ID: {record['run_id']}")
            records[record["run_id"]] = record
    return sorted(records.values(), key=lambda record: (record["started_at"], record["run_id"]))


def export_csv(records: list[dict[str, Any]], path: Path) -> None:
    """Export all configuration and measured scalar fields without dropping failures."""
    rows = []
    for record in records:
        row = {key: record.get(key) for key in ("run_id", "started_at", "status", "error", "exit_code", "sample_count")}
        row.update(record["config"])
        row.update(
            {
                "corpus": record["fixture"]["name"],
                "corpus_sha256": record["fixture"]["sha256"],
                "corpus_bytes": record["fixture"]["bytes"],
                "is_prefix": record["fixture"]["is_prefix"],
                "binary_sha256": record["binary"]["sha256"],
            }
        )
        row.update(record["metrics"])
        rows.append(row)
    with path.open("w", newline="", encoding="utf-8") as output:
        fields = list(dict.fromkeys(key for row in rows for key in row))
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str] | None = None) -> int:
    """Expose independent run and plot commands."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="encode a corpus and record every run")
    run.add_argument("--binary", type=Path, default=ROOT / "adpat/c/build/cadapt")
    run.add_argument("--corpus", type=Path, default=ROOT / "static/corpus/enwik8.zip")
    run.add_argument("--member", default="enwik8", help="member to read when the corpus is a ZIP")
    run.add_argument("--output", type=Path, required=True, help="external output parent for a new measurement")
    run.add_argument("--profiles", nargs="+", choices=("normal", "fast", "max", "rates", "sparse", "retained", "auto", "normal-bmp", "rates-bmp", "sparse-bmp", "retained-bmp"), default=["normal", "fast", "max"])
    run.add_argument("--repeats", type=positive_int, default=1)
    run.add_argument("--memory", nargs="+", type=byte_count, default=[674 * MIB], metavar="BYTES")
    run.add_argument("--threads", nargs="+", type=positive_int, default=[1])
    run.add_argument("--segment-bytes", type=byte_count, default=16 * MIB)
    run.add_argument("--verify", action="store_true", help="include the encoder's optional round-trip proof in timing")
    run.add_argument(
        "--max-bytes", type=byte_count, help="benchmark only this prefix; records are marked as partial corpus"
    )
    run.add_argument("--interval", type=positive_float, default=0.1, help="process sampling interval in seconds")
    run.add_argument("--timeout", type=positive_float, default=3600, help="timeout for each run in seconds")
    run.add_argument("--time-binary", type=Path, default=Path("/usr/bin/time"))
    run.add_argument("--no-plots", action="store_true", help="collect with only the standard library; plot later")
    plot = commands.add_parser("plot", help="plot all runs in one or more session/history directories")
    plot.add_argument("paths", nargs="+", type=Path)
    plot.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.output.resolve().is_relative_to(ROOT):
            raise ValueError("--output must be outside the source checkout")
        if args.command == "run":
            return run_benchmark(args)
        from plots import plot_runs

        plot_runs(args.paths, args.output.resolve())
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile, ImportError) as exception:
        print(f"error: {exception}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
