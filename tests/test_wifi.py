from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from calf_fw_tool.util import sha256
from calf_fw_tool.wifi import WIFI_STATE_HELPER, build_wifi_state_helper


class WifiStateHelperTests(unittest.TestCase):
    def test_helper_wraps_service_and_updates_only_wlan_profile_key(self) -> None:
        self.assertIn('"$wifiservice" "$action"', WIFI_STATE_HELPER)
        self.assertIn("^wlan:[[:space:]]*$", WIFI_STATE_HELPER)
        self.assertIn('print "  enable: " state', WIFI_STATE_HELPER)
        self.assertIn('mv "$temporary" "$profile"', WIFI_STATE_HELPER)
        self.assertIn('chown 1002:1002 "$temporary"', WIFI_STATE_HELPER)
        self.assertIn('chmod 640 "$temporary"', WIFI_STATE_HELPER)
        self.assertIn("state=true", WIFI_STATE_HELPER)
        self.assertIn("state=false", WIFI_STATE_HELPER)

    def test_build_is_ascii_and_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            destination = Path(temp_name) / "calf-wlan"
            result = build_wifi_state_helper(destination)

            self.assertEqual(destination.read_text(encoding="ascii"), WIFI_STATE_HELPER)
            self.assertEqual(result["sha256"], sha256(destination))

    def test_helper_persists_both_states_without_losing_other_yaml(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            temp = Path(temp_name)
            helper = temp / "calf-wlan"
            service = temp / "wifiservice"
            profile = temp / "ngui-profile.yaml"
            build_wifi_state_helper(helper)
            helper.chmod(0o755)
            service.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            service.chmod(0o755)
            profile.write_text(
                "cam_mode: photo\nwlan:\n  enable: true\nmedia_stor:\n"
                "  use_as_udisk: true\n",
                encoding="ascii",
            )
            environment = os.environ | {
                "CALF_WLAN_SERVICE": str(service),
                "CALF_WLAN_PROFILE": str(profile),
            }

            subprocess.run(
                [str(helper), "0"],
                check=True,
                env=environment,
                capture_output=True,
                text=True,
            )
            disabled = profile.read_text(encoding="ascii")
            self.assertIn("wlan:\n  enable: false\n", disabled)
            self.assertIn("cam_mode: photo", disabled)
            self.assertIn("use_as_udisk: true", disabled)

            subprocess.run(
                [str(helper), "1"],
                check=True,
                env=environment,
                capture_output=True,
                text=True,
            )
            enabled = profile.read_text(encoding="ascii")
            self.assertIn("wlan:\n  enable: true\n", enabled)
            self.assertEqual(enabled.count("wlan:"), 1)
            self.assertEqual(enabled.count("enable:"), 1)


if __name__ == "__main__":
    unittest.main()
