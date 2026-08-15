from __future__ import annotations

import os
import platform
import tempfile
from pathlib import Path

from .deploy import validate_package


def default_name() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    architecture = "x86_64" if machine in {"amd64", "x86_64"} else machine
    if system == "windows":
        return f"calf-installer-windows-{architecture}"
    if system == "linux":
        return f"calf-installer-linux-{architecture}"
    raise RuntimeError(f"unsupported native installer platform: {system}/{machine}")


def pyinstaller_arguments(
    package: Path,
    output: Path,
    name: str,
    work: Path,
    *,
    repository_root: Path,
) -> list[str]:
    return [
        "--clean",
        "--noconfirm",
        "--onefile",
        "--console",
        "--name",
        name,
        "--distpath",
        str(output),
        "--workpath",
        str(work / "work"),
        "--specpath",
        str(work / "spec"),
        "--paths",
        str(repository_root / "src"),
        "--add-data",
        f"{package}{os.pathsep}calf_installer_payload",
        str(repository_root / "scripts" / "install"),
    ]


def build_native_installer(
    package: Path,
    output: Path,
    name: str,
    *,
    repository_root: Path,
) -> Path:
    try:
        import PyInstaller.__main__
    except ImportError as error:
        raise RuntimeError(
            "PyInstaller is required; install the project with .[installer]"
        ) from error
    package = package.resolve()
    validate_package(package)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="calf-native-installer-") as temp:
        PyInstaller.__main__.run(
            pyinstaller_arguments(
                package,
                output.resolve(),
                name,
                Path(temp),
                repository_root=repository_root,
            )
        )
    suffix = ".exe" if platform.system().lower() == "windows" else ""
    executable = output / f"{name}{suffix}"
    if not executable.is_file() or executable.is_symlink():
        raise RuntimeError(f"native installer was not created: {executable}")
    executable.chmod(executable.stat().st_mode | 0o111)
    return executable
