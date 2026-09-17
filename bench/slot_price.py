"""Price every hashed context slot, in bytes and in seconds.

Each of the max profile's hashed contexts costs three arbiter lanes of every
bit and one claim of every nibble. This tool measures what each one is worth
by encoding a fixture with that kind dropped and comparing the container
against the baseline, then walks the kinds cheapest first to map the
ratio-against-speed frontier of deleting several at once.

It drives a development build (`-DADAPT_SIZES_OVERRIDE`), whose
`ADAPT_DROP_MASK` leaves kinds out for exactly this ablation. Containers
written under a mask decode only under the same mask, so nothing this tool
writes is a deliverable: the numbers are, and they are reproduced by running
it again.

    python3 -B bench/slot_price.py --fixture static/corpus/enwik8 --limit 8MiB
    python3 -B bench/slot_price.py --fixture <f> --frontier --json out.json

Authored with Claude Opus 5 (Claude Code), 2026-09-16.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The hashed contexts, in the bit order ADAPT_DROP_MASK uses (H_* in
# adpat/c/adapt_constants.h, a section of adapt_model.h). The tool refuses
# to run if the header disagrees.
KINDS = [
    "o1", "o2", "o3", "o4", "o6", "word", "field", "sparse", "record", "class",
    "word2", "sparse2", "indirect", "nibbles", "nest", "o5", "o8", "wordlc",
    "word3", "wordc",
]

BASE_ARGUMENTS = ["encode", "--max", "--allocation", "context", "--contexts", "lean"]


class ProbeError(RuntimeError):
    """The sweep could not be run or its inputs did not agree with the code."""


@dataclass(frozen=True)
class Row:
    label: str
    mask: int
    bytes: int
    delta: int
    percent: float
    seconds: float
    speedup: float


def parse_size(text: str) -> int:
    """Accept 8388608, 8MiB, 100MB."""
    units = {"": 1, "B": 1, "KIB": 1 << 10, "MIB": 1 << 20, "GIB": 1 << 30,
             "KB": 1000, "MB": 1000 ** 2, "GB": 1000 ** 3}
    upper = text.strip().upper()
    for suffix, scale in sorted(units.items(), key=lambda kv: -len(kv[0])):
        if suffix and upper.endswith(suffix):
            return int(float(upper[: -len(suffix)]) * scale)
    return int(upper)


def read_kind_order(header: Path) -> list[str]:
    """The H_* bit order as the codec defines it, for the agreement check."""
    order: list[tuple[int, str]] = []
    for line in header.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "#define" and fields[1].startswith("H_"):
            try:
                order.append((int(fields[2]), fields[1][2:].lower()))
            except ValueError:
                continue
    order.sort()
    if [bit for bit, _ in order] != list(range(len(order))):
        raise ProbeError("the H_* defines are not a dense bit range")
    return [name for _, name in order]


def build_probe_binary(into: Path) -> Path:
    """Build the development codec the ablation needs, out of tree."""
    command = [
        "make", "-C", str(ROOT / "adpat/c"), f"BUILD={into}",
        "CFLAGS=-std=c99 -O3 -Wall -Wextra -Wpedantic -Werror "
        "-DADAPT_THREADS -pthread -DADAPT_SIZES_OVERRIDE",
        "-j", str(max(1, os.cpu_count() or 1)),
    ]
    built = subprocess.run(command, capture_output=True)
    if built.returncode:
        raise ProbeError(built.stderr.decode(errors="replace"))
    binary = into / "cadapt"
    if not binary.exists():
        raise ProbeError(f"the build produced no {binary}")
    return binary


def encode(binary: Path, fixture: Path, out: Path, mask: int, repeats: int) -> tuple[int, float]:
    """Encode under a drop mask; return the container's bytes and the best time."""
    environment = dict(os.environ)
    if mask:
        environment["ADAPT_DROP_MASK"] = str(mask)
    else:
        environment.pop("ADAPT_DROP_MASK", None)
    best = float("inf")
    for _ in range(max(1, repeats)):
        start = time.monotonic()
        done = subprocess.run(
            [str(binary), *BASE_ARGUMENTS, str(fixture), "-o", str(out), "--no-verify", "--force"],
            capture_output=True, env=environment,
        )
        elapsed = time.monotonic() - start
        if done.returncode:
            raise ProbeError(f"mask {mask}: {done.stderr.decode(errors='replace').strip()}")
        best = min(best, elapsed)
    return out.stat().st_size, best


def sweep(binary: Path, fixture: Path, scratch: Path, repeats: int, frontier: bool) -> list[Row]:
    out = scratch / "container.adpt"
    base_bytes, base_seconds = encode(binary, fixture, out, 0, repeats)
    rows = [Row("baseline", 0, base_bytes, 0, 0.0, round(base_seconds, 2), 1.0)]

    def record(label: str, mask: int) -> Row:
        size, seconds = encode(binary, fixture, out, mask, repeats)
        row = Row(label, mask, size, size - base_bytes,
                  round(100.0 * (size - base_bytes) / base_bytes, 4),
                  round(seconds, 2), round(base_seconds / seconds, 3))
        rows.append(row)
        return row

    singles = [(record(KINDS[bit], 1 << bit), bit) for bit in range(len(KINDS))]
    if frontier:
        # Kinds already outside the lean set price at zero; they are not
        # candidates, they are simply absent.
        live = [(row, bit) for row, bit in singles if row.delta > 0]
        live.sort(key=lambda pair: pair[0].delta)
        mask = 0
        for index, (_, bit) in enumerate(live, start=1):
            mask |= 1 << bit
            record(f"cheapest-{index}", mask)
    return rows


def render(rows: list[Row]) -> str:
    width = max(len(row.label) for row in rows)
    lines = [f"{'set':<{width}} {'mask':>8} {'bytes':>10} {'delta':>9} {'%':>8} {'s':>7} {'speedup':>8}"]
    for row in rows:
        lines.append(f"{row.label:<{width}} {row.mask:>8} {row.bytes:>10} {row.delta:>9} "
                     f"{row.percent:>8.3f} {row.seconds:>7.2f} {row.speedup:>7.3f}x")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--limit", default=None, help="price only the first N bytes (8MiB, 100MB, …)")
    parser.add_argument("--repeats", type=int, default=1, help="encodes per point; the fastest counts")
    parser.add_argument("--frontier", action="store_true",
                        help="after the per-kind prices, drop kinds cheapest first, cumulatively")
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument("--binary", type=Path, default=None,
                        help="an already built development codec; otherwise one is built")
    args = parser.parse_args(argv)

    declared = read_kind_order(ROOT / "adpat/c/adapt_constants.h")
    if declared != KINDS:
        raise ProbeError(f"the codec's kinds are {declared}, this tool holds {KINDS}")
    if not args.fixture.exists():
        raise ProbeError(f"no fixture at {args.fixture}")

    with tempfile.TemporaryDirectory() as name:
        scratch = Path(name)
        binary = args.binary.resolve() if args.binary else build_probe_binary(scratch / "build")
        fixture = args.fixture
        if args.limit:
            limit = parse_size(args.limit)
            cut = scratch / "fixture"
            with args.fixture.open("rb") as source, cut.open("wb") as target:
                target.write(source.read(limit))
            fixture = cut
        rows = sweep(binary, fixture, scratch, args.repeats, args.frontier)

    print(render(rows))
    if args.json:
        args.json.write_text(json.dumps({"kinds": KINDS, "rows": [asdict(r) for r in rows]}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ProbeError as failure:
        print(f"slot_price: {failure}", file=sys.stderr)
        sys.exit(1)
