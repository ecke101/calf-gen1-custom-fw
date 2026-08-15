from __future__ import annotations

import pytest

from calf_fw_tool import nginx_config
from calf_fw_tool.nginx_config import patch_download_location
from calf_fw_tool.util import FirmwareToolError, sha256


def _stock_config() -> bytes:
    return b"""http {
    server {
        location /download {
            alias    /media/DCIM/;
        #    autoindex on;
            autoindex_exact_size off;
            autoindex_localtime on;
        }
    }
}
"""


def test_download_location_serves_active_sd_root(tmp_path, monkeypatch) -> None:
    source = tmp_path / "nginx.conf"
    destination = tmp_path / "nginx.patched.conf"
    source.write_bytes(_stock_config())
    monkeypatch.setattr(nginx_config, "NGINX_CONFIG_SHA256", sha256(source))

    result = patch_download_location(source, destination)

    patched = destination.read_text(encoding="ascii")
    assert "alias    /mnt/mmcblk1p1/;" in patched
    assert "            autoindex on;" in patched
    assert "/media/DCIM" not in patched
    assert result["stock_sha256"] == sha256(source)
    assert result["installed_sha256"] == sha256(destination)


def test_download_location_rejects_unknown_config(tmp_path) -> None:
    source = tmp_path / "nginx.conf"
    destination = tmp_path / "nginx.patched.conf"
    source.write_bytes(_stock_config())

    with pytest.raises(FirmwareToolError, match="hash mismatch"):
        patch_download_location(source, destination)
