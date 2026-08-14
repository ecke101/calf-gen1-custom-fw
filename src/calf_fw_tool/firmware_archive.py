from __future__ import annotations

import tarfile
from pathlib import Path

from .util import FirmwareToolError

OUTER_MEMBERS = frozenset({"vpupdate.bin", "version.txt"})
INNER_MEMBERS = frozenset(
    {
        "uboot.img",
        "recovery.img",
        "boot.img",
        "resource.img",
        "rootfs.img",
        "app.img",
        "local.img",
    }
)


def _validated_members(
    archive: tarfile.TarFile,
    *,
    expected: frozenset[str],
    label: str,
) -> dict[str, tarfile.TarInfo]:
    members = archive.getmembers()
    by_name = {member.name: member for member in members}
    if set(by_name) != expected or len(members) != len(expected):
        raise FirmwareToolError(
            f"{label} contains unexpected members: expected {sorted(expected)}, "
            f"found {sorted(by_name)}"
        )
    for member in members:
        if not member.isfile():
            raise FirmwareToolError(
                f"{label} member is not a regular file: {member.name}"
            )
    return by_name


def extract_official_outer(source: Path, destination: Path) -> tuple[Path, Path]:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(source, "r:gz") as archive:
        members = _validated_members(
            archive, expected=OUTER_MEMBERS, label="official outer archive"
        )
        outputs: dict[str, Path] = {}
        for name in sorted(OUTER_MEMBERS):
            stream = archive.extractfile(members[name])
            if stream is None:
                raise FirmwareToolError(
                    f"cannot read {name} from official archive"
                )
            output = destination / name
            with output.open("wb") as target:
                while chunk := stream.read(1024 * 1024):
                    target.write(chunk)
            outputs[name] = output
    return outputs["vpupdate.bin"], outputs["version.txt"]


def extract_inner_image(
    vpupdate: Path, destination: Path, member_name: str
) -> Path:
    if member_name not in INNER_MEMBERS:
        raise FirmwareToolError(f"unknown firmware image: {member_name}")
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(vpupdate, "r:") as archive:
        members = _validated_members(
            archive, expected=INNER_MEMBERS, label="official vpupdate.bin"
        )
        stream = archive.extractfile(members[member_name])
        if stream is None:
            raise FirmwareToolError(
                f"cannot read {member_name} from vpupdate.bin"
            )
        output = destination / member_name
        with output.open("wb") as target:
            while chunk := stream.read(1024 * 1024):
                target.write(chunk)
    return output


def extract_app_image(vpupdate: Path, destination: Path) -> Path:
    return extract_inner_image(vpupdate, destination, "app.img")
