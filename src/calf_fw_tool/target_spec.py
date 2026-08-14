from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .firmware_targets import (
    CALF_216_ARCHIVE_NAME,
    CALF_216_FIRMWARE,
    VIEWPT_221_ARCHIVE_NAME,
    VIEWPT_221_FIRMWARE,
)
from .model import FirmwareIdentity
from .util import FirmwareToolError, sha256


@dataclass(frozen=True)
class FirmwareTarget:
    firmware: FirmwareIdentity
    archive_name: str
    accepted_ngcd_sha256: tuple[str, ...]
    accepted_ngui_sha256: tuple[str, ...]


CALF_216_TARGET = FirmwareTarget(
    firmware=CALF_216_FIRMWARE,
    archive_name=CALF_216_ARCHIVE_NAME,
    accepted_ngcd_sha256=(
        CALF_216_FIRMWARE.ngcd_sha256,
        "02fa7db5443d14de052f3a4dae2841739bb3a464a418b7d778d2720a128620fd",
    ),
    accepted_ngui_sha256=(
        CALF_216_FIRMWARE.ngui_sha256,
        "7b4a1cf1b2a4806fc6d9b41d0b6f647fab83a864b19ed97127421e1c704a9530",
    ),
)

VIEWPT_221_TARGET = FirmwareTarget(
    firmware=VIEWPT_221_FIRMWARE,
    archive_name=VIEWPT_221_ARCHIVE_NAME,
    accepted_ngcd_sha256=(
        VIEWPT_221_FIRMWARE.ngcd_sha256,
        "34a5a894d7adeef85d14778545597fcf92206597db5b93e8640621250b19bdf7",
    ),
    accepted_ngui_sha256=(
        VIEWPT_221_FIRMWARE.ngui_sha256,
        "1fcea474dd8acd79891c15ec209b6db5bfb2260f57e8fc10494143d3032a1283",
    ),
)

TARGETS = (CALF_216_TARGET, VIEWPT_221_TARGET)
DEFAULT_TARGET = CALF_216_TARGET
TARGETS_BY_VERSION = {
    target.firmware.version: target for target in TARGETS
}


def resolve_target(source: Path) -> FirmwareTarget:
    if not source.is_file() or source.is_symlink():
        raise FirmwareToolError(f"official archive is missing or unsafe: {source}")
    digest = sha256(source)
    for target in TARGETS:
        if digest == target.firmware.source_archive_sha256:
            return target
    versions = ", ".join(target.firmware.version for target in TARGETS)
    raise FirmwareToolError(
        f"official archive has unsupported SHA-256 {digest}; expected CALF "
        f"firmware target {versions}"
    )


def package_name(version: str) -> str:
    return f"calf-custom-fw-{version}.tar.gz"
