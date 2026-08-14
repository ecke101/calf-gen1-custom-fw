from __future__ import annotations

from pathlib import Path

from .util import FirmwareToolError, require_commands, run, sha256

RAW_DNG_FILESYSTEM_PATH = "/bin/calf-raw2dng"
RAW_DNG_RUNTIME_PATH = "/app/bin/calf-raw2dng"


# This product includes DNG technology under license by Adobe.
#
# The converter is deliberately small and tied to the validated Rockchip
# capture header. It streams one packed RAW12 row from each exposure at a
# time, so even a 24-frame stack does not need enough RAM to hold a full
# sensor frame.
_RAW_DNG_SOURCE = r"""
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long usize;
typedef long ssize;

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_CLOEXEC 02000000
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_FSYNC 82
#define SYS_EXIT 93

#define RAW_HEADER_SIZE 128U
#define TIFF_BUFFER_SIZE 2048U
#define ROW_BUFFER_SIZE 8192U
#define MAX_RAW_INPUTS 24U
#define SENSOR_BLACK_LEVEL 256U
#define SENSOR_WHITE_LEVEL 4095U
#define DNG_ENTRY_COUNT 33U

#define TIFF_BYTE 1U
#define TIFF_ASCII 2U
#define TIFF_SHORT 3U
#define TIFF_LONG 4U
#define TIFF_RATIONAL 5U
#define TIFF_SRATIONAL 10U

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

static void put16(u8 *destination, u16 value)
{
    destination[0] = (u8)value;
    destination[1] = (u8)(value >> 8);
}

static void put32(u8 *destination, u32 value)
{
    destination[0] = (u8)value;
    destination[1] = (u8)(value >> 8);
    destination[2] = (u8)(value >> 16);
    destination[3] = (u8)(value >> 24);
}

static u16 get16(const u8 *source)
{
    return (u16)((u16)source[0] | ((u16)source[1] << 8));
}

static u32 get32(const u8 *source)
{
    return (u32)source[0] | ((u32)source[1] << 8) |
           ((u32)source[2] << 16) | ((u32)source[3] << 24);
}

static usize text_length(const char *text)
{
    usize length = 0;
    while (text[length] != '\0')
        ++length;
    return length;
}

static void error_text(const char *text)
{
    (void)syscall3(SYS_WRITE, 2, (long)text, (long)text_length(text));
}

static int read_exact(long descriptor, u8 *buffer, usize length)
{
    usize done = 0;
    while (done < length) {
        long amount = syscall3(
            SYS_READ, descriptor, (long)(buffer + done), (long)(length - done));
        if (amount <= 0)
            return -1;
        done += (usize)amount;
    }
    return 0;
}

static int write_all(long descriptor, const u8 *buffer, usize length)
{
    usize done = 0;
    while (done < length) {
        long amount = syscall3(
            SYS_WRITE, descriptor, (long)(buffer + done), (long)(length - done));
        if (amount <= 0)
            return -1;
        done += (usize)amount;
    }
    return 0;
}

static int text_equal_at(const u8 *data, usize length, usize offset,
                         const char *wanted)
{
    usize index = 0;
    while (wanted[index] != '\0') {
        if (offset + index >= length || data[offset + index] != (u8)wanted[index])
            return 0;
        ++index;
    }
    return 1;
}

static int parse_decimal(const u8 *data, usize length, usize *offset, u32 *value)
{
    u32 parsed = 0;
    int digits = 0;
    while (*offset < length && data[*offset] >= '0' && data[*offset] <= '9') {
        if (parsed > 100000000U)
            return -1;
        parsed = parsed * 10U + (u32)(data[*offset] - '0');
        ++*offset;
        digits = 1;
    }
    if (!digits || parsed == 0)
        return -1;
    *value = parsed;
    return 0;
}

static void load_neutral(const char *metadata_path, u32 neutral[6])
{
    u8 metadata[2048];
    long descriptor;
    long amount;
    usize offset;
    u32 red;
    u32 green0;
    u32 green1;
    u32 blue;

    /* D65 calibration fallback, as 1/R, 1, 1/B. */
    neutral[0] = 1000000U;
    neutral[1] = 2202950U;
    neutral[2] = 1U;
    neutral[3] = 1U;
    neutral[4] = 1000000U;
    neutral[5] = 1457640U;

    descriptor = syscall4(
        SYS_OPENAT, AT_FDCWD, (long)metadata_path, O_RDONLY | O_CLOEXEC, 0);
    if (descriptor < 0)
        return;
    amount = syscall3(SYS_READ, descriptor, (long)metadata, sizeof(metadata));
    (void)syscall1(SYS_CLOSE, descriptor);
    if (amount <= 0)
        return;

    for (offset = 0; offset + 8U < (usize)amount; ++offset) {
        usize cursor;
        if (!text_equal_at(metadata, (usize)amount, offset, "awbGain["))
            continue;
        cursor = offset + 8U;
        if (parse_decimal(metadata, (usize)amount, &cursor, &red) != 0 ||
            cursor >= (usize)amount || metadata[cursor++] != '_' ||
            parse_decimal(metadata, (usize)amount, &cursor, &green0) != 0 ||
            cursor >= (usize)amount || metadata[cursor++] != '_' ||
            parse_decimal(metadata, (usize)amount, &cursor, &green1) != 0 ||
            cursor >= (usize)amount || metadata[cursor++] != '_' ||
            parse_decimal(metadata, (usize)amount, &cursor, &blue) != 0)
            return;
        (void)green1;
        neutral[0] = green0;
        neutral[1] = red;
        neutral[2] = 1U;
        neutral[3] = 1U;
        neutral[4] = green0;
        neutral[5] = blue;
        return;
    }
}

static u32 append_bytes(u8 *buffer, u32 *used, const u8 *source, u32 length)
{
    u32 offset = *used;
    u32 index;
    for (index = 0; index < length; ++index)
        buffer[offset + index] = source[index];
    *used += length;
    while ((*used & 3U) != 0U)
        buffer[(*used)++] = 0;
    return offset;
}

static u32 append_text(u8 *buffer, u32 *used, const char *text)
{
    return append_bytes(buffer, used, (const u8 *)text,
                        (u32)text_length(text) + 1U);
}

static u32 append_u32s(u8 *buffer, u32 *used, const u32 *values, u32 count)
{
    u32 offset = *used;
    u32 index;
    for (index = 0; index < count; ++index)
        put32(buffer + offset + index * 4U, values[index]);
    *used += count * 4U;
    return offset;
}

static u32 append_u16s(u8 *buffer, u32 *used, const u16 *values, u32 count)
{
    u32 offset = *used;
    u32 index;
    for (index = 0; index < count; ++index)
        put16(buffer + offset + index * 2U, values[index]);
    *used += count * 2U;
    while ((*used & 3U) != 0U)
        buffer[(*used)++] = 0;
    return offset;
}

static void add_entry(u8 *buffer, u32 *entry_index, u16 tag, u16 type,
                      u32 count, u32 value)
{
    u8 *entry = buffer + 10U + *entry_index * 12U;
    put16(entry, tag);
    put16(entry + 2, type);
    put32(entry + 4, count);
    put32(entry + 8, value);
    ++*entry_index;
}

static int same_raw_layout(const u8 *first, const u8 *candidate)
{
    return get16(first) == get16(candidate) &&
           get16(first + 2) == get16(candidate + 2) &&
           get16(first + 8) == get16(candidate + 8) &&
           get16(first + 10) == get16(candidate + 10) &&
           first[12] == candidate[12] && first[13] == candidate[13] &&
           first[14] == candidate[14] && first[16] == candidate[16] &&
           get16(first + 17) == get16(candidate + 17) &&
           get16(first + 19) == get16(candidate + 19);
}

static int convert(char *const *input_paths, u32 input_count,
                   const char *metadata_path,
                   const char *output_path)
{
    static const u32 matrix_d50[18] = {
        840125U, 1000000U, (u32)-223951, 1000000U, (u32)-93097, 1000000U,
        (u32)-176446, 1000000U, 1044756U, 1000000U, 151951U, 1000000U,
        (u32)-21706, 1000000U, 198365U, 1000000U, 455620U, 1000000U
    };
    static const u32 matrix_d65[18] = {
        725769U, 1000000U, (u32)-140006, 1000000U, (u32)-88057, 1000000U,
        (u32)-292231, 1000000U, 1144945U, 1000000U, 121977U, 1000000U,
        (u32)-34170, 1000000U, 199693U, 1000000U, 476498U, 1000000U
    };
    static const u8 cfa_patterns[4][4] = {
        {2, 1, 1, 0}, {1, 2, 0, 1}, {1, 0, 2, 1}, {0, 1, 1, 2}
    };
    static u8 tiff[TIFF_BUFFER_SIZE];
    static u8 packed[ROW_BUFFER_SIZE];
    static u8 unpacked[ROW_BUFFER_SIZE];
    static u32 accumulated[ROW_BUFFER_SIZE / 2U];
    u8 raw_header[RAW_HEADER_SIZE];
    u8 candidate_header[RAW_HEADER_SIZE];
    u16 black_values[4];
    u32 neutral[6];
    u32 values[4];
    u32 entry_index = 0;
    u32 aux_offset = 8U + 2U + DNG_ENTRY_COUNT * 12U + 4U;
    u32 make_offset;
    u32 model_offset;
    u32 software_offset;
    u32 unique_model_offset;
    u32 black_offset;
    u32 crop_origin_offset;
    u32 crop_size_offset;
    u32 matrix1_offset;
    u32 matrix2_offset;
    u32 neutral_offset;
    u32 active_area_offset;
    u32 pixel_offset;
    u32 pixel_bytes;
    u32 width;
    u32 height;
    u32 stride;
    u32 effective_stride;
    u32 row;
    u32 input_index;
    long inputs[MAX_RAW_INPUTS];
    long output = -1;
    long file_size;
    int status = -1;

    for (input_index = 0; input_index < MAX_RAW_INPUTS; ++input_index)
        inputs[input_index] = -1;
    if (input_count != 1U && input_count != 2U &&
        input_count != 4U && input_count != 8U &&
        input_count != 16U && input_count != 24U) {
        error_text("raw2dng: input count must be 1, 2, 4, 8, 16, or 24\n");
        goto finished;
    }
    for (input_index = 0; input_index < input_count; ++input_index) {
        u8 *header = input_index == 0U ? raw_header : candidate_header;
        inputs[input_index] = syscall4(
            SYS_OPENAT, AT_FDCWD, (long)input_paths[input_index],
            O_RDONLY | O_CLOEXEC, 0);
        if (inputs[input_index] < 0 ||
            read_exact(inputs[input_index], header, RAW_HEADER_SIZE) != 0) {
            error_text("raw2dng: cannot read input\n");
            goto finished;
        }
        if (input_index == 0U) {
            width = get16(raw_header + 8);
            height = get16(raw_header + 10);
            stride = get16(raw_header + 17);
            effective_stride = get16(raw_header + 19);
        } else if (!same_raw_layout(raw_header, candidate_header)) {
            error_text("raw2dng: RAW stack layouts differ\n");
            goto finished;
        }
        file_size = syscall3(SYS_LSEEK, inputs[input_index], 0, 2);
        if (get16(header) != 0x8080U || get16(header + 2) != 128U ||
            width == 0U || height == 0U || (width & 1U) != 0U ||
            header[12] != 12U || header[13] > 3U ||
            header[14] != 1U || header[16] != 0U ||
            effective_stride != width * 3U / 2U || stride < effective_stride ||
            stride > ROW_BUFFER_SIZE || width * 2U > ROW_BUFFER_SIZE ||
            file_size != (long)(RAW_HEADER_SIZE + stride * height) ||
            syscall3(SYS_LSEEK, inputs[input_index], RAW_HEADER_SIZE, 0) < 0) {
            error_text("raw2dng: unsupported or invalid Rockchip RAW12 file\n");
            goto finished;
        }
    }
    pixel_bytes = width * height * 2U;
    load_neutral(metadata_path, neutral);

    put16(tiff, 0x4949U);
    put16(tiff + 2, 42U);
    put32(tiff + 4, 8U);
    put16(tiff + 8, DNG_ENTRY_COUNT);
    put32(tiff + 10U + DNG_ENTRY_COUNT * 12U, 0U);

    make_offset = append_text(tiff, &aux_offset, "CALF");
    model_offset = append_text(tiff, &aux_offset, "VIEWPT VP415");
    software_offset = append_text(tiff, &aux_offset, "CALF raw capture 1.0");
    unique_model_offset = append_text(
        tiff, &aux_offset, "CALF VIEWPT VP415 IMX577");
    black_values[0] = SENSOR_BLACK_LEVEL;
    black_values[1] = SENSOR_BLACK_LEVEL;
    black_values[2] = SENSOR_BLACK_LEVEL;
    black_values[3] = SENSOR_BLACK_LEVEL;
    black_offset = append_u16s(tiff, &aux_offset, black_values, 4U);
    values[0] = 0U; values[1] = 0U;
    crop_origin_offset = append_u32s(tiff, &aux_offset, values, 2U);
    values[0] = width; values[1] = height;
    crop_size_offset = append_u32s(tiff, &aux_offset, values, 2U);
    matrix1_offset = append_u32s(tiff, &aux_offset, matrix_d50, 18U);
    matrix2_offset = append_u32s(tiff, &aux_offset, matrix_d65, 18U);
    neutral_offset = append_u32s(tiff, &aux_offset, neutral, 6U);
    values[0] = 0U; values[1] = 0U; values[2] = height; values[3] = width;
    active_area_offset = append_u32s(tiff, &aux_offset, values, 4U);
    pixel_offset = aux_offset;

    add_entry(tiff, &entry_index, 254, TIFF_LONG, 1, 0);
    add_entry(tiff, &entry_index, 256, TIFF_LONG, 1, width);
    add_entry(tiff, &entry_index, 257, TIFF_LONG, 1, height);
    add_entry(tiff, &entry_index, 258, TIFF_SHORT, 1, 16);
    add_entry(tiff, &entry_index, 259, TIFF_SHORT, 1, 1);
    add_entry(tiff, &entry_index, 262, TIFF_SHORT, 1, 32803);
    add_entry(tiff, &entry_index, 271, TIFF_ASCII, 5, make_offset);
    add_entry(tiff, &entry_index, 272, TIFF_ASCII, 13, model_offset);
    add_entry(tiff, &entry_index, 273, TIFF_LONG, 1, pixel_offset);
    add_entry(tiff, &entry_index, 274, TIFF_SHORT, 1, 1);
    add_entry(tiff, &entry_index, 277, TIFF_SHORT, 1, 1);
    add_entry(tiff, &entry_index, 278, TIFF_LONG, 1, height);
    add_entry(tiff, &entry_index, 279, TIFF_LONG, 1, pixel_bytes);
    add_entry(tiff, &entry_index, 284, TIFF_SHORT, 1, 1);
    add_entry(tiff, &entry_index, 305, TIFF_ASCII, 21, software_offset);
    add_entry(tiff, &entry_index, 33421, TIFF_SHORT, 2, 0x00020002U);
    add_entry(tiff, &entry_index, 33422, TIFF_BYTE, 4,
              get32(cfa_patterns[raw_header[13]]));
    add_entry(tiff, &entry_index, 50706, TIFF_BYTE, 4, 0x00000401U);
    add_entry(tiff, &entry_index, 50707, TIFF_BYTE, 4, 0x00000101U);
    add_entry(tiff, &entry_index, 50708, TIFF_ASCII, 25, unique_model_offset);
    add_entry(tiff, &entry_index, 50710, TIFF_BYTE, 3, 0x00020100U);
    add_entry(tiff, &entry_index, 50711, TIFF_SHORT, 1, 1);
    add_entry(tiff, &entry_index, 50713, TIFF_SHORT, 2, 0x00020002U);
    add_entry(tiff, &entry_index, 50714, TIFF_SHORT, 4, black_offset);
    add_entry(tiff, &entry_index, 50717, TIFF_LONG, 1, SENSOR_WHITE_LEVEL);
    add_entry(tiff, &entry_index, 50719, TIFF_LONG, 2, crop_origin_offset);
    add_entry(tiff, &entry_index, 50720, TIFF_LONG, 2, crop_size_offset);
    add_entry(tiff, &entry_index, 50721, TIFF_SRATIONAL, 9, matrix1_offset);
    add_entry(tiff, &entry_index, 50722, TIFF_SRATIONAL, 9, matrix2_offset);
    add_entry(tiff, &entry_index, 50728, TIFF_RATIONAL, 3, neutral_offset);
    add_entry(tiff, &entry_index, 50778, TIFF_SHORT, 1, 23);
    add_entry(tiff, &entry_index, 50779, TIFF_SHORT, 1, 21);
    add_entry(tiff, &entry_index, 50829, TIFF_LONG, 4, active_area_offset);
    if (entry_index != DNG_ENTRY_COUNT || pixel_offset > TIFF_BUFFER_SIZE) {
        error_text("raw2dng: internal TIFF layout error\n");
        goto finished;
    }

    output = syscall4(SYS_OPENAT, AT_FDCWD, (long)output_path,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (output < 0 || write_all(output, tiff, pixel_offset) != 0) {
        error_text("raw2dng: cannot create output\n");
        goto finished;
    }
    for (row = 0; row < height; ++row) {
        u32 pixel;
        for (pixel = 0; pixel < width; ++pixel)
            accumulated[pixel] = 0U;
        for (input_index = 0; input_index < input_count; ++input_index) {
            u32 input_offset;
            pixel = 0U;
            if (read_exact(inputs[input_index], packed, stride) != 0) {
                error_text("raw2dng: truncated input row\n");
                goto finished;
            }
            for (input_offset = 0; input_offset < effective_stride;
                 input_offset += 3U) {
                u16 first = (u16)((u16)packed[input_offset] |
                                  ((u16)(packed[input_offset + 1U] & 15U) << 8));
                u16 second = (u16)(((u16)packed[input_offset + 1U] >> 4) |
                                   ((u16)packed[input_offset + 2U] << 4));
                if (input_count == 1U) {
                    accumulated[pixel] = first;
                    accumulated[pixel + 1U] = second;
                } else {
                    if (first > SENSOR_BLACK_LEVEL)
                        accumulated[pixel] += first - SENSOR_BLACK_LEVEL;
                    if (second > SENSOR_BLACK_LEVEL)
                        accumulated[pixel + 1U] += second - SENSOR_BLACK_LEVEL;
                }
                pixel += 2U;
            }
        }
        for (pixel = 0; pixel < width; ++pixel) {
            u32 value = accumulated[pixel];
            if (input_count != 1U)
                value += SENSOR_BLACK_LEVEL;
            if (value > SENSOR_WHITE_LEVEL)
                value = SENSOR_WHITE_LEVEL;
            put16(unpacked + pixel * 2U, (u16)value);
        }
        if (write_all(output, unpacked, width * 2U) != 0) {
            error_text("raw2dng: output write failed\n");
            goto finished;
        }
    }
    if (syscall1(SYS_FSYNC, output) < 0) {
        error_text("raw2dng: output sync failed\n");
        goto finished;
    }
    status = 0;

finished:
    if (output >= 0)
        (void)syscall1(SYS_CLOSE, output);
    for (input_index = 0; input_index < input_count &&
                          input_index < MAX_RAW_INPUTS; ++input_index) {
        if (inputs[input_index] >= 0)
            (void)syscall1(SYS_CLOSE, inputs[input_index]);
    }
    return status;
}

static void program_start(long *stack_pointer)
    __attribute__((used, noinline, noreturn));

static void program_start(long *stack_pointer)
{
    int argument_count = (int)stack_pointer[0];
    char **arguments = (char **)&stack_pointer[1];
    int status;
    if (argument_count != 4 && argument_count != 5 &&
        argument_count != 7 && argument_count != 11) {
        error_text("usage: calf-raw2dng RAW... METADATA.txt OUTPUT.dng\n");
        status = 2;
    } else {
        status = convert(arguments + 1, (u32)argument_count - 3U,
                         arguments[argument_count - 2],
                         arguments[argument_count - 1]) == 0 ? 0 : 1;
    }
    syscall1(SYS_EXIT, status);
    for (;;) {}
}

void _start(void) __attribute__((naked, noreturn));

void _start(void)
{
    __asm__ volatile("mov x0, sp\nb program_start");
}
"""


def build_raw_dng(destination: Path) -> dict[str, object]:
    require_commands(("clang", "ld.lld", "readelf"))
    source = destination.with_suffix(".c")
    source.write_text(_RAW_DNG_SOURCE.strip() + "\n", encoding="ascii")
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
        raise FirmwareToolError("RAW-to-DNG helper is not AArch64")
    destination.chmod(0o755)
    return {
        "target": RAW_DNG_FILESYSTEM_PATH,
        "runtime_path": RAW_DNG_RUNTIME_PATH,
        "sha256": sha256(destination),
        "description": (
            "Validate and optionally stack 1, 2, 4, 8, 16, or 24 Rockchip RAW12 "
            "captures, then stream them into uncompressed "
            "16-bit CFA DNG files with the calibrated IMX577 black/white "
            "levels, dual-illuminant color matrices, and captured white balance."
        ),
    }
