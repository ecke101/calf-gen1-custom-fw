from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

from .firmware_targets import CALF_216_ARCHIVE_NAME
from .package_builder import build_package
from .util import FirmwareToolError


def parser() -> argparse.ArgumentParser:
    command = argparse.ArgumentParser(
        prog="calf-fw",
        description=(
            "Build the incremental CALF GEN1 custom firmware package without "
            "bundling camera-vendor firmware files."
        ),
    )
    subparsers = command.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build", help="build a firmware package")
    build.add_argument(
        "source",
        nargs="?",
        type=Path,
        default=Path(CALF_216_ARCHIVE_NAME),
        help=(
            "verified local official archive used only as a link-time input "
            f"(default: {CALF_216_ARCHIVE_NAME})"
        ),
    )
    build.add_argument("--output", type=Path, default=Path("build/release"))
    build.add_argument("--ui-source", type=Path, default=Path("ui"))
    build.add_argument("--ngcd-source", type=Path, default=Path("ngcd"))
    build.add_argument("--force", action="store_true")
    return command


def main(argv: Sequence[str] | None = None) -> None:
    args = parser().parse_args(argv)
    try:
        result = build_package(
            args.source.resolve(),
            args.output.absolute(),
            ui_source=args.ui_source.resolve(),
            ngcd_source=args.ngcd_source.resolve(),
            repository_root=Path(__file__).resolve().parents[2],
            force=args.force,
        )
    except FirmwareToolError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
