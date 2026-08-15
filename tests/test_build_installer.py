from __future__ import annotations

import os
from pathlib import Path

from calf_fw_tool.native_builder import default_name, pyinstaller_arguments

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


def test_default_name_supports_apple_silicon(monkeypatch) -> None:
    monkeypatch.setattr("platform.system", lambda: "Darwin")
    monkeypatch.setattr("platform.machine", lambda: "arm64")

    assert default_name() == "calf-installer-macos-arm64"


def test_default_name_supports_intel_macos(monkeypatch) -> None:
    monkeypatch.setattr("platform.system", lambda: "Darwin")
    monkeypatch.setattr("platform.machine", lambda: "x86_64")

    assert default_name() == "calf-installer-macos-x86_64"
