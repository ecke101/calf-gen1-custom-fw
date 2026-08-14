from __future__ import annotations

from pathlib import Path

from .util import sha256

WIFI_STATE_HELPER_FILESYSTEM_PATH = "/bin/calf-wlan"
WIFI_STATE_HELPER_RUNTIME_PATH = "/app/bin/calf-wlan"


# ngui already reads wlan.enable during its boot/reset callback. This helper
# wraps the two existing wifiservice commands and atomically updates only that
# profile field, completing the missing write side of the stock behavior.
WIFI_STATE_HELPER = r"""#!/bin/sh

case "$1" in
    1)
        action=start
        state=true
        ;;
    0)
        action=stop
        state=false
        ;;
    *)
        echo "Usage: $0 {0|1}" >&2
        exit 2
        ;;
esac

wifiservice=${CALF_WLAN_SERVICE:-/app/bin/wifiservice}
profile=${CALF_WLAN_PROFILE:-/local/ngui-profile.yaml}

"$wifiservice" "$action"
service_result=$?

temporary=${profile}.calf.$$

if [ -f "$profile" ]; then
    if grep -q '^wlan:[[:space:]]*$' "$profile"; then
        awk -v state="$state" '
            BEGIN { in_wlan = 0; wrote = 0 }
            /^wlan:[[:space:]]*$/ {
                print
                in_wlan = 1
                next
            }
            in_wlan && /^[^[:space:]]/ {
                if (!wrote)
                    print "  enable: " state
                in_wlan = 0
            }
            in_wlan && /^[[:space:]]+enable:[[:space:]]*/ {
                if (!wrote)
                    print "  enable: " state
                wrote = 1
                next
            }
            { print }
            END {
                if (in_wlan && !wrote)
                    print "  enable: " state
            }
        ' "$profile" > "$temporary"
        profile_result=$?
    else
        {
            cat "$profile"
            printf '\nwlan:\n  enable: %s\n' "$state"
        } > "$temporary"
        profile_result=$?
    fi

    if [ "$profile_result" -eq 0 ]; then
        chown 1002:1002 "$temporary"
        chmod 640 "$temporary"
        mv "$temporary" "$profile"
        profile_result=$?
        sync
    fi
else
    profile_result=1
fi

if [ -f "$temporary" ]; then
    rm -f "$temporary"
fi

if [ "$profile_result" -ne 0 ]; then
    echo "warning: could not persist wlan.enable=$state" >&2
fi

exit "$service_result"
"""


def build_wifi_state_helper(destination: Path) -> dict[str, object]:
    destination.write_text(WIFI_STATE_HELPER, encoding="ascii")
    return {
        "target": WIFI_STATE_HELPER_FILESYSTEM_PATH,
        "runtime_path": WIFI_STATE_HELPER_RUNTIME_PATH,
        "sha256": sha256(destination),
        "description": (
            "Wrap wifiservice start/stop and atomically save wlan.enable in "
            "/local/ngui-profile.yaml, so boot restores the last UI-selected "
            "Wi-Fi state instead of forcing Wi-Fi permanently on."
        ),
    }
