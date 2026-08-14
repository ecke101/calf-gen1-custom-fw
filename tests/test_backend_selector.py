from __future__ import annotations

import subprocess

import pytest

from calf_fw_tool.backend_selector import (
    NEW_BACKEND_FILESYSTEM_PATH,
    NEW_UI_FILESYSTEM_PATH,
    STOCK_BACKEND_FILESYSTEM_PATH,
    build_backend_selector,
)
from calf_fw_tool.ui_selector import STOCK_UI_SESSION_MARKER
from calf_fw_tool.util import FirmwareToolError, sha256


def test_selector_prefers_calf_backend_and_shares_stock_marker(tmp_path) -> None:
    selector = tmp_path / "ngcd-selector"
    manifest = build_backend_selector(selector)
    text = selector.read_text(encoding="ascii")

    assert selector.stat().st_mode & 0o111
    assert f"new_backend='/app{NEW_BACKEND_FILESYSTEM_PATH}'" in text
    assert f"stock_backend='/app{STOCK_BACKEND_FILESYSTEM_PATH}'" in text
    assert f"new_ui='/app{NEW_UI_FILESYSTEM_PATH}'" in text
    assert "capture_server='/app/bin/calf-capture-server'" in text
    assert '"$capture_server" start' in text
    assert f"marker='{STOCK_UI_SESSION_MARKER}'" in text
    assert 'exec "$new_backend" "$@"' in text
    assert 'exec "$stock_backend" "$@"' in text
    assert '"$new_backend" "$@" &' not in text
    assert 'select_stock\nexit 127' in text
    assert manifest["sha256"] == sha256(selector)
    subprocess.run(["/bin/sh", "-n", str(selector)], check=True)


def test_selector_rejects_shell_metacharacters(tmp_path) -> None:
    with pytest.raises(FirmwareToolError):
        build_backend_selector(
            tmp_path / "selector", new_backend_path="/bin/ngcd;reboot"
        )


def test_selector_accepts_reboot_reversible_tmp_targets(tmp_path) -> None:
    selector = tmp_path / "selector"
    build_backend_selector(
        selector,
        new_backend_path="/tmp/calf-ngcd-test",
        stock_backend_path="/tmp/ngcd-stock-test",
        new_ui_path="/tmp/calf-ui-test",
    )

    text = selector.read_text(encoding="ascii")
    assert "new_backend='/tmp/calf-ngcd-test'" in text
    assert "stock_backend='/tmp/ngcd-stock-test'" in text
    assert "new_ui='/tmp/calf-ui-test'" in text
