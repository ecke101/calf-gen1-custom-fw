from __future__ import annotations

import json
import subprocess
import tarfile

import pytest

from calf_fw_tool import deploy_common
from calf_fw_tool.deploy import (
    install_script,
    materialize_camera_tar,
    validate_package,
)
from calf_fw_tool.deploy_common import DeployError, camera_preflight
from calf_fw_tool.package_builder import _write_package
from calf_fw_tool.target_spec import CALF_216_TARGET
from calf_fw_tool.util import sha256


def _package(tmp_path, *, include_extra_binary: bool = False):
    root = tmp_path / "root"
    (root / "bin").mkdir(parents=True)
    binary = root / "bin/calf-ui"
    binary.write_bytes(b"calf-ui")
    helper = root / "bin/calf-sha256"
    helper.write_bytes(b"sha256-helper")
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
            },
            "calf-sha256": {
                "destination": "/app/bin/calf-sha256",
                "size": helper.stat().st_size,
                "sha256": sha256(helper),
            },
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

    assert set(manifest["payload"]) == {"calf-ui", "calf-sha256"}
    assert "version" not in manifest


def test_validator_rejects_unlisted_binary(tmp_path) -> None:
    package = _package(tmp_path, include_extra_binary=True)

    with pytest.raises(DeployError, match="do not match"):
        validate_package(package)


def test_remote_install_checks_size_hash_and_unique_temp_directory() -> None:
    script = install_script(
        "http://192.0.2.10:8000/calf-custom-fw.tar",
        123,
        "a" * 64,
        "b" * 32,
        "c" * 64,
    )

    subprocess.run(["/bin/sh", "-n"], input=script, text=True, check=True)
    assert "expected_size='123'" in script
    assert f"expected_sha256='{'a' * 64}'" in script
    assert f"expected_md5='{'b' * 32}'" in script
    assert f"expected_helper_sha256='{'c' * 64}'" in script
    assert "md5sum" in script
    assert "sha256sum" not in script
    assert "calf-sha256" in script
    assert 'install.sh" --preflight' in script
    assert 'tar -xf "$archive"' in script
    assert "tar -xzf" not in script
    assert "command -v curl" in script
    assert "command -v wget" in script
    assert 'curl -f "$url" -o "$archive"' in script
    assert 'wget -O "$archive" "$url"' in script
    assert "/usr/bin/wget" not in script
    assert "directory=/tmp/calf-custom-fw-install-$$" in script
    assert 'rm -rf -- "$directory"' in script
    assert "vpupdate.bin" not in script


def test_camera_tar_is_plain_and_retains_validated_members(tmp_path) -> None:
    package = _package(tmp_path)
    camera_tar = tmp_path / "camera.tar"

    materialize_camera_tar(package, camera_tar)

    assert camera_tar.read_bytes()[:2] != b"\x1f\x8b"
    with tarfile.open(camera_tar, "r:") as archive:
        assert "calf-custom-fw/bin/calf-sha256" in archive.getnames()


def test_camera_preflight_accepts_custom_backend_version_for_upgrade(
    monkeypatch,
) -> None:
    monkeypatch.setattr(
        deploy_common,
        "_get_json",
        lambda *_: {
            "code": 0,
            "body": {
                "product": "VR180 Camera",
                "version": "2.1.6 / ngcd-c-0.1+606078006ed7",
                "hardware": "4.0",
            },
        },
    )
    monkeypatch.setattr(
        deploy_common,
        "_post_json",
        lambda *_: {"code": 0, "body": {"rs": {"is_running": 0}}},
    )

    product = camera_preflight("192.0.2.1", timeout=1)

    assert product["version"] == "2.1.6"
    assert product["reported_version"] == (
        "2.1.6 / ngcd-c-0.1+606078006ed7"
    )


def test_camera_preflight_rejects_unrecognized_composite_version(
    monkeypatch,
) -> None:
    monkeypatch.setattr(
        deploy_common,
        "_get_json",
        lambda *_: {
            "code": 0,
            "body": {
                "version": "2.1.6 / unknown-backend",
                "hardware": "4.0",
            },
        },
    )

    with pytest.raises(DeployError, match="unsupported firmware"):
        camera_preflight("192.0.2.1", timeout=1)
