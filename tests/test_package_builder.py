from __future__ import annotations

import subprocess
import tarfile
from pathlib import Path

from calf_fw_tool.package_builder import (
    _LICENSE_FILES,
    PACKAGE_ROOT,
    _installer_script,
    _write_package,
)
from calf_fw_tool.util import sha256

ROOT = Path(__file__).resolve().parents[1]


def _fake_payload() -> dict[str, dict[str, object]]:
    names = (
        "calf-capture-server",
        "calf-ngcd",
        "calf-raw2dng",
        "calf-sensor-timing",
        "calf-snapshot-request",
        "calf-ui",
        "calf-wlan",
        "ngcd",
        "ngui",
    )
    return {
        name: {
            "destination": f"/app/bin/{name}",
            "sha256": f"{index:064x}",
            "size": index + 1,
        }
        for index, name in enumerate(names, 1)
    }


def test_installer_preserves_stock_as_hard_links_and_has_rollback(tmp_path) -> None:
    installer = tmp_path / "install.sh"
    installer.write_text(
        _installer_script(
            _fake_payload(),
            {
                "ngcd": ("a" * 64, "b" * 64),
                "ngui": ("c" * 64, "d" * 64),
                "iq": ("e" * 64, "f" * 64),
            },
        ),
        encoding="ascii",
    )

    subprocess.run(["/bin/sh", "-n", str(installer)], check=True)
    text = installer.read_text(encoding="ascii")
    assert 'ln "$current" "$stock"' in text
    assert 'ln "$stock" "$temporary"' in text
    assert "cp /app/bin/ngcd /app/bin/ngcd-stock" not in text
    assert "pause_monitor" in text
    assert "stop_children" in text
    assert "rollback_to_stock" in text
    assert "CISMinFps" in text
    assert "restore_stock_iq" in text
    assert "/local/calf-custom-fw-uninstall --rollback" in text
    assert "vpupdate.bin" not in text
    assert "app.img" not in text


def test_package_writer_is_deterministic_and_contains_only_given_tree(tmp_path) -> None:
    root = tmp_path / "root"
    (root / "bin").mkdir(parents=True)
    (root / "bin/calf-ui").write_bytes(b"ui")
    (root / "install.sh").write_text("#!/bin/sh\n", encoding="ascii")
    (root / "manifest.json").write_text("{}\n", encoding="ascii")
    first = tmp_path / "first.tar.gz"
    second = tmp_path / "second.tar.gz"

    _write_package(root, first)
    _write_package(root, second)

    assert sha256(first) == sha256(second)
    with tarfile.open(first, "r:gz") as archive:
        names = archive.getnames()
        assert f"{PACKAGE_ROOT}/bin/calf-ui" in names
        assert all("vpupdate" not in name for name in names)
        assert all(not name.endswith(".img") for name in names)
        assert all("stock" not in name for name in names)


def test_package_licensing_and_provenance_sources_are_complete() -> None:
    expected = {
        "NOTICE",
        "PROVENANCE.md",
        "THIRD_PARTY_LICENSES.md",
        "LICENSES/Apache-2.0.txt",
        "LICENSES/libxaac-NOTICE.txt",
        "LICENSES/libxaac-ORIGIN.md",
        "LICENSES/vo-aacenc-NOTICE.txt",
        "LICENSES/vo-aacenc-ORIGIN.md",
        "LICENSES/OFL-1.1.txt",
        "LICENSES/Noto-ORIGIN.md",
        "LICENSES/stb-MIT.txt",
        "LICENSES/stb-ORIGIN.md",
    }

    assert set(_LICENSE_FILES) == expected
    assert _LICENSE_FILES["LICENSES/Apache-2.0.txt"] == "LICENSE"
    assert all((ROOT / source).is_file() for source in _LICENSE_FILES.values())
