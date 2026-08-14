from __future__ import annotations

import json
import subprocess

import pytest

from calf_fw_tool.deploy import install_script, validate_package
from calf_fw_tool.deploy_common import DeployError
from calf_fw_tool.package_builder import _write_package
from calf_fw_tool.target_spec import CALF_216_TARGET
from calf_fw_tool.util import sha256


def _package(tmp_path, *, include_extra_binary: bool = False):
    root = tmp_path / "root"
    (root / "bin").mkdir(parents=True)
    binary = root / "bin/calf-ui"
    binary.write_bytes(b"calf-ui")
    if include_extra_binary:
        (root / "bin/vendor-stock").write_bytes(b"not allowed")
    (root / "install.sh").write_text("#!/bin/sh\n", encoding="ascii")
    manifest = {
        "profile": "calf-custom-fw",
        "target_firmware": "2.1.6",
        "target_archive_sha256": (
            CALF_216_TARGET.firmware.source_archive_sha256
        ),
        "licenses": [],
        "payload": {
            "calf-ui": {
                "destination": "/app/bin/calf-ui",
                "size": binary.stat().st_size,
                "sha256": sha256(binary),
            }
        },
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest), encoding="ascii"
    )
    package = tmp_path / "package.tar.gz"
    _write_package(root, package)
    return package


def test_validator_accepts_manifest_matched_incremental_package(tmp_path) -> None:
    package = _package(tmp_path)

    manifest = validate_package(package)

    assert set(manifest["payload"]) == {"calf-ui"}
    assert "version" not in manifest


def test_validator_rejects_unlisted_binary(tmp_path) -> None:
    package = _package(tmp_path, include_extra_binary=True)

    with pytest.raises(DeployError, match="do not match"):
        validate_package(package)


def test_remote_install_checks_size_hash_and_unique_temp_directory() -> None:
    script = install_script("http://192.0.2.10:8000/calf-custom-fw.tar.gz", 123, "a" * 64)

    subprocess.run(["/bin/sh", "-n"], input=script, text=True, check=True)
    assert "expected_size='123'" in script
    assert f"expected_sha256='{'a' * 64}'" in script
    assert "sha256sum" in script
    assert "directory=/tmp/calf-custom-fw-install-$$" in script
    assert 'rm -rf -- "$directory"' in script
    assert "vpupdate.bin" not in script
