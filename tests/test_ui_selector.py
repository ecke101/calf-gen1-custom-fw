from __future__ import annotations

import subprocess

import pytest

from calf_fw_tool.ui_selector import (
    NEW_BACKEND_FILESYSTEM_PATH,
    NEW_UI_FILESYSTEM_PATH,
    STOCK_UI_FILESYSTEM_PATH,
    STOCK_UI_SESSION_MARKER,
    build_ui_selector,
)
from calf_fw_tool.util import FirmwareToolError, sha256


def test_selector_prefers_new_ui_and_has_stock_session_fallback(tmp_path) -> None:
    selector = tmp_path / "ngui-selector"
    manifest = build_ui_selector(selector)
    text = selector.read_text(encoding="ascii")

    assert selector.stat().st_mode & 0o111
    assert f"new_ui='/app{NEW_UI_FILESYSTEM_PATH}'" in text
    assert f"stock_ui='/app{STOCK_UI_FILESYSTEM_PATH}'" in text
    assert f"new_backend='/app{NEW_BACKEND_FILESYSTEM_PATH}'" in text
    assert f"marker='{STOCK_UI_SESSION_MARKER}'" in text
    assert 'exec "$new_ui" "$@"' in text
    assert '"$new_ui" "$@" &' not in text
    assert 'wait "$child"' not in text
    assert 'select_stock\nexit 127' in text
    assert manifest["sha256"] == sha256(selector)
    subprocess.run(["/bin/sh", "-n", str(selector)], check=True)


def test_selector_rejects_shell_metacharacters(tmp_path) -> None:
    with pytest.raises(FirmwareToolError):
        build_ui_selector(tmp_path / "selector", new_ui_path="/bin/ui;reboot")


def test_selector_accepts_reboot_reversible_tmp_targets(tmp_path) -> None:
    selector = tmp_path / "selector"
    build_ui_selector(
        selector,
        new_ui_path="/tmp/calf-ui-test",
        stock_ui_path="/tmp/ngui-stock-test",
        new_backend_path="/tmp/calf-ngcd-test",
    )

    text = selector.read_text(encoding="ascii")
    assert "new_ui='/tmp/calf-ui-test'" in text
    assert "stock_ui='/tmp/ngui-stock-test'" in text
    assert "new_backend='/tmp/calf-ngcd-test'" in text
