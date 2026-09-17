"""The feature macros must reach the system headers, and the build must say so.

_POSIX_C_SOURCE and _GNU_SOURCE decide what the system headers declare. A
translation unit that includes a system header before them compiles without
them, and the code that needs the missing declarations takes its fallback
branch silently: the sparse map reports no holes, every restored file comes
back dense, and the only thing that notices is an end-to-end folder test
inside the coverage build. cadapt_platform.h refuses to compile in that state
instead. These tests hold that refusal.

Run after make; an optional positional argument names the compiler.

Authored with Claude Opus 5 (Claude Code), 2026-09-17.
"""

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CODEC = Path(__file__).resolve().parent.parent
GUARD = "must be included before any system header"


def compile_source(body: str) -> subprocess.CompletedProcess:
    """Compile one translation unit against the codec headers."""
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "unit.c"
        source.write_text(body)
        return subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-I", str(CODEC),
             "-c", "-o", str(Path(scratch) / "unit.o"), str(source)],
            capture_output=True, text=True)


class PlatformHeaderTest(unittest.TestCase):
    def test_the_platform_header_first_makes_the_sparse_map_visible(self):
        """SEEK_HOLE and SEEK_DATA are what the sparse map is built on."""
        if sys.platform != "linux":
            self.skipTest("the guard covers Linux, where _GNU_SOURCE hides these")
        done = compile_source(
            '#include "cadapt_fs.h"\n'
            "#if !defined(SEEK_HOLE) || !defined(SEEK_DATA)\n"
            '#error "the sparse map would compile to its no-holes fallback"\n'
            "#endif\n"
            "int main(void) { return 0; }\n")
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_a_system_header_before_the_platform_header_fails_the_build(self):
        """The failure must be here, at the cause, not in a folder test."""
        if sys.platform != "linux":
            self.skipTest("the guard covers Linux, where _GNU_SOURCE hides these")
        done = compile_source(
            "#include <stdio.h>\n"
            '#include "cadapt_fs.h"\n'
            "int main(void) { return 0; }\n")
        self.assertNotEqual(done.returncode, 0, "the wrong include order compiled")
        self.assertIn(GUARD, done.stderr)

    def test_the_codec_header_before_the_platform_header_fails_the_build(self):
        """adapt.h pulls in system headers, which is how this happened."""
        if sys.platform != "linux":
            self.skipTest("the guard covers Linux, where _GNU_SOURCE hides these")
        done = compile_source(
            '#include "adapt.h"\n'
            '#include "cadapt_fs.h"\n'
            "int main(void) { return 0; }\n")
        self.assertNotEqual(done.returncode, 0, "the wrong include order compiled")
        self.assertIn(GUARD, done.stderr)

    def test_the_command_line_and_its_filesystem_layer_include_it_first(self):
        """Every unit that takes the platform header takes it before anything."""
        for name in ("cadapt.c", "cadapt_folder.c", "cadapt_folder.h", "cadapt_fs.c", "cadapt_fs.h"):
            with self.subTest(unit=name):
                includes = [line for line in (CODEC / name).read_text().splitlines()
                            if line.startswith("#include")]
                self.assertTrue(includes, f"{name} includes nothing")
                self.assertEqual(includes[0], '#include "cadapt_platform.h"',
                                 f"{name} must include cadapt_platform.h first")


if __name__ == "__main__":
    unittest.main()
