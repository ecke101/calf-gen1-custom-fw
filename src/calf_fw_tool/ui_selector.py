from __future__ import annotations

import re
from pathlib import Path

from .util import FirmwareToolError, sha256

UI_SELECTOR_FILESYSTEM_PATH = "/bin/ngui"
NEW_UI_FILESYSTEM_PATH = "/bin/calf-ui"
STOCK_UI_FILESYSTEM_PATH = "/bin/ngui-stock"
NEW_BACKEND_FILESYSTEM_PATH = "/bin/calf-ngcd"
STOCK_UI_SESSION_MARKER = "/tmp/calf-ui-stock-session"


def _runtime_path(filesystem_path: str) -> str:
    if re.fullmatch(r"/bin/[A-Za-z0-9._-]+", filesystem_path):
        return f"/app{filesystem_path}"
    if re.fullmatch(r"/tmp/[A-Za-z0-9._-]+", filesystem_path):
        return filesystem_path
    raise FirmwareToolError(f"unsafe UI filesystem path: {filesystem_path!r}")


def build_ui_selector(
    destination: Path,
    *,
    new_ui_path: str = NEW_UI_FILESYSTEM_PATH,
    stock_ui_path: str = STOCK_UI_FILESYSTEM_PATH,
    new_backend_path: str = NEW_BACKEND_FILESYSTEM_PATH,
) -> dict[str, object]:
    """Build the supervised UI dispatcher installed at /bin/ngui."""

    new_ui = _runtime_path(new_ui_path)
    stock_ui = _runtime_path(stock_ui_path)
    new_backend = _runtime_path(new_backend_path)
    script = f"""#!/bin/sh
marker='{STOCK_UI_SESSION_MARKER}'
new_ui='{new_ui}'
stock_ui='{stock_ui}'
new_backend='{new_backend}'

select_stock()
{{
    umask 077
    set -C
    : > "$marker" 2>/dev/null || test -e "$marker"
}}

start_stock()
{{
    exec "$stock_ui" "$@"
    exit 127
}}

if test -e "$marker"; then
    start_stock "$@"
fi

if test ! -x "$new_ui" || test ! -x "$new_backend" ||
   ! /lib/ld-linux-aarch64.so.1 --verify "$new_ui" >/dev/null 2>&1 ||
   ! /lib/ld-linux-aarch64.so.1 --verify "$new_backend" >/dev/null 2>&1; then
    select_stock || exit 127
    start_stock "$@"
fi

# Replace the selector process rather than keeping a shell parent. ngmonitor
# can kill and restart this exact PID without leaving an orphan replacement UI.
# The replacement creates the marker before an unexpected or requested exit.
exec "$new_ui" "$@"
select_stock
exit 127
"""
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(script, encoding="ascii")
    destination.chmod(0o755)
    return {
        "target": UI_SELECTOR_FILESYSTEM_PATH,
        "sha256": sha256(destination),
        "new_ui": new_ui,
        "new_backend": new_backend,
        "stock_ui": stock_ui,
        "stock_session_marker": STOCK_UI_SESSION_MARKER,
        "behavior": (
            "replacement UI by default; stock UI for the remainder of the boot "
            "after the replacement records an explicit or unexpected exit"
        ),
    }
