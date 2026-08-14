from __future__ import annotations

import hashlib
import io
import subprocess
import tarfile
from pathlib import Path

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def update_validator(tmp_path_factory: pytest.TempPathFactory) -> Path:
    output = tmp_path_factory.mktemp("target-update") / "validate-update"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Iui/include",
            "-Iui/src",
            "ui/src/sha256.c",
            "ui/src/target_update.c",
            "ui/tests/test_target_update.c",
            "-o",
            str(output),
        ],
        cwd=REPOSITORY_ROOT,
        check=True,
    )
    return output


def _write_archive(path: Path, names: tuple[str, ...] = ("app.img",)) -> None:
    payload = b"A" * (1024 * 1024)
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name in names:
            member = tarfile.TarInfo(name)
            member.size = len(payload)
            archive.addfile(member, io.BytesIO(payload))


def _write_identity(archive: Path, identity: Path, *, version: str = "2.2.1") -> None:
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    identity.write_text(
        f"CALF-VR180-GEN1 {version}\nSHA256 {digest}\n",
        encoding="ascii",
    )


def _validate(executable: Path, archive: Path, identity: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(executable), str(archive), str(identity)],
        text=True,
        capture_output=True,
        check=False,
    )


def test_update_validator_accepts_matching_app_archive(
    update_validator: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "vpupdate.bin"
    identity = tmp_path / "vpupdate.bin.sha256"
    _write_archive(archive)
    _write_identity(archive, identity)

    result = _validate(update_validator, archive, identity)

    assert result.returncode == 0
    assert hashlib.sha256(archive.read_bytes()).hexdigest() in result.stdout


def test_update_validator_rejects_same_size_replacement(
    update_validator: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "vpupdate.bin"
    identity = tmp_path / "vpupdate.bin.sha256"
    _write_archive(archive)
    _write_identity(archive, identity)
    contents = bytearray(archive.read_bytes())
    contents[512] ^= 1
    archive.write_bytes(contents)

    assert _validate(update_validator, archive, identity).returncode == 1


def test_update_validator_rejects_duplicate_app_member(
    update_validator: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "vpupdate.bin"
    identity = tmp_path / "vpupdate.bin.sha256"
    _write_archive(archive, ("app.img", "app.img"))
    _write_identity(archive, identity)

    assert _validate(update_validator, archive, identity).returncode == 1


def test_update_validator_rejects_wrong_camera_version_identity(
    update_validator: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "vpupdate.bin"
    identity = tmp_path / "vpupdate.bin.sha256"
    _write_archive(archive)
    _write_identity(archive, identity, version="9.9.9")

    assert _validate(update_validator, archive, identity).returncode == 1


def test_update_validator_rejects_nonzero_data_after_tar_end(
    update_validator: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "vpupdate.bin"
    identity = tmp_path / "vpupdate.bin.sha256"
    _write_archive(archive)
    with archive.open("ab") as stream:
        stream.write(b"unexpected")
    _write_identity(archive, identity)

    assert _validate(update_validator, archive, identity).returncode == 1
