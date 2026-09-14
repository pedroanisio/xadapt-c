#!/usr/bin/env python3
"""Check the codec against its locked ratio and throughput floors.

Source: Claude Opus 5 via Claude Code. bench/ratio_floor.json records, per
fixture, the largest container and the lowest throughput the locked command
may produce (LOCKS.md, 2026-09-14). This tool encodes the fixture with the
current binary and fails when the container is larger by even one byte.

Throughput is gated in one of three explicit ways. On the reference host named
in the floor file it is compared with the locked figure, minus the recorded
tolerance (wall time is noisy). With --baseline-binary the baseline encodes the
same fixture in the same run and the current binary must be at least as fast,
minus the tolerance: this works on any host and is what a CI runner should use.
Otherwise throughput is printed and marked "not gated on this host"; size is
still gated. `--verify` also decodes the container and compares byte for byte.

Exit status: 0 every requested floor holds; 1 a floor is broken or the round
trip differs; 2 the check could not run (wrong input, bad floor file, timeout).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "cadapt.ratio-floor.v1"
TOP_KEYS = {"schema", "locked_on", "host", "arguments", "throughput_tolerance", "floors"}
FLOOR_KEYS = {"fixture", "input_bytes", "input_md5", "max_container_bytes", "wall_seconds",
              "min_bytes_per_second", "measured_with"}


class FloorError(ValueError):
    """The floor file is not a valid cadapt.ratio-floor.v1 document."""


@dataclass(frozen=True)
class Floor:
    fixture: str
    input_bytes: int
    input_md5: str
    max_container_bytes: int
    wall_seconds: float
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
    if data["schema"] != SCHEMA:
        raise FloorError(f"{path}: schema {data['schema']!r} is not {SCHEMA}")
    if not isinstance(data["arguments"], list) or not data["arguments"] or data["arguments"][0] != "encode":
        raise FloorError(f"{path}: arguments must start with 'encode'")
    tolerance = data["throughput_tolerance"]
    if isinstance(tolerance, bool) or not isinstance(tolerance, (int, float)) or not 0 <= tolerance < 1:
        raise FloorError(f"{path}: throughput_tolerance must be in [0, 1)")
    if not isinstance(data["floors"], list) or not data["floors"]:
        raise FloorError(f"{path}: floors must be a non-empty list")
    floors = []
    for item in data["floors"]:
        if not isinstance(item, dict) or set(item) != FLOOR_KEYS:
            raise FloorError(f"{path}: each floor needs exactly the keys {sorted(FLOOR_KEYS)}")
        if not re.fullmatch(r"[0-9a-f]{32}", str(item["input_md5"])):
            raise FloorError(f"{path}: {item.get('fixture')}: input_md5 is not an MD5 digest")
        floors.append(Floor(
            fixture=str(item["fixture"]),
            input_bytes=_positive_int(item["input_bytes"], "input_bytes"),
            input_md5=item["input_md5"],
            max_container_bytes=_positive_int(item["max_container_bytes"], "max_container_bytes"),
            wall_seconds=_positive_float(item["wall_seconds"], "wall_seconds"),
            min_bytes_per_second=_positive_float(item["min_bytes_per_second"], "min_bytes_per_second"),
            measured_with=str(item["measured_with"]),
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


def _run(command: list[str], timeout: float) -> float:
    started = time.monotonic()
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise SetupError(f"{command[1]} timed out after {timeout:g} s") from error
    except OSError as error:
        raise SetupError(f"cannot run {command[0]}: {error}") from error
    if result.returncode != 0:
        raise SetupError(f"{command[1]} exited {result.returncode}: {result.stderr.strip()[:400]}")
    return time.monotonic() - started


def check(locked: Locked, floor: Floor, binary: Path, fixture: Path, out_dir: Path, timeout: float,
          verify: bool, baseline: Path | None = None, host: str | None = None) -> tuple[bool, list[str]]:
    """Encode one fixture and compare it with its floor; returns (holds, report lines)."""
    host = host_model() if host is None else host
    lines = []
    if not fixture.is_file():
        raise SetupError(f"{floor.fixture}: fixture {fixture} is not a file")
    size, digest = fixture.stat().st_size, _md5(fixture)
    if size != floor.input_bytes or digest != floor.input_md5:
        raise SetupError(f"{floor.fixture}: {fixture} ({size:,} B, md5 {digest}) does not match the "
                         f"locked input ({floor.input_bytes:,} B, md5 {floor.input_md5})")
    out_dir.mkdir(parents=True, exist_ok=True)
    container = out_dir / f"{floor.fixture}.adpt"
    seconds = _run([str(binary), *locked.arguments, str(fixture), "-o", str(container)], timeout)
    produced = container.stat().st_size
    rate = floor.input_bytes / seconds if seconds > 0 else float("inf")
    holds = True
    delta = produced - floor.max_container_bytes
    verdict = "PASS" if delta <= 0 else "FAIL"
    holds &= delta <= 0
    lines.append(f"{verdict} {floor.fixture} size: {produced:,} B vs floor {floor.max_container_bytes:,} B "
                 f"({delta:+,} B, {100 * delta / floor.max_container_bytes:+.4f} %)")
    tolerance = 1 - locked.throughput_tolerance
    if baseline is not None:
        reference = out_dir / f"{floor.fixture}.baseline.adpt"
        base_seconds = _run([str(baseline), *locked.arguments, str(fixture), "-o", str(reference)], timeout)
        base_rate = floor.input_bytes / base_seconds if base_seconds > 0 else float("inf")
        allowed = base_rate * tolerance
        verdict = "PASS" if rate >= allowed else "FAIL"
        holds &= rate >= allowed
        lines.append(f"{verdict} {floor.fixture} throughput: {rate:,.0f} B/s in {seconds:,.1f} s vs baseline "
                     f"binary {base_rate:,.0f} B/s in {base_seconds:,.1f} s on this host, tolerance "
                     f"{100 * locked.throughput_tolerance:.0f} % → at least {allowed:,.0f} B/s")
    elif host == locked.host:
        allowed = floor.min_bytes_per_second * tolerance
        verdict = "PASS" if rate >= allowed else "FAIL"
        holds &= rate >= allowed
        lines.append(f"{verdict} {floor.fixture} throughput: {rate:,.0f} B/s in {seconds:,.1f} s vs floor "
                     f"{floor.min_bytes_per_second:,.0f} B/s ({floor.wall_seconds:,.0f} s on {locked.host}), "
                     f"tolerance {100 * locked.throughput_tolerance:.0f} % → at least {allowed:,.0f} B/s")
    else:
        lines.append(f"INFO {floor.fixture} throughput: {rate:,.0f} B/s in {seconds:,.1f} s; not gated on this "
                     f"host ({host or 'unknown CPU'} is not {locked.host}); pass --baseline-binary to gate "
                     f"relative to a baseline build")
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
    parser.add_argument("--floor", type=Path, default=ROOT / "bench/ratio_floor.json")
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH",
                        help="fixture to check; default enwik8=static/corpus/enwik8 when present")
    parser.add_argument("--verify", action="store_true", help="also decode and compare byte for byte")
    parser.add_argument("--baseline-binary", type=Path,
                        help="a build of the locked commit; gates throughput relative to it on any host")
    parser.add_argument("--timeout", type=float, default=7200.0, help="seconds per command")
    parser.add_argument("--output-dir", type=Path, help="where containers go (default: a temporary directory)")
    args = parser.parse_args(argv)
    try:
        locked = load_floor(args.floor)
    except FloorError as error:
        parser.error(str(error))
    by_name = {f.fixture: f for f in locked.floors}
    requested = []
    for spec in args.fixture or ([f"enwik8={ROOT / 'static/corpus/enwik8'}"]
                                 if (ROOT / "static/corpus/enwik8").exists() else []):
        name, _, path = spec.partition("=")
        if name not in by_name or not path:
            parser.error(f"unknown fixture {name!r}; the floor file locks {sorted(by_name)}")
        requested.append((by_name[name], Path(path)))
    if not requested:
        parser.error("no fixture to check: pass --fixture NAME=PATH")
    print(f"locked {locked.locked_on}: {' '.join(locked.arguments)} with {args.binary}")
    all_hold = True
    with tempfile.TemporaryDirectory() as temporary:
        out_dir = args.output_dir or Path(temporary)
        for floor, path in requested:
            try:
                holds, lines = check(locked, floor, args.binary, path, out_dir, args.timeout, args.verify,
                                     baseline=args.baseline_binary)
            except SetupError as error:
                print(f"ERROR {error}", file=sys.stderr)
                return 2
            print("\n".join(lines))
            all_hold &= holds
    print("all floors hold" if all_hold else "a locked floor is broken")
    return 0 if all_hold else 1


if __name__ == "__main__":
    raise SystemExit(main())
