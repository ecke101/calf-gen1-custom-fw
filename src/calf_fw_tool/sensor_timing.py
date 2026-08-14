from __future__ import annotations

from pathlib import Path

from .util import FirmwareToolError, require_commands, run, sha256

SENSOR_TIMING_FILESYSTEM_PATH = "/bin/calf-sensor-timing"
SENSOR_TIMING_RUNTIME_PATH = "/app/bin/calf-sensor-timing"


# This helper is intentionally tied to the exact CALF 2.2.1 media graph. The
# fixed subdevice nodes and master roles were verified on hardware: subdev2 is
# the internal master/XVS owner and subdev7 is the external master. It has no
# libc dependency and uses only AArch64 Linux syscalls and fixed V4L2 ioctls.
_SENSOR_TIMING_SOURCE = r"""
typedef unsigned long size_t;

#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC 0x80000

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_NANOSLEEP 101

#define VIDIOC_G_CTRL 0xc008561bUL
#define VIDIOC_S_CTRL 0xc008561cUL
#define VIDIOC_SUBDEV_S_FRAME_INTERVAL 0xc0305616UL
#define RKMODULE_SET_QUICK_STREAM 0x400456caUL
#define V4L2_CID_EXPOSURE 0x00980911U
#define V4L2_CID_VBLANK 0x009e0901U

#define SENSOR_HEIGHT 3040
#define EXPOSURE_MARGIN 20
#define BASELINE_EXPOSURE 3150

struct timespec {
    long seconds;
    long nanoseconds;
};

struct v4l2_fract {
    unsigned int numerator;
    unsigned int denominator;
};

struct v4l2_subdev_frame_interval {
    unsigned int pad;
    struct v4l2_fract interval;
    unsigned int reserved[9];
};

struct v4l2_control {
    unsigned int id;
    int value;
};

static long syscall1(long number, long argument0)
{
    register long x0 __asm__("x0") = argument0;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static long syscall2(long number, long argument0, long argument1)
{
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x8 __asm__("x8") = number;
    __asm__ volatile(
        "svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

static long syscall3(
    long number, long argument0, long argument1, long argument2)
{
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x2 __asm__("x2") = argument2;
    register long x8 __asm__("x8") = number;
    __asm__ volatile(
        "svc 0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc");
    return x0;
}

static long syscall4(
    long number, long argument0, long argument1, long argument2,
    long argument3)
{
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x2 __asm__("x2") = argument2;
    register long x3 __asm__("x3") = argument3;
    register long x8 __asm__("x8") = number;
    __asm__ volatile(
        "svc 0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc");
    return x0;
}

static size_t text_length(const char *value)
{
    size_t length = 0;
    while (value[length])
        ++length;
    return length;
}

static void write_text(int descriptor, const char *value)
{
    syscall3(SYS_WRITE, descriptor, (long)value, (long)text_length(value));
}

static int parse_interval(const char *value, unsigned int *numerator,
                          unsigned int *denominator)
{
    *numerator = 1;
    if (value[0] == '2' && value[1] == '\0') *denominator = 2;
    else if (value[0] == '4' && value[1] == '\0') *denominator = 4;
    else if (value[0] == '8' && value[1] == '\0') *denominator = 8;
    else if (value[0] == '1' && value[1] == '5' && value[2] == '\0')
        *denominator = 15;
    else if (value[0] == '3' && value[1] == '0' && value[2] == '\0')
        *denominator = 30;
    else if ((value[0] == '1' || value[0] == '2' || value[0] == '4') &&
             value[1] == 's' && value[2] == '\0') {
        *numerator = (unsigned int)(value[0] - '0');
        *denominator = 1;
    }
    else return -1;
    return 0;
}

static long set_frame_interval(long descriptor, unsigned int numerator,
                               unsigned int denominator)
{
    struct v4l2_subdev_frame_interval value;
    int index;
    value.pad = 0;
    value.interval.numerator = numerator;
    value.interval.denominator = denominator;
    for (index = 0; index < 9; ++index)
        value.reserved[index] = 0;
    return syscall3(
        SYS_IOCTL, descriptor, VIDIOC_SUBDEV_S_FRAME_INTERVAL, (long)&value);
}

static long get_control(long descriptor, unsigned int id, int *value)
{
    struct v4l2_control control;
    long result;
    control.id = id;
    control.value = 0;
    result = syscall3(SYS_IOCTL, descriptor, VIDIOC_G_CTRL, (long)&control);
    if (result >= 0)
        *value = control.value;
    return result;
}

static long set_control(long descriptor, unsigned int id, int value)
{
    struct v4l2_control control;
    control.id = id;
    control.value = value;
    return syscall3(SYS_IOCTL, descriptor, VIDIOC_S_CTRL, (long)&control);
}

static long set_quick_stream(long descriptor, unsigned int enabled)
{
    return syscall3(
        SYS_IOCTL, descriptor, RKMODULE_SET_QUICK_STREAM, (long)&enabled);
}

static void pause_between_stages(void)
{
    struct timespec duration;
    duration.seconds = 0;
    duration.nanoseconds = 20000000;
    syscall2(SYS_NANOSLEEP, (long)&duration, 0);
}

static int rearm_streams(long internal_descriptor, long external_descriptor)
{
    if (set_quick_stream(internal_descriptor, 0) < 0)
        return -1;
    pause_between_stages();
    if (set_quick_stream(external_descriptor, 0) < 0)
        return -1;
    pause_between_stages();
    if (set_quick_stream(external_descriptor, 1) < 0)
        return -1;
    pause_between_stages();
    if (set_quick_stream(internal_descriptor, 1) < 0)
        return -1;
    return 0;
}

static void recover_streams(long internal_descriptor, long external_descriptor)
{
    set_quick_stream(external_descriptor, 1);
    pause_between_stages();
    set_quick_stream(internal_descriptor, 1);
}

static int configure_pair(
    long internal_descriptor, long external_descriptor,
    unsigned int numerator, unsigned int denominator)
{
    int internal_vblank;
    int external_vblank;
    int internal_readback;
    int external_readback;
    int exposure;
    if (set_frame_interval(internal_descriptor, numerator, denominator) < 0 ||
        set_frame_interval(external_descriptor, numerator, denominator) < 0)
        return -1;
    if (get_control(
            internal_descriptor, V4L2_CID_VBLANK, &internal_vblank) < 0 ||
        get_control(
            external_descriptor, V4L2_CID_VBLANK, &external_vblank) < 0 ||
        internal_vblank != external_vblank)
        return -1;
    exposure = numerator == 1 && denominator == 30
        ? BASELINE_EXPOSURE
        : SENSOR_HEIGHT + internal_vblank - EXPOSURE_MARGIN;
    if (exposure < 8 ||
        set_control(internal_descriptor, V4L2_CID_EXPOSURE, exposure) < 0 ||
        set_control(external_descriptor, V4L2_CID_EXPOSURE, exposure) < 0)
        return -1;
    if (get_control(
            internal_descriptor, V4L2_CID_EXPOSURE, &internal_readback) < 0 ||
        get_control(
            external_descriptor, V4L2_CID_EXPOSURE, &external_readback) < 0 ||
        internal_readback != exposure || external_readback != exposure)
        return -1;
    return 0;
}

static void restore_baseline(
    long internal_descriptor, long external_descriptor)
{
    configure_pair(internal_descriptor, external_descriptor, 1, 30);
    recover_streams(internal_descriptor, external_descriptor);
}

static void program_start(long *stack_pointer)
    __attribute__((used, noinline, noreturn));

static void program_start(long *stack_pointer)
{
    static const char internal_path[] = "/dev/v4l-subdev2";
    static const char external_path[] = "/dev/v4l-subdev7";
    char **arguments;
    long internal_descriptor = -1;
    long external_descriptor = -1;
    int argument_count;
    unsigned int numerator;
    unsigned int denominator;
    int status = 1;

    argument_count = (int)stack_pointer[0];
    arguments = (char **)&stack_pointer[1];
    if (argument_count != 2 ||
        parse_interval(arguments[1], &numerator, &denominator) != 0) {
        write_text(2, "usage: calf-sensor-timing {4s|2s|1s|2|4|8|15|30}\n");
        goto finished;
    }

    internal_descriptor = syscall4(
        SYS_OPENAT, AT_FDCWD, (long)internal_path, O_RDWR | O_CLOEXEC, 0);
    external_descriptor = syscall4(
        SYS_OPENAT, AT_FDCWD, (long)external_path, O_RDWR | O_CLOEXEC, 0);
    if (internal_descriptor < 0 || external_descriptor < 0) {
        write_text(2, "sensor timing: cannot open both fixed sensor nodes\n");
        goto finished;
    }
    if (configure_pair(internal_descriptor, external_descriptor,
                       numerator, denominator) < 0) {
        write_text(2, "sensor timing: paired configuration failed\n");
        restore_baseline(internal_descriptor, external_descriptor);
        goto finished;
    }
    if (rearm_streams(internal_descriptor, external_descriptor) < 0) {
        write_text(2, "sensor timing: coordinated XVS rearm failed\n");
        restore_baseline(internal_descriptor, external_descriptor);
        goto finished;
    }
    write_text(1, "sensor timing: paired configuration applied\n");
    status = 0;

finished:
    if (external_descriptor >= 0)
        syscall1(SYS_CLOSE, external_descriptor);
    if (internal_descriptor >= 0)
        syscall1(SYS_CLOSE, internal_descriptor);
    syscall1(SYS_EXIT, status);
    for (;;) {}
}

void _start(void) __attribute__((naked, noreturn));

void _start(void)
{
    __asm__ volatile("mov x0, sp\nb program_start");
}
"""


def build_sensor_timing(destination: Path) -> dict[str, object]:
    require_commands(("clang", "ld.lld", "readelf"))
    source = destination.with_suffix(".c")
    source.write_text(_SENSOR_TIMING_SOURCE.strip() + "\n", encoding="ascii")
    run(
        [
            "clang",
            "--target=aarch64-linux-gnu",
            "-march=armv8-a",
            "-Os",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-builtin",
            "-fno-ident",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-nostdlib",
            "-fuse-ld=lld",
            "-Wl,-e,_start",
            "-Wl,--build-id=none",
            "-o",
            str(destination),
            str(source),
        ]
    )
    header = run(["readelf", "-h", str(destination)]).stdout
    if "Machine:                           AArch64" not in header:
        raise FirmwareToolError("sensor timing helper is not AArch64")
    destination.chmod(0o755)
    return {
        "target": SENSOR_TIMING_FILESYSTEM_PATH,
        "runtime_path": SENSOR_TIMING_RUNTIME_PATH,
        "sha256": sha256(destination),
        "description": (
            "Apply matching 4/2/1 second and 2/4/8/15/30 fps frame intervals "
            "and exposure controls to both fixed IMX577 subdevices, then rearm the "
            "external master before the internal-master XVS source; restore "
            "the verified 30 fps baseline on failure."
        ),
    }
