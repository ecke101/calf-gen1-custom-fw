from __future__ import annotations

from .model import FirmwareIdentity

CALF_216_ARCHIVE_NAME = "CALF-VR180-Camera-V2.1.6-20250102-UPDATE.tar.gz"
VIEWPT_221_ARCHIVE_NAME = "VIEWPT-REALIA-V2.2.1-20260626-UPDATE.tar.gz"

CALF_216_FIRMWARE = FirmwareIdentity(
    version="2.1.6",
    source_archive_sha256=(
        "387e7899b82bee861e2f3b997b83b1dee4493d860092007942629dacba8ae8c4"
    ),
    vpupdate_sha256=(
        "f7fc4421296bd52b0521149c72248900f4db2d75b60d20ac6db6e870d6dfdac8"
    ),
    app_image_sha256=(
        "9178f741491861627bf00b87f0637254e5d4983f98ee0382742a775037fd989f"
    ),
    ngcd_sha256=(
        "c59c40c876a8fb2e52aecaa3f7d5a4596abd3bdcb0cf1a949ad68763408cf5f4"
    ),
    ngui_sha256=(
        "15274192c0b3305ee4ba28a712d0bd540b85b1169918001d72b2f10733958bf8"
    ),
)

VIEWPT_221_FIRMWARE = FirmwareIdentity(
    version="2.2.1",
    source_archive_sha256=(
        "1cdf3ed5236cc6686c8581c64c5fb93afe2d2054e1a3c8832068a5674965e635"
    ),
    vpupdate_sha256=(
        "0e9946153684f6a176c0f521546710c24b3597444d502b6aa327f83b9d7cfd1c"
    ),
    app_image_sha256=(
        "95120f99d2a63d2462463ddadf9ec04e8ad5d5964eb4a3ef6bf378d2c9806003"
    ),
    ngcd_sha256=(
        "b48db001d9f7d395be4ba70c290a1b66ac2f3b68736b1b23b791d1bea9cfd488"
    ),
    ngui_sha256=(
        "ff7fcbaf5d249552423031d11b2fdf5bf0132d5abec977e483c1f90b095e3447"
    ),
)
