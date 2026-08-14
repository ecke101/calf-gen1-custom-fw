from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

from calf_fw_tool.build_output import prepare_output
from calf_fw_tool.cli import parser
from calf_fw_tool.firmware_targets import CALF_216_ARCHIVE_NAME
from calf_fw_tool.target_spec import package_name

ROOT = Path(__file__).resolve().parents[1]


def test_release_cli_defaults_to_calf_216() -> None:
    arguments = parser().parse_args(["build"])

    assert arguments.source == Path(CALF_216_ARCHIVE_NAME)


def test_package_name_uses_only_the_firmware_compatibility_version() -> None:
    assert package_name("2.1.6") == "calf-custom-fw-2.1.6.tar.gz"


def test_force_accepts_a_previous_project_build(tmp_path) -> None:
    manifest = tmp_path / "manifest.json"
    manifest.write_text(
        json.dumps({"project": "calf-gen1-custom-fw"}), encoding="utf-8"
    )

    prepare_output(tmp_path, force=True)


def test_release_tree_audit_passes() -> None:
    subprocess.run(
        [sys.executable, "tools/audit_release.py"],
        cwd=ROOT,
        check=True,
    )


def test_tracked_filenames_do_not_embed_project_versions() -> None:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    paths = [path for path in result.stdout.decode().split("\0") if path]
    versioned_name = re.compile(r"(?:^|[/_.-])v[0-9]+(?:$|[/_.-])", re.IGNORECASE)

    assert not [path for path in paths if versioned_name.search(path)]
