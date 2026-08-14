from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from calf_fw_tool.iq import IMX577_IQ_PATH, patch_imx577_min_fps
from calf_fw_tool.util import FirmwareToolError

_IQ = b'{\n\t"sensor_calib": {\n\t\t"CISMinFps":\t5,\n\t\t"CISFlip":\t0\n\t}\n}\n'


class IqPatchTests(unittest.TestCase):
    def test_lowers_only_the_minimum_frame_rate(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            temp = Path(temp_name)
            source = temp / "imx577.json"
            output = temp / "imx577.patched.json"
            source.write_bytes(_IQ)

            with patch("calf_fw_tool.iq.require_hash"), patch(
                "calf_fw_tool.iq.sha256",
                side_effect=["source", "patched"],
            ):
                result = patch_imx577_min_fps(source, output)

            patched = output.read_bytes()
            self.assertEqual(len(patched), len(_IQ))
            self.assertEqual(patched.count(b'"CISMinFps":\t2,'), 1)
            self.assertNotIn(b'"CISMinFps":\t5,', patched)
            self.assertEqual(result["target"], IMX577_IQ_PATH)

    def test_refuses_an_unknown_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            temp = Path(temp_name)
            source = temp / "imx577.json"
            output = temp / "imx577.patched.json"
            source.write_bytes(_IQ.replace(b"CISMinFps", b"MinimumFps"))
            with patch("calf_fw_tool.iq.require_hash"):
                with self.assertRaises(FirmwareToolError):
                    patch_imx577_min_fps(source, output)


if __name__ == "__main__":
    unittest.main()
