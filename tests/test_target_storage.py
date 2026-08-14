from __future__ import annotations

import stat
import subprocess
from pathlib import Path

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def atomic_writer(tmp_path_factory: pytest.TempPathFactory) -> Path:
    output = tmp_path_factory.mktemp("target-storage") / "atomic-writer"
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
            "ui/src/target_storage.c",
            "ui/tests/test_target_storage.c",
            "-o",
            str(output),
        ],
        cwd=REPOSITORY_ROOT,
        check=True,
    )
    return output


def test_atomic_writer_replaces_file_and_sets_mode(
    atomic_writer: Path, tmp_path: Path
) -> None:
    destination = tmp_path / "config"
    temporary = tmp_path / "config.tmp"
    destination.write_text("old\n", encoding="ascii")

    subprocess.run(
        [str(atomic_writer), str(destination), str(temporary)], check=True
    )

    assert destination.read_text(encoding="ascii") == "new config\n"
    assert stat.S_IMODE(destination.stat().st_mode) == 0o640
    assert not temporary.exists()


def test_atomic_writer_does_not_follow_temporary_symlink(
    atomic_writer: Path, tmp_path: Path
) -> None:
    destination = tmp_path / "config"
    temporary = tmp_path / "config.tmp"
    victim = tmp_path / "victim"
    victim.write_text("keep\n", encoding="ascii")
    temporary.symlink_to(victim)

    result = subprocess.run(
        [str(atomic_writer), str(destination), str(temporary)], check=False
    )

    assert result.returncode == 1
    assert victim.read_text(encoding="ascii") == "keep\n"


def test_atomic_writer_replaces_destination_symlink_not_target(
    atomic_writer: Path, tmp_path: Path
) -> None:
    destination = tmp_path / "config"
    temporary = tmp_path / "config.tmp"
    victim = tmp_path / "victim"
    victim.write_text("keep\n", encoding="ascii")
    destination.symlink_to(victim)

    subprocess.run(
        [str(atomic_writer), str(destination), str(temporary)], check=True
    )

    assert not destination.is_symlink()
    assert destination.read_text(encoding="ascii") == "new config\n"
    assert victim.read_text(encoding="ascii") == "keep\n"
