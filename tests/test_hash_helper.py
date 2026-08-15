from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from calf_fw_tool.hash_helper import build_hash_helper

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(
    shutil.which("clang") is None or shutil.which("readelf") is None,
    reason="cross-compiler inspection tools are unavailable",
)
def test_hash_helper_is_freestanding_aarch64(tmp_path) -> None:
    output = tmp_path / "calf-sha256"

    result = build_hash_helper(output, ui_source=ROOT / "ui")

    header = subprocess.run(
        ["readelf", "-h", str(output)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    dynamic = subprocess.run(
        ["readelf", "-d", str(output)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout
    assert "AArch64" in header
    assert "There is no dynamic section" in dynamic
    assert result["runtime_dependencies"] == "none"
