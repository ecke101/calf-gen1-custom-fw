#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_TOP_LEVEL = {
    ".clang-tidy",
    ".gitattributes",
    ".gitignore",
    "LICENSE",
    "LICENSES",
    "Makefile",
    "NOTICE",
    "PROVENANCE.md",
    "README.md",
    "REUSE.toml",
    "THIRD_PARTY_LICENSES.md",
    "docs",
    "ngcd",
    "pyproject.toml",
    "scripts",
    "src",
    "tests",
    "tools",
    "ui",
}
ALLOWED_BINARY_ASSETS = {
    "ui/assets/fonts/NotoSans-Regular.ttf",
    "ui/assets/fonts/NotoSansSymbols-Regular.ttf",
    "ui/assets/fonts/NotoSansSymbols2-Regular.ttf",
}
FORBIDDEN_SUFFIXES = {
    ".a",
    ".bin",
    ".bmp",
    ".dng",
    ".elf",
    ".gz",
    ".img",
    ".jpeg",
    ".jpg",
    ".ko",
    ".mp4",
    ".o",
    ".png",
    ".raw",
    ".so",
    ".tar",
    ".tgz",
    ".wav",
    ".zip",
}
FORBIDDEN_NAMES = {
    "app.img",
    "boot.img",
    "local.img",
    "recovery.img",
    "resource.img",
    "rootfs.img",
    "uboot.img",
    "vpupdate.bin",
}
REQUIRED_LICENSES = {
    "LICENSE",
    "LICENSES/Apache-2.0.txt",
    "LICENSES/MIT.txt",
    "LICENSES/OFL-1.1.txt",
    "NOTICE",
    "PROVENANCE.md",
    "REUSE.toml",
    "THIRD_PARTY_LICENSES.md",
    "ngcd/vendor/libxaac/LICENSE",
    "ngcd/vendor/libxaac/NOTICE",
    "ngcd/vendor/libxaac/ORIGIN.md",
    "ngcd/vendor/vo-aacenc/COPYING",
    "ngcd/vendor/vo-aacenc/NOTICE",
    "ngcd/vendor/vo-aacenc/README.calf",
    "ui/assets/fonts/OFL.txt",
    "ui/assets/fonts/README.md",
    "ui/third_party/README.md",
    "ui/third_party/stb/LICENSE",
}


def git_output(*arguments: str) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def tracked_paths() -> set[str]:
    if (ROOT / ".git").is_dir():
        return {
            value.decode()
            for value in git_output("ls-files", "-z").split(b"\0")
            if value
        }
    return {
        path.relative_to(ROOT).as_posix()
        for path in ROOT.rglob("*")
        if path.is_file()
        and not any(
            part
            in {
                ".git",
                ".pytest_cache",
                ".ruff_cache",
                ".venv",
                "__pycache__",
                "build",
            }
            or part.endswith(".egg-info")
            for part in path.relative_to(ROOT).parts
        )
    }


def history_paths() -> set[str]:
    if not (ROOT / ".git").is_dir():
        return set()
    output = git_output("rev-list", "--objects", "--all").decode(
        errors="replace"
    )
    return {
        path
        for line in output.splitlines()
        if " " in line
        for path in (line.split(" ", 1)[1],)
        if path
    }


def path_errors(path_text: str) -> list[str]:
    path = PurePosixPath(path_text)
    errors: list[str] = []
    if not path.parts or path.parts[0] not in ALLOWED_TOP_LEVEL:
        errors.append(f"unapproved top-level path: {path_text}")
    if path.name in FORBIDDEN_NAMES:
        errors.append(f"forbidden firmware member: {path_text}")
    suffixes = set(path.suffixes)
    if suffixes & FORBIDDEN_SUFFIXES:
        errors.append(f"forbidden release-file suffix: {path_text}")
    if path_text.startswith(("build/", "recovery/")):
        errors.append(f"forbidden generated/recovery path: {path_text}")
    return errors


def main() -> None:
    tracked = tracked_paths()
    errors: list[str] = []
    for path in sorted(tracked | history_paths()):
        errors.extend(path_errors(path))
    missing = REQUIRED_LICENSES - tracked
    errors.extend(f"missing required license: {path}" for path in sorted(missing))

    for relative in sorted(tracked):
        path = ROOT / relative
        if not path.is_file() or path.is_symlink():
            errors.append(f"tracked path is missing, non-regular, or linked: {relative}")
            continue
        header = path.read_bytes()[:4096]
        if relative not in ALLOWED_BINARY_ASSETS:
            if header.startswith((b"\x7fELF", b"\x1f\x8b")) or b"\0" in header:
                errors.append(f"unapproved binary content: {relative}")

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
    print(
        f"release audit passed: {len(tracked)} tracked files, "
        f"{len(history_paths())} historical paths"
    )


if __name__ == "__main__":
    main()
