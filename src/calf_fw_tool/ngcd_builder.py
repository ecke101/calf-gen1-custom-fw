from __future__ import annotations

import hashlib
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from .ext4_read import dump_file
from .firmware_archive import extract_inner_image, extract_official_outer
from .firmware_targets import VIEWPT_221_FIRMWARE
from .model import FirmwareIdentity
from .util import FirmwareToolError, require_commands, require_hash, run, sha256


def build_ngcd_binary(
    source: Path,
    output: Path,
    *,
    ngcd_source: Path,
    build_time: str | None = None,
    firmware: FirmwareIdentity = VIEWPT_221_FIRMWARE,
) -> dict[str, object]:
    """Cross-link the C replacement daemon against the exact target libc."""

    require_commands(("clang", "debugfs", "llvm-ar"))
    require_hash(source, firmware.source_archive_sha256, "official archive")
    include = ngcd_source / "include"
    target_include = ngcd_source / "target_include"
    sources = tuple(
        ngcd_source / "src" / name
        for name in (
            "app.c",
            "api.c",
            "aac.c",
            "aac_decoder.c",
            "audio.c",
            "backlight.c",
            "backend_mock.c",
            "backend_target.c",
            "http.c",
            "json.c",
            "power.c",
            "profile.c",
            "system.c",
            "rockchip_graph.c",
            "raw_stack.c",
            "rockchip_display.c",
            "rockchip_encoder.c",
            "rockchip_image.c",
            "mp4.c",
            "mp4_reader.c",
            "rockchip_audio.c",
            "rockchip_playback.c",
            "rockchip_target.c",
            "imu.c",
            "storage.c",
            "wifi.c",
            "main.c",
            "target_start.c",
        )
    )
    aac_root = ngcd_source / "vendor" / "vo-aacenc"
    aac_sources = (
        aac_root / "common" / "cmnMemory.c",
        *sorted((aac_root / "aacenc" / "basic_op").glob("*.c")),
        *sorted((aac_root / "aacenc" / "src").glob("*.c")),
    )
    xaac_root = ngcd_source / "vendor" / "libxaac"
    xaac_sources = (
        *sorted((xaac_root / "common").glob("*.c")),
        *sorted((xaac_root / "decoder").glob("*.c")),
        *sorted((xaac_root / "decoder" / "drc_src").glob("*.c")),
        *sorted((xaac_root / "decoder" / "armv8").glob("*.c")),
        *sorted((xaac_root / "decoder" / "armv8").glob("*.s")),
    )
    sources = (*sources, *aac_sources)
    required = (
        include / "ngcd.h",
        include / "ngcd_imu.h",
        target_include / "pthread.h",
        target_include / "math.h",
        target_include / "memory.h",
        target_include / "setjmp.h",
        target_include / "stdio.h",
        target_include / "sys" / "socket.h",
        *sources,
        *xaac_sources,
    )
    for path in required:
        if not path.is_file():
            raise FirmwareToolError(f"replacement ngcd source is missing: {path}")

    identity_hash = hashlib.sha256()
    identity_files = sorted(
        (
            path
            for directory in ("include", "src", "target_include", "vendor")
            for path in (ngcd_source / directory).rglob("*")
            if path.suffix in {".c", ".h", ".s"}
        ),
        key=lambda path: str(path.relative_to(ngcd_source)),
    )
    for path in identity_files:
        identity_hash.update(str(path.relative_to(ngcd_source)).encode("utf-8"))
        identity_hash.update(b"\0")
        identity_hash.update(path.read_bytes())
        identity_hash.update(b"\0")
    build_id = identity_hash.hexdigest()[:12]
    if build_time is None:
        build_time = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    resource_result = run(["clang", "--print-resource-dir"])
    resource_include = Path(resource_result.stdout.strip()) / "include"
    if not resource_include.is_dir():
        raise FirmwareToolError(
            f"clang resource include directory is missing: {resource_include}"
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="calf-ngcd-build-") as temp_name:
        temp = Path(temp_name)
        vpupdate, _ = extract_official_outer(source, temp)
        require_hash(vpupdate, firmware.vpupdate_sha256, "vpupdate.bin")
        rootfs = extract_inner_image(vpupdate, temp / "images", "rootfs.img")
        libc = temp / "libc-2.33.so"
        libdl = temp / "libdl-2.33.so"
        libm = temp / "libm-2.33.so"
        libpthread = temp / "libpthread-2.33.so"
        dump_file(rootfs, "/lib/libc-2.33.so", libc)
        dump_file(rootfs, "/lib/libdl-2.33.so", libdl)
        dump_file(rootfs, "/lib/libm-2.33.so", libm)
        dump_file(rootfs, "/lib/libpthread-2.33.so", libpthread)

        staged = temp / "ngcd-aarch64"
        target_flags = [
            "clang",
            "--target=aarch64-linux-gnu",
            "-std=c11",
            "-Os",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-builtin",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-Wno-misleading-indentation",
            "-Wno-unused-but-set-variable",
            "-D__unused=",
            "-D_GNU_SOURCE",
            "-DUSE_DEFAULT_STDLIB",
            "-DNDEBUG",
            f'-DNGCD_BUILD_ID="{build_id}"',
            f'-DNGCD_BUILD_TIME="{build_time}"',
            "-nostdinc",
            "-isystem",
            str(resource_include),
            f"-I{target_include}",
            f"-I{include}",
            f"-I{aac_root / 'common' / 'include'}",
            f"-I{aac_root / 'aacenc' / 'basic_op'}",
            f"-I{aac_root / 'aacenc' / 'inc'}",
            f"-I{xaac_root / 'common'}",
            f"-I{xaac_root / 'decoder'}",
            f"-I{xaac_root / 'decoder' / 'drc_src'}",
            f"-I{xaac_root / 'decoder' / 'armv8'}",
        ]
        xaac_objects = []
        for index, path in enumerate(xaac_sources):
            object_path = temp / f"xaac-{index:03d}.o"
            command = [
                *target_flags,
                "-march=armv8-a",
                "-DARMV8",
                "-fwrapv",
                "-w",
                "-c",
                str(path),
                "-o",
                str(object_path),
            ]
            run(command)
            xaac_objects.append(object_path)
        xaac_library = temp / "libxaacdec.a"
        run(["llvm-ar", "rcs", str(xaac_library),
             *(str(path) for path in xaac_objects)])

        command = [
            *target_flags,
            "-nostdlib",
            "-fuse-ld=lld",
            "-Wl,-e,_start",
            "-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1",
            *(str(path) for path in sources),
            str(xaac_library),
            str(libc),
            str(libdl),
            str(libm),
            str(libpthread),
            "-o",
            str(staged),
        ]
        run(command)
        output.write_bytes(staged.read_bytes())
        output.chmod(0o755)

    return {
        "architecture": "aarch64",
        "firmware_version": firmware.version,
        "output": str(output),
        "sha256": sha256(output),
        "status": (
            "built; target backend provides the native LCD sensor preview; "
            "stitched AVS output has lazy JPEG snapshots with EXIF and "
            "on-demand H.264/H.265 MP4 recording"
        ),
    }
