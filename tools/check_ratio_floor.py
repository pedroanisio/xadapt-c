#!/usr/bin/env python3
"""Check the codec against its locked ratio and throughput floors.

Source: Claude Opus 5 via Claude Code. tools/ratio_floor.json records, per
fixture, the largest container and the lowest throughput the locked command
may produce (see LOCKS.md). This tool encodes the fixture with the
current binary and fails when the container is larger by even one byte.

Throughput is gated in one of three explicit ways. On the reference host named
in the floor file it is compared with the locked figure, minus the recorded
tolerance. With --baseline-binary the baseline encodes the same fixture in the
same run and the current binary must be at least as fast, minus the tolerance:
this works on any host and is what a CI runner should use. Otherwise throughput
is printed and marked "not gated on this host"; size is still gated. `--verify`
also decodes the container and compares byte for byte.

Each floor names its timing measure: child CPU seconds or elapsed wall seconds.
CPU time excludes runqueue wait but still reflects cache and memory contention.
Use a quiet reference host for absolute floors. On a shared host,
--baseline-binary alternates baseline and candidate runs; --runs N compares
their medians. Every run must emit identical bytes (RUN-5). If child CPU
accounting is unavailable, CPU floors use wall time and report that fallback.

Exit status: 0 every requested floor holds; 1 a floor is broken or the round
trip differs; 2 the check could not run (wrong input, bad floor file, timeout).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import resource
except ImportError:  # not a POSIX platform: no child CPU accounting
    resource = None

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "cadapt.ratio-floor.v1"
STRUCTURED_SCHEMA = "cadapt.ratio-floor.v2"
TOP_KEYS = {"schema", "locked_on", "host", "arguments", "throughput_tolerance", "floors"}
FLOOR_KEYS = {"fixture", "input_bytes", "input_md5", "max_container_bytes", "min_bytes_per_second", "measured_with"}
# a floor's seconds are in exactly one of these measures
MEASURE_KEYS = {"cpu_seconds": "cpu", "wall_seconds": "wall"}


class FloorError(ValueError):
    """The floor file is not a supported ratio-floor document."""


@dataclass(frozen=True)
class Floor:
    fixture: str
    input_bytes: int
    input_md5: str
    max_container_bytes: int
    seconds: float
    measure: str  # "cpu" or "wall": what `seconds` and `min_bytes_per_second` were taken in
    min_bytes_per_second: float
    measured_with: str


@dataclass(frozen=True)
class Locked:
    locked_on: str
    host: str
    arguments: list[str]
    throughput_tolerance: float
    floors: list[Floor]


def _positive_int(value, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise FloorError(f"{name} must be a positive integer")
    return value


def _positive_float(value, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        raise FloorError(f"{name} must be a positive number")
    return float(value)


def load_floor(path: Path) -> Locked:
    """Typed parse of the floor file; any unknown field or bad value is an error."""
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise FloorError(f"{path}: {error}") from error
    if not isinstance(data, dict) or set(data) != TOP_KEYS:
        raise FloorError(f"{path}: expected exactly the keys {sorted(TOP_KEYS)}")
    if data["schema"] not in (SCHEMA, STRUCTURED_SCHEMA):
        raise FloorError(f"{path}: schema {data['schema']!r} is not {SCHEMA}")
    if not isinstance(data["arguments"], list) or not data["arguments"] or data["arguments"][0] != "encode":
        raise FloorError(f"{path}: arguments must start with 'encode'")
    tolerance = data["throughput_tolerance"]
    if isinstance(tolerance, bool) or not isinstance(tolerance, (int, float)) or not 0 <= tolerance < 1:
        raise FloorError(f"{path}: throughput_tolerance must be in [0, 1)")
    if not isinstance(data["floors"], list) or not data["floors"]:
        raise FloorError(f"{path}: floors must be a non-empty list")
    floors = []
    structured = data["schema"] == STRUCTURED_SCHEMA
    required = (FLOOR_KEYS - {"measured_with"}) | {"provenance"} if structured else FLOOR_KEYS
    for item in data["floors"]:
        measures = set(item) & set(MEASURE_KEYS) if isinstance(item, dict) else set()
        if not isinstance(item, dict) or len(measures) != 1 or set(item) - measures != required:
            raise FloorError(f"{path}: each floor needs exactly the keys {sorted(required)} "
                             f"and one of {sorted(MEASURE_KEYS)}")
        if not re.fullmatch(r"[0-9a-f]{32}", str(item["input_md5"])):
            raise FloorError(f"{path}: {item.get('fixture')}: input_md5 is not an MD5 digest")
        (seconds_key,) = measures
        if structured:
            provenance = item["provenance"]
            keys = {"definition_revision", "timing_measure", "verification"}
            if not isinstance(provenance, dict) or set(provenance) != keys:
                raise FloorError(f"{path}: provenance needs exactly {sorted(keys)}")
            if not re.fullmatch(r"[0-9a-f]{40}", str(provenance["definition_revision"])):
                raise FloorError(f"{path}: provenance definition_revision must identify a commit")
            if provenance["timing_measure"] != seconds_key:
                raise FloorError(f"{path}: provenance timing_measure differs from the floor")
            if not isinstance(provenance["verification"], str) or not provenance["verification"].strip():
                raise FloorError(f"{path}: provenance verification must state the recorded check")
            measured_with = json.dumps(provenance, sort_keys=True)
        else:
            measured_with = str(item["measured_with"])
        floors.append(Floor(
            fixture=str(item["fixture"]),
            input_bytes=_positive_int(item["input_bytes"], "input_bytes"),
            input_md5=item["input_md5"],
            max_container_bytes=_positive_int(item["max_container_bytes"], "max_container_bytes"),
            seconds=_positive_float(item[seconds_key], seconds_key),
            measure=MEASURE_KEYS[seconds_key],
            min_bytes_per_second=_positive_float(item["min_bytes_per_second"], "min_bytes_per_second"),
            measured_with=measured_with,
        ))
    if len({f.fixture for f in floors}) != len(floors):
        raise FloorError(f"{path}: duplicate fixture names")
    return Locked(str(data["locked_on"]), str(data["host"]), list(data["arguments"]), float(tolerance), floors)


def host_model() -> str:
    """The CPU model name, as /proc/cpuinfo reports it; empty when unknown."""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return ""


def _md5(path: Path) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _same_bytes(a: Path, b: Path) -> bool:
    if a.stat().st_size != b.stat().st_size:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


class SetupError(RuntimeError):
    """The check could not be carried out; nothing is known about the floor."""


@dataclass(frozen=True)
class Timed:
    """One command's clock time and, where the platform accounts it, the CPU
    time (user and system) the kernel charged to it and its children."""
    wall: float
    cpu: float | None

    def seconds(self, measure: str) -> float:
        return self.wall if measure == "wall" or self.cpu is None else self.cpu


def _children_cpu() -> float | None:
    if resource is None:
        return None
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def _run(command: list[str], timeout: float) -> Timed:
    """Run the command to completion; its clock and CPU times. The CPU time
    is the change in what the kernel has charged this process's waited-for
    children, which after subprocess.run is exactly this child."""
    cpu_before = _children_cpu()
    started = time.monotonic()
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise SetupError(f"{command[1]} timed out after {timeout:g} s") from error
    except OSError as error:
        raise SetupError(f"cannot run {command[0]}: {error}") from error
    wall = time.monotonic() - started
    if result.returncode != 0:
        raise SetupError(f"{command[1]} exited {result.returncode}: {result.stderr.strip()[:400]}")
    cpu_after = _children_cpu()
    return Timed(wall, None if cpu_before is None else cpu_after - cpu_before)


@dataclass(frozen=True)
class Measured:
    """`runs` encodes of one fixture: the container of the first, whether
    every later run wrote the same bytes, and each run's time."""
    container: Path
    consistent: bool
    times: list[Timed]

    def seconds(self, measure: str) -> float:
        return statistics.median(t.seconds(measure) for t in self.times)

    def describe(self, measure: str) -> str:
        """The verdict's seconds, then every run in both measures — a wall
        time well above the CPU time is the host's load showing."""
        other = "wall" if measure == "cpu" else "cpu"
        each = ", ".join(f"{t.seconds(measure):,.1f}" for t in self.times)
        others = ", ".join("-" if t.cpu is None else f"{t.seconds(other):,.1f}" for t in self.times)
        if len(self.times) == 1:
            return f"{each} {measure} s ({others} {other} s)"
        return f"{self.seconds(measure):,.1f} {measure} s (median of {len(self.times)} runs: {each} s; {others} {other} s)"


def _encode(binaries: list[tuple[Path, Path]], arguments: list[str], fixture: Path, timeout: float,
            runs: int) -> list[Measured]:
    """Encode the fixture `runs` times with each binary, the binaries taking
    turns (candidate, baseline, candidate, ...), so that a neighbour's load
    that comes and goes weighs on both alike. Each binary's first run is
    kept as its container; its later runs must match it byte for byte."""
    times = [[] for _ in binaries]
    consistent = [True] * len(binaries)
    # A named file's container carries its access and modification times
    # (the metadata record), and a filesystem mounted relatime moves the
    # access time when a freshly written file is first read — the first
    # encode then records one time and every later one another, and the
    # containers differ in the record while the coded bytes are the same.
    # The fixture's times are put back after every run, so each run sees
    # what the first saw; a noatime mount never moved them.
    stamp = fixture.stat()
    for run in range(runs):
        for k, (binary, container) in enumerate(binaries):
            out = container if run == 0 else container.with_name(f"{container.name}.run{run + 1}")
            times[k].append(_run([str(binary), *arguments, str(fixture), "-o", str(out)], timeout))
            os.utime(fixture, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            if run > 0:
                consistent[k] &= _same_bytes(container, out)
                out.unlink()
    return [Measured(container, consistent[k], times[k]) for k, (_, container) in enumerate(binaries)]


def check(locked: Locked, floor: Floor, binary: Path, fixture: Path, out_dir: Path, timeout: float,
          verify: bool, baseline: Path | None = None, host: str | None = None, runs: int = 1) -> tuple[bool, list[str]]:
    """Encode one fixture and compare it with its floor; returns (holds, report lines)."""
    host = host_model() if host is None else host
    lines = []
    if not fixture.is_file():
        raise SetupError(f"{floor.fixture}: fixture {fixture} is not a file")
    # the hash reads the fixture, which on a relatime mount moves the access
    # time a named file's container records: put it back, so every encode
    # sees the fixture as it was given (see _encode)
    stamp = fixture.stat()
    size, digest = stamp.st_size, _md5(fixture)
    os.utime(fixture, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    if size != floor.input_bytes or digest != floor.input_md5:
        raise SetupError(f"{floor.fixture}: {fixture} ({size:,} B, md5 {digest}) does not match the "
                         f"locked input ({floor.input_bytes:,} B, md5 {floor.input_md5})")
    container = out_dir / f"{floor.fixture}.adpt"
    container.parent.mkdir(parents=True, exist_ok=True)  # a corpus fixture is named corpus/file
    measure = floor.measure
    if measure == "cpu" and resource is None:
        lines.append(f"WARN {floor.fixture}: this platform does not account a child's CPU time; the CPU floor "
                     f"is compared against wall time, which a busy host stretches")
        measure = "wall"
    reference = out_dir / f"{floor.fixture}.baseline.adpt"
    binaries = [(binary, container)] + ([(baseline, reference)] if baseline is not None else [])
    measured, *rest = _encode(binaries, locked.arguments, fixture, timeout, runs)
    seconds = measured.seconds(measure)
    produced = container.stat().st_size
    rate = floor.input_bytes / seconds if seconds > 0 else float("inf")
    holds = True
    delta = produced - floor.max_container_bytes
    verdict = "PASS" if delta <= 0 else "FAIL"
    holds &= delta <= 0
    lines.append(f"{verdict} {floor.fixture} size: {produced:,} B vs floor {floor.max_container_bytes:,} B "
                 f"({delta:+,} B, {100 * delta / floor.max_container_bytes:+.4f} %)")
    if not measured.consistent:
        holds = False
        lines.append(f"FAIL {floor.fixture}: the {runs} runs' containers differ; the same command must write "
                     f"the same bytes (RUN-5)")
    tolerance = 1 - locked.throughput_tolerance
    if baseline is not None:
        (base,) = rest
        base_seconds = base.seconds(measure)
        base_rate = floor.input_bytes / base_seconds if base_seconds > 0 else float("inf")
        allowed = base_rate * tolerance
        verdict = "PASS" if rate >= allowed else "FAIL"
        holds &= rate >= allowed
        lines.append(f"{verdict} {floor.fixture} throughput: {rate:,.0f} B/s in {measured.describe(measure)} vs "
                     f"baseline binary {base_rate:,.0f} B/s in {base.describe(measure)} on this host, tolerance "
                     f"{100 * locked.throughput_tolerance:.0f} % → at least {allowed:,.0f} B/s")
    elif host == locked.host:
        allowed = floor.min_bytes_per_second * tolerance
        verdict = "PASS" if rate >= allowed else "FAIL"
        holds &= rate >= allowed
        lines.append(f"{verdict} {floor.fixture} throughput: {rate:,.0f} B/s in {measured.describe(measure)} vs "
                     f"floor {floor.min_bytes_per_second:,.0f} B/s ({floor.seconds:,.0f} {floor.measure} s on "
                     f"{locked.host}), tolerance {100 * locked.throughput_tolerance:.0f} % → at least "
                     f"{allowed:,.0f} B/s")
    else:
        lines.append(f"INFO {floor.fixture} throughput: {rate:,.0f} B/s in {measured.describe(measure)}; not gated "
                     f"on this host ({host or 'unknown CPU'} is not {locked.host}); pass --baseline-binary to "
                     f"gate relative to a baseline build")
    if verify:
        restored = out_dir / f"{floor.fixture}.restored"
        _run([str(binary), "decode", str(container), "-o", str(restored)], timeout)
        if _same_bytes(fixture, restored):
            lines.append(f"PASS {floor.fixture} round trip verified byte for byte")
        else:
            holds = False
            lines.append(f"FAIL {floor.fixture} round trip differs from the input")
    return holds, lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=ROOT / "adpat/c/build/cadapt")
    parser.add_argument("--floor", type=Path, default=ROOT / "tools/ratio_floor.json")
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH",
                        help="fixture to check; default enwik8=static/corpus/enwik8 when present")
    parser.add_argument("--corpus", type=Path, metavar="DIR",
                        help="check every floor in the file, each fixture at DIR/<fixture name> "
                             "(the mixed corpus of tools/fetch_corpus.py)")
    parser.add_argument("--verify", action="store_true", help="also decode and compare byte for byte")
    parser.add_argument("--baseline-binary", type=Path,
                        help="a build of the locked commit; gates throughput relative to it on any host")
    parser.add_argument("--timeout", type=float, default=7200.0, help="seconds per command")
    parser.add_argument("--runs", type=int, default=1,
                        help="encode each fixture this many times and take the median time (default 1); "
                             "every run must write the same bytes")
    parser.add_argument("--output-dir", type=Path, help="where containers go (default: a temporary directory)")
    args = parser.parse_args(argv)
    if args.runs < 1:
        parser.error("--runs must be one or more")
    try:
        locked = load_floor(args.floor)
    except FloorError as error:
        parser.error(str(error))
    by_name = {f.fixture: f for f in locked.floors}
    requested = []
    if args.corpus is not None:
        requested.extend((f, args.corpus / f.fixture) for f in locked.floors)
    for spec in args.fixture or ([f"enwik8={ROOT / 'static/corpus/enwik8'}"]
                                 if not requested and (ROOT / "static/corpus/enwik8").exists() else []):
        name, _, path = spec.partition("=")
        if name not in by_name or not path:
            parser.error(f"unknown fixture {name!r}; the floor file locks {sorted(by_name)}")
        requested.append((by_name[name], Path(path)))
    if not requested:
        parser.error("no fixture to check: pass --fixture NAME=PATH or --corpus DIR")
    print(f"locked {locked.locked_on}: {' '.join(locked.arguments)} with {args.binary}")
    all_hold = True
    with tempfile.TemporaryDirectory() as temporary:
        out_dir = args.output_dir or Path(temporary)
        for floor, path in requested:
            try:
                holds, lines = check(locked, floor, args.binary, path, out_dir, args.timeout, args.verify,
                                     baseline=args.baseline_binary, runs=args.runs)
            except SetupError as error:
                print(f"ERROR {error}", file=sys.stderr)
                return 2
            print("\n".join(lines))
            all_hold &= holds
    print("all floors hold" if all_hold else "a locked floor is broken")
    return 0 if all_hold else 1


if __name__ == "__main__":
    raise SystemExit(main())
