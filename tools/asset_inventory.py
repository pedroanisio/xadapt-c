"""Enumerate source assets consistently in a checkout or an exported release."""

import json
from pathlib import Path
import subprocess


def files(root: Path) -> list[str]:
    manifest = root / "tools/release_assets.json"
    if manifest.exists():
        return sorted(json.loads(manifest.read_text())["source"])
    result = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True)
    if result.returncode == 0:
        return sorted(
            p for p in result.stdout.decode().split("\0") if p and (root / p).is_file()
        )
    return sorted(
        json.loads((root / "tools/release_assets.json").read_text())["source"]
    )
