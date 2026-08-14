from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BytePatch:
    name: str
    offset: int
    before: bytes
    after: bytes
    description: str

    def __post_init__(self) -> None:
        if self.offset < 0:
            raise ValueError(f"{self.name}: offset cannot be negative")
        if not self.before:
            raise ValueError(f"{self.name}: before sequence cannot be empty")
        if len(self.before) != len(self.after):
            raise ValueError(f"{self.name}: replacement must preserve length")


@dataclass(frozen=True)
class BinaryPatchSet:
    target: str
    source_sha256: str
    patches: tuple[BytePatch, ...]


@dataclass(frozen=True)
class FirmwareIdentity:
    version: str
    source_archive_sha256: str
    vpupdate_sha256: str
    app_image_sha256: str
    ngcd_sha256: str
    ngui_sha256: str
