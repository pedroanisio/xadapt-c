#!/usr/bin/env python3
"""Hold the codec to its memory promise: the peak resident set of an encode
or a decode never exceeds what the container declares decoding needs, plus
what a process costs beside its model.

RUN-1 and RUN-2 (docs/CONSTRAINTS.md): the tier bounds the model, the
window and one block's workspace, the file records the model it produced,
and decoding needs that model's memory — `cadapt decode --max-memory 1`
refuses with the exact figure ("need N bytes"). This tool encodes each
fixture under each command, decodes the result, measures both peak
resident sets as the kernel accounts them to the child (wait4's ru_maxrss),
and fails when either exceeds the declared need by more than SLACK — the
process itself: the binary and the C library, the stack, stdio's buffers,
the staged output. The default slack is 16 MiB. This check detects excess
allocations such as a second model or retained whole input. Runtime and allocator
overheads can vary by platform; the allowance is an explicit gate parameter.

    python3 tools/check_memory.py --binary adpat/c/build/cadapt --fixture NAME=PATH ...
        [--command="..."]... [--slack BYTES] [--output-dir DIR]

Without --command the commands are what tools/profiles.py names — every
profile of `cadapt --help`, the locked command of tools/ratio_floor.json
and --max -6 — each without --verify (a proof holds two models and says so
in its own bound); a command's flags begin with a dash, so they are given
as --command="--max -6", not as a separate word. Exit 0 when every
measurement holds, 1 when one exceeds, 2 when the check could not run.

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-17,
with Claude Opus 5 (Claude Code).
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profiles  # noqa: E402  (the commands come from the binary, never a typed list)

SLACK = 16 << 20
NEED = re.compile(rb"need (\d+) bytes")


class CheckError(Exception):
    pass


def run_measured(args: list[str]) -> tuple[int, int, bytes]:
    """Run `args`; return (exit status, peak resident set in bytes, stderr)."""
    with tempfile.TemporaryFile() as err:
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
        _, status, usage = os.wait4(proc.pid, 0)
        proc.returncode = os.waitstatus_to_exitcode(status)
        err.seek(0)
        text = err.read()
    # ru_maxrss is KiB on Linux, bytes on macOS
    rss = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    return proc.returncode, rss, text


def declared_need(binary: str, container: Path) -> int:
    """What the container says decoding needs, from the refusal at a 1-byte cap."""
    run = subprocess.run([binary, "decode", str(container), "--max-memory", "1", "-o", "-"], capture_output=True)
    m = NEED.search(run.stderr)
    if run.returncode != 1 or m is None:
        raise CheckError(f"{container.name}: no declared need in: {run.stderr.decode(errors='replace').strip()}")
    return int(m[1])


def check(binary: str, fixtures: dict[str, Path], commands: list[str], slack: int, out_dir: Path) -> bool:
    ok = True
    for name, path in fixtures.items():
        for command in commands:
            flags = shlex.split(command)
            label = f"{name} encode {command}".rstrip()
            container = out_dir / f"{name}-{'-'.join(f.strip('-') for f in flags) or 'default'}.adpt"
            rc, encode_rss, err = run_measured([binary, "encode", str(path), *flags, "--force", "--no-verify", "-o", str(container)])
            if rc != 0:
                raise CheckError(f"{label}: encode failed: {err.decode(errors='replace').strip()}")
            need = declared_need(binary, container)
            back = out_dir / (container.name + ".out")
            rc, decode_rss, err = run_measured([binary, "decode", str(container), "--force", "-o", str(back)])
            if rc != 0:
                raise CheckError(f"{label}: decode failed: {err.decode(errors='replace').strip()}")
            if back.read_bytes() != path.read_bytes():
                raise CheckError(f"{label}: the round trip differs")
            back.unlink()
            for side, rss in (("encode", encode_rss), ("decode", decode_rss)):
                held = rss <= need + slack
                ok &= held
                print(f"{'PASS' if held else 'FAIL'} {label} {side}: peak {rss:,} B against {need:,} B declared + {slack:,} slack "
                      f"({rss - need:+,} B over the declaration)")
    return ok


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", default="adpat/c/build/cadapt")
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--command", action="append", default=None, metavar="FLAGS",
                        help="encode flags to measure under (repeatable); the default set otherwise")
    parser.add_argument("--slack", type=int, default=SLACK)
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args(argv)
    fixtures = {}
    for spec in args.fixture:
        if "=" not in spec:
            print(f"--fixture takes NAME=PATH, not {spec!r}", file=sys.stderr)
            return 2
        name, path = spec.split("=", 1)
        if not Path(path).is_file():
            print(f"{path}: not a file", file=sys.stderr)
            return 2
        fixtures[name] = Path(path)
    if not fixtures:
        print("at least one --fixture NAME=PATH", file=sys.stderr)
        return 2
    if not os.access(args.binary, os.X_OK):
        print(f"{args.binary}: not executable", file=sys.stderr)
        return 2
    try:
        commands = profiles.commands(args.binary) if args.command is None else args.command
    except profiles.ProfilesError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    try:
        if args.output_dir:
            out = Path(args.output_dir)
            out.mkdir(parents=True, exist_ok=True)
            held = check(args.binary, fixtures, commands, args.slack, out)
        else:
            with tempfile.TemporaryDirectory() as scratch:
                held = check(args.binary, fixtures, commands, args.slack, Path(scratch))
    except CheckError as error:
        print(f"check_memory: {error}", file=sys.stderr)
        return 2
    print("every peak resident set is within its declaration" if held else "a peak resident set exceeds its declaration")
    return 0 if held else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
