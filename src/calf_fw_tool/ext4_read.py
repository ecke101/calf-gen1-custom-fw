from __future__ import annotations

from pathlib import Path

from .util import FirmwareToolError, run


def dump_file(image: Path, filesystem_path: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    result = run(
        ["debugfs", "-R", f"dump {filesystem_path} {destination}", str(image)],
        check=False,
    )
    if result.returncode != 0 or not destination.is_file():
        raise FirmwareToolError(
            f"failed to extract {filesystem_path} from {image}:\n"
            f"{result.stdout}{result.stderr}".rstrip()
        )
