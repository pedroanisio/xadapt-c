"""Measure any codec on one footing: bytes, CPU and wall seconds, peak memory.

Each codec runs in a separate process. Measurements include CPU and wall time,
peak resident memory, complete output size and byte-exact restoration.

CPU seconds are the figure to read. Threads move work between cores without
removing any of it, so a wall figure credits a threaded run for parallelism it
spent rather than for work it saved.

    python3 -B bench/codec_bench.py --input static/corpus/enwik8
    python3 -B bench/codec_bench.py --input <f> --only cadapt-mid,xz-9e
    python3 -B bench/codec_bench.py --input <f> --json row.json

Codecs are declared in CODECS below: a name, the encode argv, the decode argv
and whether the tool is present. Nothing here is a codec path; it drives
binaries that already exist.

Authored with Claude Opus 5 (Claude Code), 2026-09-17.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CADAPT = ROOT / "adpat/c/build/cadapt"

# One measured process, so its peak RSS is its own and not a parent's. `out`
# names a file to take the child's stdout when the codec writes to a pipe;
# without it stdout is discarded and the codec writes its own files. That is
# the only difference between the two kinds of measurement, so there is one
# runner.
CHILD = (
    "import json,resource,subprocess,sys,time;"
    "argv=json.loads(sys.argv[1]);out=sys.argv[2];"
    "f=open(out,'wb') if out else subprocess.DEVNULL;"
    "t=time.monotonic();"
    "r=subprocess.run(argv,stdout=f,stderr=subprocess.PIPE);"
    "e=time.monotonic()-t;"
    "f.close() if out else None;"
    "u=resource.getrusage(resource.RUSAGE_CHILDREN);"
    "print(json.dumps({'rc':r.returncode,'wall':e,"
    "'cpu':u.ru_utime+u.ru_stime,'rss':u.ru_maxrss*1024,"
    "'err':r.stderr.decode('utf-8','replace')[-400:]}))"
)


class BenchError(RuntimeError):
    """The run could not be made, or a codec did not restore its input."""


@dataclass(frozen=True)
class Row:
    """CPU seconds are the headline: threads move work between cores, they do
    not remove it, so a wall figure flatters a threaded run against a budget
    that is really about work done."""
    codec: str
    input_bytes: int
    output_bytes: int
    ratio: float
    encode_cpu: float
    decode_cpu: float
    encode_wall: float
    decode_wall: float
    encode_rss_mib: float
    decode_rss_mib: float
    restored: bool


def cadapt(name: str, *flags: str, threads: int = 1) -> dict:
    """A cadapt row. With threads > 1 the encoder cuts segments and the
    decoder is told the same count, so both sides are measured threaded."""
    thread_flags = ["--threads", str(threads)] if threads > 1 else []
    return {
        "name": name,
        "tool": str(CADAPT),
        "encode": [str(CADAPT), "encode", *flags, *thread_flags, "{in}", "-o", "{enc}",
                   "--no-verify", "--force"],
        "decode": [str(CADAPT), "decode", *thread_flags, "{enc}", "-o", "{dec}", "--force"],
    }


def CODECS(kanzi_binary: Path | None = None) -> list[dict]:
    """Declared late so a tool installed after import is still found."""
    xz = shutil.which("xz")
    zstd = shutil.which("zstd")
    kanzi = kanzi_binary or (Path(shutil.which("kanzi")) if shutil.which("kanzi") else None)
    rows = [
        cadapt("cadapt-fast", "--fast"),
        cadapt("cadapt-mid", "--mid"),
        cadapt("cadapt-default"),
        cadapt("cadapt-max-lean", "--max", "--allocation", "context", "--contexts", "lean"),
        cadapt("cadapt-max-lean-t4", "--max", "--allocation", "context", "--contexts", "lean", threads=4),
        cadapt("cadapt-mid-t4", "--mid", threads=4),
        cadapt("cadapt-max-lean-8", "--max", "--allocation", "context", "--contexts", "lean", "-8"),
        cadapt("cadapt-max-lean-8-t4", "--max", "--allocation", "context", "--contexts", "lean", "-8", threads=4),
        # --contexts lean is refused with --mid, whose set is fixed at seven,
        # so the only knob left between --fast and --mid is the memory tier.
        cadapt("cadapt-fast-10", "--fast", "-10"),
        cadapt("cadapt-fast-11", "--fast", "-11"),
        cadapt("cadapt-mid-11", "--mid", "-11"),
        cadapt("cadapt-mid-t2", "--mid", threads=2),
        cadapt("cadapt-mid-t3", "--mid", threads=3),
        cadapt("cadapt-default-t4", threads=4),
        cadapt("cadapt-default-t3", threads=3),
    ]
    if xz:
        rows.append({"name": "xz-9e", "tool": xz,
                     "encode": [xz, "-9e", "-T1", "-k", "-f", "-c", "{in}"],
                     "decode": [xz, "-d", "-T1", "-k", "-f", "-c", "{enc}"],
                     "stdout": True})
    if zstd:
        rows.append({"name": "zstd-19", "tool": zstd,
                     "encode": [zstd, "-19", "-T1", "-f", "-q", "-c", "{in}"],
                     "decode": [zstd, "-d", "-f", "-q", "-c", "{enc}"],
                     "stdout": True})
    if kanzi is not None and kanzi.is_file():
        for level in ("7", "8", "9"):
            rows.append({"name": f"kanzi-l{level}", "tool": str(kanzi),
                         "encode": [str(kanzi), "-c", "-i", "{in}", "-o", "{enc}", "-l", level, "-j", "1", "-f"],
                         "decode": [str(kanzi), "-d", "-i", "{enc}", "-o", "{dec}", "-j", "1", "-f"]})
    return rows


def run_once(argv: list[str], out: Path | None = None) -> tuple[float, float, float]:
    """Run one measured process; return (CPU s, wall s, peak RSS).

    With `out` the child's stdout becomes that file, for a codec that writes to
    a pipe; without it the codec writes its own files.
    """
    done = subprocess.run(
        [sys.executable, "-c", CHILD, json.dumps(argv), str(out) if out else ""],
        capture_output=True)
    try:
        info = json.loads(done.stdout.decode())
    except Exception as failure:
        raise BenchError(f"{argv[0]}: no measurement came back ({failure}); {done.stderr.decode()[-300:]}")
    if info["rc"]:
        raise BenchError(f"{argv[0]}: exited {info['rc']}: {info['err'].strip()}")
    return info["cpu"], info["wall"], info["rss"]


def fill(argv: list[str], mapping: dict[str, str]) -> list[str]:
    return [mapping.get(part, part) for part in argv]


def measure(codec: dict, source: Path, scratch: Path) -> Row:
    encoded = scratch / f"{codec['name']}.enc"
    decoded = scratch / f"{codec['name']}.dec"
    mapping = {"{in}": str(source), "{enc}": str(encoded), "{dec}": str(decoded)}
    to_stdout = codec.get("stdout", False)

    enc_argv = fill(codec["encode"], mapping)
    dec_argv = fill(codec["decode"], mapping)
    enc_cpu, enc_wall, enc_rss = run_once(enc_argv, encoded if to_stdout else None)
    dec_cpu, dec_wall, dec_rss = run_once(dec_argv, decoded if to_stdout else None)

    source_bytes = source.stat().st_size
    restored = decoded.exists() and decoded.stat().st_size == source_bytes and \
        subprocess.run(["cmp", "-s", str(decoded), str(source)]).returncode == 0
    row = Row(codec["name"], source_bytes, encoded.stat().st_size,
              round(encoded.stat().st_size / source_bytes, 6),
              round(enc_cpu, 1), round(dec_cpu, 1), round(enc_wall, 1), round(dec_wall, 1),
              round(enc_rss / (1 << 20), 1), round(dec_rss / (1 << 20), 1), restored)
    encoded.unlink(missing_ok=True)
    decoded.unlink(missing_ok=True)
    return row


def render(rows: list[Row]) -> str:
    width = max((len(r.codec) for r in rows), default=6)
    out = [f"{'codec':<{width}} {'bytes':>12} {'ratio':>7} {'encCPU':>8} {'decCPU':>8} "
           f"{'encWall':>8} {'decWall':>8} {'encMiB':>8} {'decMiB':>8}  restored"]
    for r in sorted(rows, key=lambda r: r.output_bytes):
        out.append(f"{r.codec:<{width}} {r.output_bytes:>12,} {r.ratio:>7.4f} {r.encode_cpu:>8.1f} "
                   f"{r.decode_cpu:>8.1f} {r.encode_wall:>8.1f} {r.decode_wall:>8.1f} "
                   f"{r.encode_rss_mib:>8.1f} {r.decode_rss_mib:>8.1f}  "
                   f"{'yes' if r.restored else 'NO'}")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--only", default=None, help="comma-separated codec names")
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument("--scratch", type=Path, default=None,
                        help="external intermediate directory (default: temporary directory)")
    parser.add_argument("--kanzi", type=Path, help="external Kanzi executable; otherwise discover it on PATH")
    args = parser.parse_args(argv)

    if not args.input.exists():
        raise BenchError(f"no input at {args.input}")
    wanted = set(args.only.split(",")) if args.only else None
    codecs = [c for c in CODECS(args.kanzi) if wanted is None or c["name"] in wanted]
    if wanted:
        missing = wanted - {c["name"] for c in codecs}
        if missing:
            raise BenchError(f"not available here: {', '.join(sorted(missing))}")
    if not codecs:
        raise BenchError("no codec available to run")

    if args.json and args.json.resolve().is_relative_to(ROOT):
        raise BenchError("--json must be outside the source checkout")
    if args.scratch and args.scratch.resolve().is_relative_to(ROOT):
        raise BenchError("--scratch must be outside the source checkout")
    with tempfile.TemporaryDirectory(prefix="cadapt-bench-") as temporary:
        scratch = args.scratch or Path(temporary)
        scratch.mkdir(parents=True, exist_ok=True)
        rows: list[Row] = []
        for codec in codecs:
            row = measure(codec, args.input, scratch)
            rows.append(row)
            print(f"{row.codec}|{row.output_bytes}|cpu {row.encode_cpu}/{row.decode_cpu}|"
                  f"wall {row.encode_wall}/{row.decode_wall}|{row.encode_rss_mib}/{row.decode_rss_mib} MiB|"
                  f"{'ok' if row.restored else 'NOT RESTORED'}", flush=True)

    print()
    print(render(rows))
    broken = [r.codec for r in rows if not r.restored]
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"input": str(args.input), "rows": [asdict(r) for r in rows]}, indent=2) + "\n")
    if broken:
        raise BenchError(f"did not restore the input: {', '.join(broken)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BenchError as failure:
        print(f"codec_bench: {failure}", file=sys.stderr)
        sys.exit(1)
