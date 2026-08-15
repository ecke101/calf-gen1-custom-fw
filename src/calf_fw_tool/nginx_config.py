from __future__ import annotations

from pathlib import Path

from .util import FirmwareToolError, require_hash, sha256

NGINX_CONFIG_PATH = "/nginx/conf/nginx.conf"
NGINX_CONFIG_SHA256 = (
    "8b683d9dd4041a7cf9fb119246b3766024e953e728f67138515b3a905e1a0a24"
)

_STOCK_DOWNLOAD_LOCATION = b"""        location /download {
            alias    /media/DCIM/;
        #    autoindex on;
            autoindex_exact_size off;
            autoindex_localtime on;
        }
"""

_SD_DOWNLOAD_LOCATION = b"""        location /download {
            alias    /mnt/mmcblk1p1/;
            autoindex on;
            autoindex_exact_size off;
            autoindex_localtime on;
        }
"""


def patch_download_location(source: Path, destination: Path) -> dict[str, str]:
    require_hash(source, NGINX_CONFIG_SHA256, NGINX_CONFIG_PATH)
    data = source.read_bytes()
    if data.count(_STOCK_DOWNLOAD_LOCATION) != 1:
        raise FirmwareToolError(
            "expected exactly one stock /download location in nginx.conf"
        )
    patched = data.replace(_STOCK_DOWNLOAD_LOCATION, _SD_DOWNLOAD_LOCATION)
    destination.write_bytes(patched)
    return {
        "path": f"/app{NGINX_CONFIG_PATH}",
        "stock_sha256": sha256(source),
        "installed_sha256": sha256(destination),
    }
