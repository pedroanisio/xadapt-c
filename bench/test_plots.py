"""Optional Matplotlib integration test; run after installing bench requirements.

Source: OpenAI GPT-6 via Codex. The codec's default build/test gate remains
independent of the optional plotting dependencies.
"""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import plots
import telemetry

# Matplotlib is in bench/requirements.txt and not in the codec's own gate, so
# a checkout without it skips this rather than failing: an absent optional
# dependency is not a broken plotter.
NO_MATPLOTLIB = importlib.util.find_spec("matplotlib") is None


@unittest.skipIf(NO_MATPLOTLIB, "install bench/requirements.txt for the plotting tests")
class PlotTest(unittest.TestCase):
    def test_all_adaptive_and_unknown_profiles_render_without_dropping_runs(self):
        profiles = (
            "rates",
            "sparse",
            "retained",
            "auto",
            "normal-bmp",
            "rates-bmp",
            "sparse-bmp",
            "retained-bmp",
            "future",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, profile in enumerate(profiles):
                directory = root / str(index)
                directory.mkdir()
                record = {
                    "schema": telemetry.SCHEMA,
                    "run_id": str(index),
                    "started_at": "2026-01-01",
                    "status": "ok" if index < 8 else "failed",
                    "config": {
                        "profile": profile,
                        "repeat": 1,
                        "memory_bytes": 64 << 20,
                        "threads": 1,
                        "verify": False,
                    },
                    "fixture": {
                        "name": "render-test",
                        "sha256": "0" * 64,
                        "bytes": 128,
                        "is_prefix": False,
                    },
                    "binary": {"sha256": "1" * 64},
                    "metrics": {"output_bytes": 64, "elapsed_seconds": 1},
                }
                telemetry.atomic_json(directory / "run.json", record)
            with patch.object(plots, "SAMPLE_FIELDS", ["elapsed_seconds", "rss_bytes"]):
                artifacts = plots.plot_runs([root], root / "plots")
            self.assertEqual(len(artifacts), 4)
            self.assertTrue(all(path.stat().st_size > 100 for path in artifacts))
            manifest = json.loads((root / "plots/plots.json").read_text())
            self.assertEqual(len(manifest["run_ids"]), len(profiles))
            index = (root / "plots/index.html").read_text()
            for profile in profiles:
                self.assertIn(profile, index)
            self.assertIn("1 failed or unfinished", index)


if __name__ == "__main__":
    unittest.main()
