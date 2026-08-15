from __future__ import annotations

import tempfile
from pathlib import Path

from .util import FirmwareToolError, require_commands, run, sha256

HASH_HELPER_NAME = "calf-sha256"

_HASH_TOOL_SOURCE = r"""
#include "calf_sha256.h"

typedef unsigned char u8;
typedef unsigned long usize;

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_CLOEXEC 02000000
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT 93

static long syscall1(long number, long argument0)
{
    register long x0 __asm__("x0") = argument0;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static long syscall3(long number, long argument0, long argument1, long argument2)
{
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x2 __asm__("x2") = argument2;
    register long x8 __asm__("x8") = number;
    __asm__ volatile(
        "svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

static long syscall4(long number, long argument0, long argument1,
                     long argument2, long argument3)
{
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x2 __asm__("x2") = argument2;
    register long x3 __asm__("x3") = argument3;
    register long x8 __asm__("x8") = number;
    __asm__ volatile(
        "svc 0" : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}

static usize text_length(const char *text)
{
    usize length = 0;
    while(text[length] != '\0') ++length;
    return length;
}

static int write_all(long descriptor, const u8 *buffer, usize length)
{
    usize done = 0;
    while(done < length) {
        long amount = syscall3(SYS_WRITE, descriptor,
                               (long)(buffer + done), (long)(length - done));
        if(amount <= 0) return -1;
        done += (usize)amount;
    }
    return 0;
}

static void fail(const char *message, long status)
{
    (void)write_all(2, (const u8 *)message, text_length(message));
    (void)syscall1(SYS_EXIT, status);
    for(;;) { }
}

static void program_start(long *stack_pointer)
    __attribute__((used, noinline, noreturn));

static void program_start(long *stack_pointer)
{
    static const char digits[] = "0123456789abcdef";
    long argc = stack_pointer[0];
    char **argv = (char **)(stack_pointer + 1);
    calf_sha256_t context;
    u8 buffer[16384];
    u8 digest[32];
    char output[65];
    long descriptor;
    usize index;

    if(argc != 2) fail("usage: calf-sha256 FILE\n", 2);
    descriptor = syscall4(SYS_OPENAT, AT_FDCWD, (long)argv[1],
                          O_RDONLY | O_CLOEXEC, 0);
    if(descriptor < 0) fail("calf-sha256: cannot open file\n", 1);
    calf_sha256_init(&context);
    for(;;) {
        long amount = syscall3(SYS_READ, descriptor, (long)buffer,
                               (long)sizeof(buffer));
        if(amount < 0) fail("calf-sha256: read failed\n", 1);
        if(amount == 0) break;
        calf_sha256_update(&context, buffer, (usize)amount);
    }
    if(syscall1(SYS_CLOSE, descriptor) != 0)
        fail("calf-sha256: close failed\n", 1);
    calf_sha256_final(&context, digest);
    for(index = 0; index < sizeof(digest); ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 15];
    }
    output[64] = '\n';
    if(write_all(1, (const u8 *)output, sizeof(output)) != 0)
        fail("calf-sha256: write failed\n", 1);
    (void)syscall1(SYS_EXIT, 0);
    for(;;) { }
}

void _start(void) __attribute__((naked, noreturn));

void _start(void)
{
    __asm__ volatile("mov x0, sp\nb program_start");
}
"""


def build_hash_helper(
    destination: Path, *, ui_source: Path
) -> dict[str, object]:
    """Build a no-libc AArch64 SHA-256 tool for minimal stock BusyBox."""

    require_commands(("clang",))
    sha_source = ui_source / "src" / "sha256.c"
    sha_header = ui_source / "include" / "calf_sha256.h"
    if not sha_source.is_file() or not sha_header.is_file():
        raise FirmwareToolError("UI SHA-256 source is missing")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="calf-sha256-build-") as name:
        temporary = Path(name)
        tool_source = temporary / "calf_sha256_tool.c"
        staged = temporary / HASH_HELPER_NAME
        tool_source.write_text(_HASH_TOOL_SOURCE, encoding="ascii")
        run(
            [
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
                f"-I{ui_source / 'include'}",
                "-nostdlib",
                "-static",
                "-fuse-ld=lld",
                "-Wl,-e,_start",
                str(tool_source),
                str(sha_source),
                "-o",
                str(staged),
            ]
        )
        destination.write_bytes(staged.read_bytes())
        destination.chmod(0o755)
    return {
        "architecture": "aarch64",
        "runtime_dependencies": "none",
        "sha256": sha256(destination),
        "output": str(destination),
    }
