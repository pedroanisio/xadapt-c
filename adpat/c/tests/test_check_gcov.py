"""The gcov branch-coverage gate accepts the floors and refuses everything
else: a low file, a missing file, a duplicated file, a malformed report.

Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
"""

import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("check_gcov", HERE.parent / "check-gcov.py")
check_gcov = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(check_gcov)


def report(files: dict[str, tuple[int, int]], version: str = "1") -> dict:
    """A gcov JSON report for one unit: each file with `count` branches,
    `covered` of them taken."""
    return {
        "format_version": version,
        "gcc_version": "13.3.0",
        "files": [
            {
                "file": name,
                "functions": [],
                "lines": [
                    {
                        "line_number": 1,
                        "count": 1,
                        "branches": [
                            {
                                "count": 1 if i < covered else 0,
                                "throw": False,
                                "fallthrough": False,
                            }
                            for i in range(count)
                        ],
                    }
                ],
            }
            for name, (covered, count) in files.items()
        ],
    }


def units(
    decode: tuple[int, int],
    encode: tuple[int, int],
    cli: tuple[int, int],
    header: tuple[int, int] = (9, 10),
    archive: tuple[int, int] = (10, 10),
):
    """The four unit reports the gate reads: the header appears in both
    library units that inline it, as gcov reports an inlined header; the
    archive unit stands alone. The command line's report comes last."""
    return [
        report({"adapt_decode.c": decode, "adapt_model.h": header, "adapt_adaptive.h": (0, 0), "adapt_bmp.h": (0, 0),
                "adapt_text.h": (0, 0)}),
        report({"adapt_encode.c": encode, "adapt_model.h": header, "adapt_adaptive.h": (0, 0), "adapt_bmp.h": (0, 0),
                "adapt_text.h": (0, 0)}),
        report({"adapt_archive.c": archive}),
        report({"cadapt.c": cli}),
    ]


class GateTest(unittest.TestCase):
    def test_make_reruns_gate_tests_after_a_success_marker(self) -> None:
        build = Path(self.dir.name) / "build"
        marker = build / "runs" / "gcov-tests"
        marker.parent.mkdir(parents=True)
        marker.touch()
        for changed in (None, "check-gcov.py", "tests/test_check_gcov.py"):
            with self.subTest(changed=changed):
                command = ["make", "-n", f"BUILD={build}", str(marker)]
                if changed is not None:
                    command += ["-W", changed]
                run = subprocess.run(command, cwd=HERE.parent, capture_output=True, text=True, check=True)
                self.assertIn("tests/test_check_gcov.py", run.stdout)

    def test_new_algorithm_headers_are_required_and_counted(self) -> None:
        for header in ("adapt_adaptive.h", "adapt_bmp.h", "adapt_text.h"):
            evidence = units((50, 50), (50, 50), (90, 100))
            for unit in evidence:
                unit["files"] = [entry for entry in unit["files"] if entry["file"] != header]
            code, _, error = self.run_gate(*self.write_units(evidence))
            self.assertEqual(code, 1)
            self.assertIn(header, error)
            evidence[0]["files"].extend(report({header: (0, 200)})["files"])
            code, output, _ = self.run_gate(*self.write_units(evidence))
            self.assertEqual(code, 1)
            self.assertIn("LOW library", output)

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name: str, content: object) -> str:
        path = Path(self.dir.name) / name
        path.write_text(
            content if isinstance(content, str) else json.dumps(content),
            encoding="utf-8",
        )
        return str(path)

    def write_units(self, reports: list, prefix: str = "u") -> list[str]:
        return [self.write(f"{prefix}{i}.json", r) for i, r in enumerate(reports)]

    def run_gate(self, *paths: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = check_gcov.main(list(paths))
        return code, out.getvalue(), err.getvalue()

    def test_floors_pass_at_the_boundary_and_fail_below_it(self) -> None:
        # library: 40 + 40 + 9 + 9 + 10 of 50 + 50 + 10 + 10 + 10 = 108/130 = 83.1 %
        code, out, _ = self.run_gate(*self.write_units(units((40, 50), (40, 50), (60, 100))))
        self.assertEqual(code, 0, out)
        self.assertIn("ok  library  branches 108/130 = 83.1% (floor 80%)", out)
        self.assertIn("ok  cli      branches 60/100 = 60.0% (floor 60%)", out)
        self.assertIn("u0.json:adapt_model.h", out)
        self.assertIn("u1.json:adapt_model.h", out)
        self.assertIn("adapt_archive.c", out)
        # exactly the floor passes; one branch fewer fails
        code, out, _ = self.run_gate(
            *self.write_units(units((40, 50), (40, 50), (60, 100), header=(8, 10), archive=(8, 10)), "b")
        )
        self.assertEqual(code, 0, out)
        self.assertIn("104/130 = 80.0%", out)
        code, out, _ = self.run_gate(
            *self.write_units(units((39, 50), (40, 50), (60, 100), header=(8, 10), archive=(8, 10)), "c")
        )
        self.assertEqual(code, 1)
        self.assertIn("LOW library", out)
        code, out, _ = self.run_gate(*self.write_units(units((50, 50), (50, 50), (59, 100)), "d"))
        self.assertEqual(code, 1)
        self.assertIn("LOW cli", out)

    def test_a_missing_or_duplicated_source_fails(self) -> None:
        paths = self.write_units(units((50, 50), (50, 50), (90, 100)))
        code, _, err = self.run_gate(*paths[:3])
        self.assertEqual(code, 1)
        self.assertIn("lacks ['cadapt.c']", err)
        code, _, err = self.run_gate(*paths[:2], paths[3])
        self.assertEqual(code, 1)
        self.assertIn("lacks ['adapt_archive.c']", err)
        code, _, err = self.run_gate(*paths, paths[0])
        self.assertEqual(code, 1)
        self.assertIn("duplicate source evidence", err)
        twice = self.write("twice.json", report({"adapt_decode.c": (5, 5), "adapt_encode.c": (5, 5)}))
        code, _, err = self.run_gate(twice, paths[1], paths[2], paths[3])
        self.assertEqual(code, 1)
        self.assertIn("duplicate source evidence", err)

    def test_other_sources_are_ignored_and_paths_are_matched_by_name(self) -> None:
        reports = units((50, 50), (50, 50), (90, 100))
        reports[0]["files"][0]["file"] = "/build/../c/adapt_decode.c"
        reports[3]["files"].append(report({"tests/test_adapt_decode.c": (0, 100)})["files"][0])
        code, out, _ = self.run_gate(*self.write_units(reports))
        self.assertEqual(code, 0, out)
        self.assertNotIn("test_adapt_decode", out)

    def test_malformed_reports_fail_closed(self) -> None:
        paths = self.write_units(units((50, 50), (50, 50), (90, 100)))
        for name, content in [
            ("not-json.json", "{"),
            ("version.json", report({"adapt_decode.c": (5, 5)}, version="2")),
            ("no-files.json", {"format_version": "1", "files": []}),
            (
                "count.json",
                {
                    "format_version": "1",
                    "files": [
                        {
                            "file": "adapt_decode.c",
                            "lines": [{"branches": [{"count": "1"}]}],
                        }
                    ],
                },
            ),
            (
                "negative.json",
                {
                    "format_version": "1",
                    "files": [
                        {
                            "file": "adapt_decode.c",
                            "lines": [{"branches": [{"count": -1}]}],
                        }
                    ],
                },
            ),
        ]:
            code, _, err = self.run_gate(self.write(name, content), paths[1], paths[2], paths[3])
            self.assertEqual(code, 1, name)
            self.assertIn("invalid coverage report", err, name)
        code, _, err = self.run_gate(str(Path(self.dir.name) / "absent.json"), paths[1], paths[2], paths[3])
        self.assertEqual(code, 1)
        self.assertIn("invalid coverage report", err)

    def test_zero_branches_is_zero_percent(self) -> None:
        code, out, _ = self.run_gate(*self.write_units(units((0, 0), (0, 0), (90, 100), header=(0, 0), archive=(0, 0))))
        self.assertEqual(code, 1)
        self.assertIn("0/0 = 0.0%", out)


if __name__ == "__main__":
    sys.exit(unittest.main())
