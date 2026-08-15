from __future__ import annotations

import tempfile
from pathlib import Path

from .ext4_read import dump_file
from .firmware_archive import extract_inner_image, extract_official_outer
from .firmware_targets import VIEWPT_221_FIRMWARE
from .font_embed import write_embedded_fonts
from .model import FirmwareIdentity
from .util import FirmwareToolError, require_commands, require_hash, run, sha256


def build_ui_binary(
    source: Path,
    output: Path,
    *,
    ui_source: Path,
    firmware: FirmwareIdentity = VIEWPT_221_FIRMWARE,
) -> dict[str, object]:
    """Cross-link the replacement UI against an exact supported runtime."""

    require_commands(("clang", "debugfs"))
    require_hash(source, firmware.source_archive_sha256, "official archive")
    header = ui_source / "include" / "calf_ui.h"
    core_sources = (
        ui_source / "src" / "ui.c",
        ui_source / "src" / "ui_input.c",
        ui_source / "src" / "ui_render.c",
        ui_source / "src" / "ui_font.c",
        ui_source / "src" / "ui_locale.c",
        ui_source / "src" / "sha256.c",
    )
    fonts = (
        ("calf_font_noto_sans",
         ui_source / "assets" / "fonts" / "NotoSans-Regular.ttf"),
        ("calf_font_noto_symbols",
         ui_source / "assets" / "fonts" / "NotoSansSymbols-Regular.ttf"),
        ("calf_font_noto_symbols2",
         ui_source / "assets" / "fonts" / "NotoSansSymbols2-Regular.ttf"),
    )
    rasterizer = ui_source / "third_party" / "stb" / "stb_truetype.h"
    target_sources = (
        ui_source / "src" / "target.c",
        ui_source / "src" / "target_capture.c",
        ui_source / "src" / "target_gallery.c",
        ui_source / "src" / "target_platform.c",
        ui_source / "src" / "target_power.c",
        ui_source / "src" / "target_storage.c",
        ui_source / "src" / "target_update.c",
    )
    for required in (
        header,
        rasterizer,
        *(font for _, font in fonts),
        *core_sources,
        *target_sources,
    ):
        if not required.is_file():
            raise FirmwareToolError(f"replacement UI source is missing: {required}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="calf-ui-build-") as temp_name:
        temp = Path(temp_name)
        vpupdate, _ = extract_official_outer(source, temp)
        require_hash(vpupdate, firmware.vpupdate_sha256, "vpupdate.bin")
        rootfs = extract_inner_image(vpupdate, temp / "images", "rootfs.img")
        app = extract_inner_image(vpupdate, temp / "images", "app.img")
        require_hash(app, firmware.app_image_sha256, "app.img")

        sysroot_lib = temp / "rootfs-lib"
        app_lib = temp / "app-lib"
        libc = sysroot_lib / "libc-2.33.so"
        libdl = sysroot_lib / "libdl-2.33.so"
        libm = sysroot_lib / "libm-2.33.so"
        libpthread = sysroot_lib / "libpthread-2.33.so"
        librockit = app_lib / "librockit.so"
        dump_file(rootfs, "/lib/libc-2.33.so", libc)
        dump_file(rootfs, "/lib/libdl-2.33.so", libdl)
        dump_file(rootfs, "/lib/libm-2.33.so", libm)
        dump_file(rootfs, "/lib/libpthread-2.33.so", libpthread)
        dump_file(app, "/lib/librockit.so", librockit)

        staged = temp / "calf-ui-aarch64"
        font_source = temp / "ui_font_data.c"
        write_embedded_fonts(font_source, fonts)
        command = [
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
            f'-DCALF_FIRMWARE_VERSION="{firmware.version}"',
            f"-I{ui_source / 'include'}",
            "-nostdlib",
            "-fuse-ld=lld",
            "-Wl,-e,_start",
            "-Wl,--allow-shlib-undefined",
            "-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1",
            "-Wl,-rpath,/app/lib",
            f"-Wl,-rpath-link,{sysroot_lib}",
            *(str(core) for core in core_sources),
            str(font_source),
            *(str(target) for target in target_sources),
            f"-L{app_lib}",
            "-lrockit",
            str(libm),
            str(libpthread),
            str(libc),
            str(libdl),
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
        "status": "built; hardware validation required",
    }
