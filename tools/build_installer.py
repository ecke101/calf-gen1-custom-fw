#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]

from calf_fw_tool.native_builder import (  # noqa: E402
    build_native_installer,
    default_name,
)


def parser() -> argparse.ArgumentParser:
    command = argparse.ArgumentParser(
        description="Build the native CALF installer for the current host OS."
    )
    command.add_argument("package", type=Path)
    command.add_argument("--output", type=Path, default=Path("build/installers"))
    command.add_argument("--name", default=default_name())
    return command


def main(argv: Sequence[str] | None = None) -> None:
    args = parser().parse_args(argv)
    if not args.package.is_file() or args.package.is_symlink():
        raise SystemExit(f"package is missing or unsafe: {args.package}")
    if not args.name or any(character in args.name for character in "/\\"):
        raise SystemExit("installer name must be a plain filename")
    try:
        executable = build_native_installer(
            args.package,
            args.output,
            args.name,
            repository_root=ROOT,
        )
    except RuntimeError as error:
        raise SystemExit(str(error)) from error
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    sidecar = executable.with_name(f"{executable.name}.sha256")
    sidecar.write_text(f"{digest}  {executable.name}\n", encoding="ascii")
    print(executable)
    print(sidecar)


if __name__ == "__main__":
    main()
