from __future__ import annotations

import gzip
import io
import json
import shutil
import tarfile
import tempfile
from pathlib import Path

from .backend_selector import build_backend_selector
from .build_output import prepare_output
from .capture import build_capture_request, build_capture_server
from .ext4_read import dump_file
from .firmware_archive import extract_app_image, extract_official_outer
from .iq import IMX577_IQ_PATH, patch_imx577_min_fps
from .ngcd_builder import build_ngcd_binary
from .raw import build_raw_dng
from .sensor_timing import build_sensor_timing
from .target_spec import DEFAULT_TARGET, FirmwareTarget, package_name, resolve_target
from .ui_builder import build_ui_binary
from .ui_selector import build_ui_selector
from .util import FirmwareToolError, require_commands, require_hash, sha256
from .wifi import build_wifi_state_helper

PACKAGE_NAME = package_name(DEFAULT_TARGET.firmware.version)
PACKAGE_ROOT = "calf-custom-fw"

_PAYLOAD_BUILDERS = {
    "calf-capture-server": build_capture_server,
    "calf-snapshot-request": build_capture_request,
    "calf-sensor-timing": build_sensor_timing,
    "calf-raw2dng": build_raw_dng,
    "calf-wlan": build_wifi_state_helper,
}

_LICENSE_FILES = {
    "NOTICE": "NOTICE",
    "PROVENANCE.md": "PROVENANCE.md",
    "THIRD_PARTY_LICENSES.md": "THIRD_PARTY_LICENSES.md",
    "LICENSES/Apache-2.0.txt": "LICENSE",
    "LICENSES/libxaac-NOTICE.txt": "ngcd/vendor/libxaac/NOTICE",
    "LICENSES/libxaac-ORIGIN.md": "ngcd/vendor/libxaac/ORIGIN.md",
    "LICENSES/vo-aacenc-NOTICE.txt": "ngcd/vendor/vo-aacenc/NOTICE",
    "LICENSES/vo-aacenc-ORIGIN.md": "ngcd/vendor/vo-aacenc/README.calf",
    "LICENSES/OFL-1.1.txt": "ui/assets/fonts/OFL.txt",
    "LICENSES/Noto-ORIGIN.md": "ui/assets/fonts/README.md",
    "LICENSES/stb-MIT.txt": "ui/third_party/stb/LICENSE",
    "LICENSES/stb-ORIGIN.md": "ui/third_party/README.md",
}


def _quoted_shell(value: str) -> str:
    if "'" in value or "\r" in value:
        raise FirmwareToolError(f"unsafe generated shell value: {value!r}")
    return f"'{value}'"


def _stock_hashes(
    source: Path, temporary: Path, target: FirmwareTarget
) -> dict[str, tuple[str, ...]]:
    vpupdate, _ = extract_official_outer(source, temporary / "official")
    app = extract_app_image(vpupdate, temporary / "official")
    original_ngcd = temporary / "stock-ngcd"
    original_ngui = temporary / "stock-ngui"
    original_iq = temporary / "stock-imx577.json"
    patched_iq = temporary / "calf-imx577.json"
    dump_file(app, "/bin/ngcd", original_ngcd)
    dump_file(app, "/bin/ngui", original_ngui)
    dump_file(app, IMX577_IQ_PATH, original_iq)
    require_hash(original_ngcd, target.firmware.ngcd_sha256, "/bin/ngcd")
    require_hash(original_ngui, target.firmware.ngui_sha256, "/bin/ngui")
    patch_imx577_min_fps(original_iq, patched_iq)
    return {
        "ngcd": target.accepted_ngcd_sha256,
        "ngui": target.accepted_ngui_sha256,
        "iq": (sha256(original_iq), sha256(patched_iq)),
    }


def _installer_script(
    payload: dict[str, dict[str, object]],
    stock_hashes: dict[str, tuple[str, ...]],
) -> str:
    payload_lines = "\n".join(
        f"{name} {entry['sha256']}" for name, entry in sorted(payload.items())
    )
    destination_lines = "\n".join(
        f"{name} {entry['destination']}" for name, entry in sorted(payload.items())
    )
    required_kb = (
        sum(int(entry["size"]) for entry in payload.values()) + 1023
    ) // 1024 + 4096
    return f"""#!/bin/sh

set -u

payload_hashes={_quoted_shell(payload_lines)}
destinations={_quoted_shell(destination_lines)}
allowed_ngcd={_quoted_shell(' '.join(stock_hashes['ngcd']))}
allowed_ngui={_quoted_shell(' '.join(stock_hashes['ngui']))}
stock_iq_sha256={_quoted_shell(stock_hashes['iq'][0])}
calf_iq_sha256={_quoted_shell(stock_hashes['iq'][1])}
required_kb={required_kb}
stock_marker=/tmp/calf-ui-stock-session
iq_current=/app/data/imx577_VIEWPT_VP415.json
iq_stock=/app/data/imx577_VIEWPT_VP415.json-calf-custom-fw-stock
iq_stage=/app/data/.calf-custom-fw-imx577-$$
iq_stock_stage=/app/data/.calf-custom-fw-stock-imx577-$$
iq_stage_ready=0
monitor_pid=
monitor_paused=0
transaction_started=0
rollback_active=0
local_manifest=/local/.calf-custom-fw-manifest.$$
local_uninstall=/local/.calf-custom-fw-uninstall.$$

fail()
{{
    echo "Installation error: $*" >&2
    if [ "$transaction_started" -eq 1 ] && [ "$rollback_active" -eq 0 ]; then
        rollback_active=1
        if rollback_to_stock; then
            echo "Installation rollback: stock process pair restored." >&2
        else
            echo "Installation rollback failed; reboot and use recovery." >&2
        fi
    fi
    exit 1
}}

file_hash()
{{
    sha256sum "$1" 2>/dev/null | awk '{{print $1}}'
}}

hash_allowed()
{{
    wanted=$1
    shift
    for candidate in "$@"; do
        [ "$wanted" = "$candidate" ] && return 0
    done
    return 1
}}

find_one_process()
{{
    wanted=$1
    found=
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        read -r name < "$proc/comm" || continue
        [ "$name" = "$wanted" ] || continue
        [ -z "$found" ] || return 2
        found=${{proc##*/}}
    done
    [ -n "$found" ] || return 1
    printf '%s\n' "$found"
}}

matching_children()
{{
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        read -r name < "$proc/comm" || continue
        case "$name" in
            ngcd|calf-ngcd|ngui|calf-ui) printf '%s\n' "${{proc##*/}}" ;;
        esac
    done
}}

stop_children()
{{
    children=$(matching_children)
    [ -z "$children" ] || kill -TERM $children 2>/dev/null || return 1
    attempts=0
    while [ "$attempts" -lt 30 ]; do
        [ -z "$(matching_children)" ] && return 0
        attempts=$((attempts + 1))
        sleep 1
    done
    return 1
}}

pause_monitor()
{{
    monitor_pid=$(find_one_process ngmonitor) || return 1
    kill -STOP "$monitor_pid" || return 1
    monitor_paused=1
}}

resume_monitor()
{{
    [ "$monitor_paused" -eq 1 ] || return 0
    kill -CONT "$monitor_pid" || return 1
    monitor_paused=0
}}

resume_on_exit()
{{
    if [ "$monitor_paused" -eq 1 ] && [ -n "$monitor_pid" ]; then
        kill -CONT "$monitor_pid" 2>/dev/null || :
    fi
}}

cleanup_temporary()
{{
    while read -r name destination; do
        [ -n "$name" ] || continue
        rm -f "/app/bin/.calf-custom-fw-$name-$$"
    done <<'CALF_PACKAGE_CLEANUP'
$destinations
CALF_PACKAGE_CLEANUP
    rm -f "$local_manifest" "$local_uninstall"
    rm -f "$iq_stage" "$iq_stock_stage"
}}

exit_cleanup()
{{
    cleanup_temporary
    resume_on_exit
}}

signal_exit()
{{
    fail "installation interrupted"
}}

trap exit_cleanup EXIT
trap signal_exit HUP INT TERM

restore_stock_entry()
{{
    name=$1
    stock=/app/bin/$name-stock
    temporary=/app/bin/.calf-custom-fw-restore-$name-$$
    rm -f "$temporary"
    ln "$stock" "$temporary" || return 1
    mv -f "$temporary" "/app/bin/$name" || {{
        rm -f "$temporary"
        return 1
    }}
}}

restore_stock_iq()
{{
    temporary=/app/data/.calf-custom-fw-restore-imx577-$$
    rm -f "$temporary"
    ln "$iq_stock" "$temporary" || return 1
    mv -f "$temporary" "$iq_current" || {{
        rm -f "$temporary"
        return 1
    }}
}}

rollback_to_stock()
{{
    if [ "$monitor_paused" -ne 1 ]; then
        pause_monitor || return 1
    fi
    stop_children || return 1
    if [ -x /app/bin/calf-capture-server ]; then
        /app/bin/calf-capture-server stop >/dev/null 2>&1 || :
    fi
    restore_stock_entry ngcd || return 1
    restore_stock_entry ngui || return 1
    restore_stock_iq || return 1
    rm -f "$stock_marker"
    sync
    resume_monitor || return 1
    return 0
}}

uninstall_custom_firmware()
{{
    [ -f /app/bin/ngcd-stock ] && [ ! -L /app/bin/ngcd-stock ] ||
        fail "the preserved stock backend is missing"
    [ -f /app/bin/ngui-stock ] && [ ! -L /app/bin/ngui-stock ] ||
        fail "the preserved stock UI is missing"
    [ -f "$iq_stock" ] && [ ! -L "$iq_stock" ] ||
        fail "the preserved stock sensor policy is missing"
    rollback_to_stock || fail "could not restore the stock process pair"
    rm -f /app/bin/calf-ngcd /app/bin/calf-ui \
        /app/bin/calf-capture-server /app/bin/calf-snapshot-request \
        /app/bin/calf-sensor-timing /app/bin/calf-raw2dng \
        /app/bin/calf-wlan
    rm -f /app/bin/ngcd-stock /app/bin/ngui-stock
    rm -f "$iq_stock"
    rm -f /local/calf-custom-fw-manifest.json /local/calf-custom-fw-uninstall
    sync
    echo "CALF custom firmware removed; the original backend and UI are active."
}}

if [ "${{1:-}}" = "--rollback" ]; then
    uninstall_custom_firmware
    exit 0
fi
[ "$#" -eq 0 ] || fail "usage: $0 [--rollback]"

payload_root=$(dirname "$0")
payload_root=$(cd "$payload_root" && pwd -P) || fail "cannot resolve package directory"
[ -d "$payload_root/bin" ] || fail "package bin directory is missing"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is unavailable"
command -v awk >/dev/null 2>&1 || fail "awk is unavailable"
command -v sed >/dev/null 2>&1 || fail "sed is unavailable"
[ -d /app/bin ] && [ -w /app/bin ] || fail "/app/bin is not writable"
[ -d /app/data ] && [ -w /app/data ] || fail "/app/data is not writable"
[ -d /local ] && [ -w /local ] || fail "/local is not writable"

while read -r name expected; do
    [ -n "$name" ] || continue
    source_file=$payload_root/bin/$name
    [ -f "$source_file" ] && [ ! -L "$source_file" ] ||
        fail "payload file is missing or unsafe: $name"
    actual=$(file_hash "$source_file") || fail "cannot hash payload file: $name"
    [ "$actual" = "$expected" ] || fail "payload hash mismatch: $name"
done <<'CALF_PACKAGE_HASHES'
$payload_hashes
CALF_PACKAGE_HASHES

available_kb=$(df -Pk /app/bin 2>/dev/null | awk 'NR == 2 {{print $4}}')
case "$available_kb" in ''|*[!0-9]*) fail "cannot determine free app space" ;; esac
[ "$available_kb" -ge "$required_kb" ] ||
    fail "insufficient app space: need $required_kb KiB, have $available_kb KiB"

for mounted in ngcd ngui; do
    if grep -q " /app/bin/$mounted " /proc/mounts; then
        fail "temporary bind mount exists at /app/bin/$mounted; reboot or roll it back first"
    fi
done

preserve_stock()
{{
    name=$1
    allowed=$2
    current=/app/bin/$name
    stock=/app/bin/$name-stock
    if [ -e "$stock" ]; then
        [ -f "$stock" ] && [ ! -L "$stock" ] || return 1
        digest=$(file_hash "$stock") || return 1
        hash_allowed "$digest" $allowed
        return
    fi
    [ -f "$current" ] && [ ! -L "$current" ] || return 1
    digest=$(file_hash "$current") || return 1
    hash_allowed "$digest" $allowed || return 1
    ln "$current" "$stock" || return 1
    sync
}}

preserve_stock ngcd "$allowed_ngcd" || fail "backend is not a supported stock or prior installation"
preserve_stock ngui "$allowed_ngui" || fail "UI is not a supported stock or prior installation"

write_iq_variant()
{{
    source=$1
    destination=$2
    from=$3
    to=$4
    expected=$5
    [ ! -e "$destination" ] || return 1
    sed 's/"CISMinFps":\t'"$from"',/"CISMinFps":\t'"$to"',/' \
        "$source" > "$destination" || return 1
    digest=$(file_hash "$destination") || return 1
    [ "$digest" = "$expected" ] || return 1
}}

[ -f "$iq_current" ] && [ ! -L "$iq_current" ] ||
    fail "installed sensor policy is missing or unsafe"
current_iq_digest=$(file_hash "$iq_current") ||
    fail "cannot hash the installed sensor policy"
if [ -e "$iq_stock" ]; then
    [ -f "$iq_stock" ] && [ ! -L "$iq_stock" ] ||
        fail "preserved stock sensor policy is unsafe"
    [ "$(file_hash "$iq_stock")" = "$stock_iq_sha256" ] ||
        fail "preserved stock sensor policy has the wrong identity"
elif [ "$current_iq_digest" = "$stock_iq_sha256" ]; then
    ln "$iq_current" "$iq_stock" || fail "cannot preserve stock sensor policy"
    sync
elif [ "$current_iq_digest" = "$calf_iq_sha256" ]; then
    write_iq_variant "$iq_current" "$iq_stock_stage" 2 5 "$stock_iq_sha256" ||
        fail "cannot reconstruct the stock sensor policy"
    chmod 664 "$iq_stock_stage" || fail "cannot chmod stock sensor policy"
    chown 1002:1002 "$iq_stock_stage" || fail "cannot chown stock sensor policy"
    mv "$iq_stock_stage" "$iq_stock" ||
        fail "cannot preserve reconstructed stock sensor policy"
    sync
else
    fail "sensor policy is not the supported stock/CALF version"
fi

case "$current_iq_digest" in
    "$stock_iq_sha256")
        write_iq_variant "$iq_current" "$iq_stage" 5 2 "$calf_iq_sha256" ||
            fail "cannot stage the 2 fps sensor policy"
        chmod 664 "$iq_stage" || fail "cannot chmod the sensor policy"
        chown 1002:1002 "$iq_stage" || fail "cannot chown the sensor policy"
        iq_stage_ready=1
        ;;
    "$calf_iq_sha256") ;;
    *) fail "sensor policy changed during installation" ;;
esac

staged=
while read -r name destination; do
    [ -n "$name" ] || continue
    stage=/app/bin/.calf-custom-fw-$name-$$
    [ ! -e "$stage" ] || fail "staging path already exists: $stage"
    cp "$payload_root/bin/$name" "$stage" || fail "could not stage $name"
    chmod 755 "$stage" || fail "could not chmod $name"
    chown 1002:1002 "$stage" || fail "could not chown $name"
    staged="$staged $stage"
done <<'CALF_PACKAGE_DESTINATIONS'
$destinations
CALF_PACKAGE_DESTINATIONS

[ ! -e "$local_manifest" ] || fail "local manifest staging path already exists"
[ ! -e "$local_uninstall" ] || fail "local uninstaller staging path already exists"
cp "$payload_root/manifest.json" "$local_manifest" || fail "cannot stage local manifest"
cp "$payload_root/install.sh" "$local_uninstall" || fail "cannot stage uninstaller"
chmod 644 "$local_manifest" || fail "cannot chmod local manifest"
chmod 755 "$local_uninstall" || fail "cannot chmod local uninstaller"
sync

pause_monitor || fail "expected exactly one ngmonitor process"
stop_children || fail "replacement/stock children did not stop; reboot to recover"
if [ -x /app/bin/calf-capture-server ]; then
    /app/bin/calf-capture-server stop >/dev/null 2>&1 || :
fi

transaction_started=1
if [ "$iq_stage_ready" -eq 1 ]; then
    mv "$iq_stage" "$iq_current" || fail "cannot publish the 2 fps sensor policy"
    iq_stage_ready=0
fi
while read -r name destination; do
    [ -n "$name" ] || continue
    stage=/app/bin/.calf-custom-fw-$name-$$
    mv -f "$stage" "$destination" || fail "commit failed while publishing $name"
done <<'CALF_PACKAGE_COMMIT'
$destinations
CALF_PACKAGE_COMMIT

mv -f "$local_manifest" /local/calf-custom-fw-manifest.json || fail "cannot publish manifest"
mv -f "$local_uninstall" /local/calf-custom-fw-uninstall || fail "cannot publish uninstaller"
rm -f "$stock_marker"
sync
resume_monitor || fail "could not resume ngmonitor; reboot to recover"

stable=0
attempts=0
while [ "$attempts" -lt 20 ]; do
    backend=$(find_one_process calf-ngcd 2>/dev/null || :)
    ui=$(find_one_process calf-ui 2>/dev/null || :)
    if [ -n "$backend" ] && [ -n "$ui" ]; then
        stable=$((stable + 1))
        [ "$stable" -ge 3 ] && break
    else
        stable=0
    fi
    attempts=$((attempts + 1))
    sleep 2
done

if [ "$stable" -lt 3 ]; then
    fail "replacement process pair did not become stable"
fi

transaction_started=0
trap - HUP INT TERM
trap - EXIT
cleanup_temporary
echo "CALF custom firmware installed: calf-ngcd PID $backend, calf-ui PID $ui"
echo "Rollback command: /local/calf-custom-fw-uninstall --rollback"
"""


def _tar_info(name: str, size: int, mode: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size = size
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    return info


def _write_package(root: Path, output: Path) -> None:
    files = sorted(path for path in root.rglob("*") if path.is_file())
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as archive:
                directories = {PACKAGE_ROOT}
                for path in files:
                    relative = path.relative_to(root)
                    parent = relative.parent
                    while str(parent) != ".":
                        directories.add(f"{PACKAGE_ROOT}/{parent.as_posix()}")
                        parent = parent.parent
                for name in sorted(directories):
                    info = _tar_info(f"{name}/", 0, 0o755)
                    info.type = tarfile.DIRTYPE
                    archive.addfile(info)
                for path in files:
                    relative = path.relative_to(root).as_posix()
                    data = path.read_bytes()
                    mode = 0o755 if relative == "install.sh" or relative.startswith("bin/") else 0o644
                    archive.addfile(
                        _tar_info(f"{PACKAGE_ROOT}/{relative}", len(data), mode),
                        io.BytesIO(data),
                    )


def build_package(
    source: Path,
    output: Path,
    *,
    ui_source: Path,
    ngcd_source: Path,
    repository_root: Path,
    force: bool = False,
) -> dict[str, object]:
    """Build a vendor-file-free incremental camera package."""

    target = resolve_target(source)
    firmware = target.firmware
    archive_name = package_name(firmware.version)
    require_commands(("clang", "debugfs", "llvm-ar", "sha256sum"))
    prepare_output(output, force)
    with tempfile.TemporaryDirectory(prefix=".package-work-", dir=output.parent) as name:
        temporary = Path(name)
        package_root = temporary / PACKAGE_ROOT
        bin_root = package_root / "bin"
        helper_root = temporary / "helpers"
        bin_root.mkdir(parents=True)
        helper_root.mkdir()

        calf_ngcd = bin_root / "calf-ngcd"
        calf_ui = bin_root / "calf-ui"
        build_ngcd_binary(
            source,
            calf_ngcd,
            ngcd_source=ngcd_source,
            build_time="reproducible-source-build",
            firmware=firmware,
        )
        build_ui_binary(source, calf_ui, ui_source=ui_source, firmware=firmware)

        for filename, builder in _PAYLOAD_BUILDERS.items():
            built_helper = helper_root / filename
            builder(built_helper)
            shutil.copyfile(built_helper, bin_root / filename)
            (bin_root / filename).chmod(0o755)
        build_backend_selector(bin_root / "ngcd")
        build_ui_selector(bin_root / "ngui")

        destinations = {
            "ngcd": "/app/bin/ngcd",
            "ngui": "/app/bin/ngui",
            **{
                name: f"/app/bin/{name}"
                for name in (
                    "calf-ngcd",
                    "calf-ui",
                    "calf-capture-server",
                    "calf-snapshot-request",
                    "calf-sensor-timing",
                    "calf-raw2dng",
                    "calf-wlan",
                )
            },
        }
        payload = {
            path.name: {
                "destination": destinations[path.name],
                "sha256": sha256(path),
                "size": path.stat().st_size,
            }
            for path in sorted(bin_root.iterdir())
        }
        expected_names = set(destinations)
        if set(payload) != expected_names:
            raise FirmwareToolError(
                f"unexpected package payload: {sorted(payload)}, "
                f"expected {sorted(expected_names)}"
            )

        stocks = _stock_hashes(source, temporary, target)
        manifest = {
            "project": "calf-gen1-custom-fw",
            "profile": "calf-custom-fw",
            "target_firmware": firmware.version,
            "target_archive_sha256": firmware.source_archive_sha256,
            "format": (
                "CALF-built files and redistributable notices only; no "
                "camera-vendor firmware file"
            ),
            "license": (
                "Apache-2.0 for CALF-authored files; bundled components retain "
                "the licenses identified in NOTICE and THIRD_PARTY_LICENSES.md"
            ),
            "payload": payload,
            "preserved_on_camera": {
                "/app/bin/ngcd-stock": list(stocks["ngcd"]),
                "/app/bin/ngui-stock": list(stocks["ngui"]),
                "/app/data/imx577_VIEWPT_VP415.json-calf-custom-fw-stock": stocks[
                    "iq"
                ][0],
                "vendor_libraries": "used in place and never included in this package",
            },
            "on_camera_transform": {
                "path": "/app/data/imx577_VIEWPT_VP415.json",
                "stock_sha256": stocks["iq"][0],
                "installed_sha256": stocks["iq"][1],
                "change": "CISMinFps 5 to 2; generated locally and restored on rollback",
            },
            "installation": {
                "supervisor": "/app/bin/ngmonitor",
                "transaction": "stage, pause supervisor, atomically rename, verify, rollback on failure",
                "rollback": "/local/calf-custom-fw-uninstall --rollback",
            },
            "licenses": sorted(_LICENSE_FILES),
        }
        manifest_path = package_root / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        install = package_root / "install.sh"
        install.write_text(_installer_script(payload, stocks), encoding="ascii")
        install.chmod(0o755)

        for license_name, source_name in _LICENSE_FILES.items():
            license_source = repository_root / source_name
            if not license_source.is_file():
                raise FirmwareToolError(
                    f"package license file is missing: {license_source}"
                )
            license_target = package_root / license_name
            license_target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(license_source, license_target)

        package = temporary / archive_name
        _write_package(package_root, package)
        package_sidecar = temporary / f"{archive_name}.sha256"
        package_sidecar.write_text(
            f"{sha256(package)}  {archive_name}\n", encoding="ascii"
        )

        published_manifest = output / "manifest.json"
        published_package = output / archive_name
        published_sidecar = output / package_sidecar.name
        shutil.copyfile(manifest_path, published_manifest)
        shutil.copyfile(package, published_package)
        shutil.copyfile(package_sidecar, published_sidecar)

    return {
        **manifest,
        "outputs": {
            "package": str(published_package),
            "package_sha256": sha256(published_package),
            "sidecar": str(published_sidecar),
            "manifest": str(published_manifest),
        },
    }
