from __future__ import annotations

from pathlib import Path

from .util import FirmwareToolError, require_hash, sha256

IMX577_IQ_PATH = "/data/imx577_VIEWPT_VP415.json"
IMX577_IQ_SHA256 = (
    "df3accdb249834c5a699299d60f34262df3910fd2c908a847597ac936819c283"
)

_STOCK_MIN_FPS = b'"CISMinFps":\t5,'
_NIGHT_MIN_FPS = b'"CISMinFps":\t2,'


def patch_imx577_min_fps(source: Path, destination: Path) -> dict[str, object]:
    require_hash(source, IMX577_IQ_SHA256, IMX577_IQ_PATH)
    data = source.read_bytes()
    if data.count(_STOCK_MIN_FPS) != 1:
        raise FirmwareToolError(
            f"expected exactly one CISMinFps=5 field in {IMX577_IQ_PATH}"
        )
    if _NIGHT_MIN_FPS in data:
        raise FirmwareToolError(f"{IMX577_IQ_PATH} is already patched")

    destination.write_bytes(data.replace(_STOCK_MIN_FPS, _NIGHT_MIN_FPS))
    return {
        "target": IMX577_IQ_PATH,
        "source_sha256": sha256(source),
        "patched_sha256": sha256(destination),
        "patches": [
            {
                "name": "imx577-minimum-frame-rate",
                "before": "CISMinFps=5",
                "after": "CISMinFps=2",
                "size": 1,
                "description": (
                    "Permit the linear IMX577 exposure policy to lower the "
                    "sensor frame rate to 2 fps for a requested 1/2-second "
                    "integration."
                ),
            }
        ],
    }
