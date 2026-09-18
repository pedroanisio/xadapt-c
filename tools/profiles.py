#!/usr/bin/env python3
"""The commands the gates hold every profile to, from the binary — never a
typed list.

`cadapt --help` names the profiles as the usage's bracket group,
`[--fast | --mid | --max]`, with the default profile as no flag; the locked
command is the `arguments` of tools/ratio_floor.json; and the smallest tier
of the max profile that the memory gate has always measured is `--max -6`.
The memory gate (tools/check_memory.py) and the identical-bytes job
(.github/workflows/ci.yml) take their command lists from here, so a fourth
profile is gated the day it appears in --help.

    python3 tools/profiles.py --binary adpat/c/build/cadapt      # one command per line, the default profile an empty line
    python3 tools/profiles.py --binary … --profiles              # the profiles alone

Authored with Claude Opus 5 (Claude Code), 2026-09-18.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLOOR = ROOT / "tools" / "ratio_floor.json"
GROUP = re.compile(r"\[(--[a-z]+(?: \| --[a-z]+)+)\]")


class ProfilesError(Exception):
    """The binary's usage names no profile group, or the floor file no command."""


def profiles_of(help_text: str) -> list[str]:
    """The profiles a usage text names: the default (no flag) and each flag of
    the first bracket group of two or more `--word` alternatives."""
    match = GROUP.search(help_text)
    if match is None:
        raise ProfilesError("the usage names no profile group `[--a | --b | …]`")
    return [""] + [flag.strip() for flag in match.group(1).split("|")]


def profiles(binary: Path) -> list[str]:
    run = subprocess.run([str(binary), "--help"], capture_output=True, text=True, timeout=30)
    return profiles_of(run.stdout + run.stderr)


def locked_command(floor: Path = FLOOR) -> str:
    """The locked command's flags, without the `encode` in front."""
    try:
        arguments = json.loads(floor.read_text(encoding="utf-8"))["arguments"]
    except (OSError, ValueError, KeyError) as error:
        raise ProfilesError(f"{floor}: no locked arguments ({error})") from error
    if not isinstance(arguments, list) or not arguments or arguments[0] != "encode":
        raise ProfilesError(f"{floor}: the locked arguments do not start with `encode`")
    return " ".join(arguments[1:])


def commands(binary: Path, floor: Path = FLOOR) -> list[str]:
    """Every profile, then the locked command and the max profile's -6 tier,
    each once, in that order."""
    out: list[str] = []
    for command in profiles(binary) + [locked_command(floor), "--max -6"]:
        if command not in out:
            out.append(command)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--floor", type=Path, default=FLOOR)
    parser.add_argument("--profiles", action="store_true", help="the profiles alone, not the gate's commands")
    args = parser.parse_args(argv)
    try:
        lines = profiles(args.binary) if args.profiles else commands(args.binary, args.floor)
    except (ProfilesError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    sys.stdout.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
