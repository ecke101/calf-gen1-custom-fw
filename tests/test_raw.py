from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from calf_fw_tool.raw import _RAW_DNG_SOURCE, build_raw_dng


class RawDngTests(unittest.TestCase):
    def test_source_validates_rockchip_header_and_writes_required_dng_tags(
        self,
    ) -> None:
        self.assertIn("get16(raw_header + 17)", _RAW_DNG_SOURCE)
        self.assertIn("get16(raw_header + 19)", _RAW_DNG_SOURCE)
        self.assertIn("header[12] != 12U", _RAW_DNG_SOURCE)
        self.assertIn("33422, TIFF_BYTE", _RAW_DNG_SOURCE)
        self.assertIn("50706, TIFF_BYTE", _RAW_DNG_SOURCE)
        self.assertIn("50721, TIFF_SRATIONAL", _RAW_DNG_SOURCE)
        self.assertIn("50722, TIFF_SRATIONAL", _RAW_DNG_SOURCE)
        self.assertIn("50728, TIFF_RATIONAL", _RAW_DNG_SOURCE)
        self.assertIn("50829, TIFF_LONG", _RAW_DNG_SOURCE)

    def test_source_supports_black_level_aware_multiframe_stacking(self) -> None:
        self.assertIn("#define MAX_RAW_INPUTS 24U", _RAW_DNG_SOURCE)
        self.assertIn("input_count != 2U", _RAW_DNG_SOURCE)
        self.assertIn("input_count != 4U", _RAW_DNG_SOURCE)
        self.assertIn("input_count != 8U", _RAW_DNG_SOURCE)
        self.assertIn("input_count != 16U", _RAW_DNG_SOURCE)
        self.assertIn("input_count != 24U", _RAW_DNG_SOURCE)
        self.assertIn("first - SENSOR_BLACK_LEVEL", _RAW_DNG_SOURCE)
        self.assertIn("second - SENSOR_BLACK_LEVEL", _RAW_DNG_SOURCE)
        self.assertIn("value > SENSOR_WHITE_LEVEL", _RAW_DNG_SOURCE)
        self.assertIn("same_raw_layout(raw_header, candidate_header)", _RAW_DNG_SOURCE)

    @unittest.skipUnless(
        shutil.which("clang")
        and shutil.which("ld.lld")
        and shutil.which("readelf"),
        "AArch64 build tools are required",
    )
    def test_build_is_aarch64_and_has_no_undefined_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            output = Path(temp_name) / "calf-raw2dng"
            result = build_raw_dng(output)
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
            self.assertEqual(result["target"], "/bin/calf-raw2dng")
            self.assertTrue(output.stat().st_mode & 0o100)


if __name__ == "__main__":
    unittest.main()
