from __future__ import annotations

import hashlib
import shutil
import subprocess
from pathlib import Path
from typing import Iterable


class FirmwareToolError(RuntimeError):
    """A validation or build failure that should be shown without a traceback."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_hash(path: Path, expected: str, label: str) -> str:
    actual = sha256(path)
    if actual != expected:
        raise FirmwareToolError(
            f"{label} hash mismatch:\n"
            f"  expected {expected}\n"
            f"  actual   {actual}\n"
            f"  file     {path}"
        )
    return actual


def require_commands(names: Iterable[str]) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise FirmwareToolError(
            "Required command(s) not found: " + ", ".join(sorted(missing))
        )


def run(
    args: list[str],
    *,
    check: bool = True,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        args,
        check=False,
        text=True,
        capture_output=capture_output,
    )
    if check and result.returncode != 0:
        output = "\n".join(part for part in (result.stdout, result.stderr) if part)
        raise FirmwareToolError(
            f"Command failed with exit code {result.returncode}: "
            f"{' '.join(args)}\n{output}".rstrip()
        )
    return result
