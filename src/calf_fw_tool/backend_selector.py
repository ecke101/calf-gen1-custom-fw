from __future__ import annotations

import re
from pathlib import Path

from .ui_selector import STOCK_UI_SESSION_MARKER
from .util import FirmwareToolError, sha256

BACKEND_SELECTOR_FILESYSTEM_PATH = "/bin/ngcd"
NEW_BACKEND_FILESYSTEM_PATH = "/bin/calf-ngcd"
STOCK_BACKEND_FILESYSTEM_PATH = "/bin/ngcd-stock"
NEW_UI_FILESYSTEM_PATH = "/bin/calf-ui"
CAPTURE_SERVER_FILESYSTEM_PATH = "/bin/calf-capture-server"


def _runtime_path(filesystem_path: str) -> str:
    if re.fullmatch(r"/bin/[A-Za-z0-9._-]+", filesystem_path):
        return f"/app{filesystem_path}"
    if re.fullmatch(r"/tmp/[A-Za-z0-9._-]+", filesystem_path):
        return filesystem_path
    raise FirmwareToolError(
        f"unsafe backend filesystem path: {filesystem_path!r}"
    )


def build_backend_selector(
    destination: Path,
    *,
    new_backend_path: str = NEW_BACKEND_FILESYSTEM_PATH,
    stock_backend_path: str = STOCK_BACKEND_FILESYSTEM_PATH,
    new_ui_path: str = NEW_UI_FILESYSTEM_PATH,
    capture_server_path: str = CAPTURE_SERVER_FILESYSTEM_PATH,
) -> dict[str, object]:
    """Build the supervised backend dispatcher installed at /bin/ngcd."""

    new_backend = _runtime_path(new_backend_path)
    stock_backend = _runtime_path(stock_backend_path)
    new_ui = _runtime_path(new_ui_path)
    capture_server = _runtime_path(capture_server_path)
    script = f"""#!/bin/sh
marker='{STOCK_UI_SESSION_MARKER}'
new_backend='{new_backend}'
stock_backend='{stock_backend}'
new_ui='{new_ui}'
capture_server='{capture_server}'

select_stock()
{{
    umask 077
    set -C
    : > "$marker" 2>/dev/null || test -e "$marker"
}}

start_stock()
{{
    exec "$stock_backend" "$@"
    exit 127
}}

if test -e "$marker"; then
    start_stock "$@"
fi

if test ! -x "$new_backend" || test ! -x "$new_ui" ||
   test ! -x "$capture_server" ||
   ! /lib/ld-linux-aarch64.so.1 --verify "$new_backend" >/dev/null 2>&1 ||
   ! /lib/ld-linux-aarch64.so.1 --verify "$new_ui" >/dev/null 2>&1; then
    select_stock || exit 127
    start_stock "$@"
fi

if ! "$capture_server" start; then
    select_stock || exit 127
    start_stock "$@"
fi

# Keep the PID visible to ngmonitor equal to the selected backend PID. CALF
# ngcd records the shared marker before an unexpected exit; ngmonitor then
# restarts this selector and the UI selector as one matched pair.
exec "$new_backend" "$@"
select_stock
exit 127
"""
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(script, encoding="ascii")
    destination.chmod(0o755)
    return {
        "target": BACKEND_SELECTOR_FILESYSTEM_PATH,
        "sha256": sha256(destination),
        "new_backend": new_backend,
        "new_ui": new_ui,
        "capture_server": capture_server,
        "stock_backend": stock_backend,
        "stock_session_marker": STOCK_UI_SESSION_MARKER,
        "behavior": (
            "replacement backend by default; stock backend for the remainder "
            "of the boot after either replacement records an unexpected exit"
        ),
    }
