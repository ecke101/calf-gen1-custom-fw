from __future__ import annotations

import json
import os
from pathlib import Path

from .util import FirmwareToolError


def prepare_output(output: Path, force: bool) -> None:
    absolute = Path(os.path.abspath(output))
    if absolute == Path(absolute.anchor):
        raise FirmwareToolError(f"refusing to use a filesystem root: {output}")
    if output.is_symlink():
        raise FirmwareToolError(
            f"output directory must not be a symlink: {output}"
        )
    if output.exists() and not output.is_dir():
        raise FirmwareToolError(f"output path is not a directory: {output}")
    if not output.exists():
        output.mkdir(parents=True)
        return

    entries = list(output.iterdir())
    if entries and not force:
        raise FirmwareToolError(
            f"output directory is not empty: {output}\n"
            "Choose another directory or pass --force."
        )
    if entries and force:
        manifest_path = output / "manifest.json"
        if manifest_path.is_symlink() or not manifest_path.is_file():
            raise FirmwareToolError(
                "--force only accepts a previous CALF build directory "
                f"containing manifest.json: {output}"
            )
        try:
            previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise FirmwareToolError(
                f"invalid previous build manifest: {manifest_path}"
            ) from error
        if previous.get("project") != "calf-gen1-custom-fw":
            raise FirmwareToolError(
                f"not a CALF firmware build directory: {output}"
            )
