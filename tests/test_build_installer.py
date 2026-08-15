from __future__ import annotations

import os
from pathlib import Path

from calf_fw_tool.native_builder import pyinstaller_arguments

ROOT = Path(__file__).resolve().parents[1]


def test_native_builder_embeds_payload_and_uses_one_file_mode(tmp_path) -> None:
    package = tmp_path / "calf-custom-fw.tar.gz"
    output = tmp_path / "dist"
    work = tmp_path / "work"

    arguments = pyinstaller_arguments(
        package,
        output,
        "calf-installer",
        work,
        repository_root=ROOT,
    )

    assert "--onefile" in arguments
    assert "--console" in arguments
    assert (
        f"{package}{os.pathsep}calf_installer_payload" in arguments
    )
    assert arguments[-1].endswith("scripts/install")
