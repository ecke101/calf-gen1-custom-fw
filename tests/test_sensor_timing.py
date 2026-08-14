from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from calf_fw_tool.sensor_timing import (
    _SENSOR_TIMING_SOURCE,
    build_sensor_timing,
)


class SensorTimingTests(unittest.TestCase):
    def test_source_uses_verified_nodes_controls_and_master_order(self) -> None:
        self.assertIn('"/dev/v4l-subdev2"', _SENSOR_TIMING_SOURCE)
        self.assertIn('"/dev/v4l-subdev7"', _SENSOR_TIMING_SOURCE)
        self.assertIn(
            "VIDIOC_SUBDEV_S_FRAME_INTERVAL 0xc0305616UL",
            _SENSOR_TIMING_SOURCE,
        )
        self.assertIn(
            "RKMODULE_SET_QUICK_STREAM 0x400456caUL", _SENSOR_TIMING_SOURCE
        )
        self.assertIn(
            "SENSOR_HEIGHT + internal_vblank - EXPOSURE_MARGIN",
            _SENSOR_TIMING_SOURCE,
        )
        self.assertIn("value[1] == 's'", _SENSOR_TIMING_SOURCE)
        self.assertIn("value.interval.numerator = numerator", _SENSOR_TIMING_SOURCE)
        external_on = _SENSOR_TIMING_SOURCE.index(
            "set_quick_stream(external_descriptor, 1)"
        )
        internal_on = _SENSOR_TIMING_SOURCE.index(
            "set_quick_stream(internal_descriptor, 1)", external_on
        )
        self.assertLess(external_on, internal_on)

    @unittest.skipUnless(
        shutil.which("clang")
        and shutil.which("ld.lld")
        and shutil.which("readelf"),
        "AArch64 build tools are required",
    )
    def test_build_is_aarch64_and_has_no_undefined_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            output = Path(temp_name) / "calf-sensor-timing"
            result = build_sensor_timing(output)
            header = subprocess.run(
                ["readelf", "-h", str(output)],
                text=True,
                capture_output=True,
                check=True,
            ).stdout
            symbols = subprocess.run(
                ["readelf", "--dyn-syms", "--wide", str(output)],
                text=True,
                capture_output=True,
                check=True,
            ).stdout
            self.assertIn("AArch64", header)
            self.assertIn("contains 1 entry", symbols)
            self.assertEqual(result["target"], "/bin/calf-sensor-timing")
            self.assertTrue(output.stat().st_mode & 0o100)


if __name__ == "__main__":
    unittest.main()
