#include "ngcd_raw.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    RAW_HEADER_SIZE = 128,
    RKRAW_FORMAT_SIZE = 89,
    RKRAW_FRAME_INFO_SIZE = 84,
    RKRAW_BLOCK_HEADER_SIZE = 6,
    RKRAW_START_TAG = 0xff00,
    RKRAW_FORMAT_TAG = 0xff01,
    RKRAW_NORMAL_TAG = 0xff02,
    RKRAW_STATS_TAG = 0xff06,
    RKRAW_END_TAG = 0x00ff,
};

static uint16_t get_u16(const unsigned char *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t get_u32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void put_u16(unsigned char *data, uint16_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8U);
}

static void put_u32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8U);
    data[2] = (unsigned char)(value >> 16U);
    data[3] = (unsigned char)(value >> 24U);
}

static void put_float(unsigned char *data, float value)
{
    memcpy(data, &value, sizeof(value));
}

static uint32_t fourcc(char first, char second, char third, char fourth)
{
    return (uint32_t)(unsigned char)first |
           ((uint32_t)(unsigned char)second << 8U) |
           ((uint32_t)(unsigned char)third << 16U) |
           ((uint32_t)(unsigned char)fourth << 24U);
}

static uint32_t bayer_format(unsigned int bayer)
{
    static const char code[4][2] = {
        {'B', 'G'}, {'G', 'B'}, {'B', 'A'}, {'R', 'G'}
    };
    if (bayer >= 4U)
        return 0U;
    return fourcc(code[bayer][0], code[bayer][1], '1', '2');
}

static int valid_count(unsigned int count)
{
    return count == 1U || count == 2U || count == 4U || count == 8U ||
           count == 16U || count == 24U;
}

int ngcd_rkraw_latest_capture(const char *root, int sensor,
                              char *directory, size_t directory_size)
{
    glob_t matches;
    struct statx status;
    struct statx_timestamp newest = {0, 0, 0};
    const char *selected = NULL;
    char pattern[512];
    size_t index;
    int count;
    if (root == NULL || sensor < 0 || sensor > 1 || directory == NULL ||
        directory_size == 0U)
        return -1;
    count = snprintf(pattern, sizeof(pattern), "%s/Cam%d-raw_*", root,
                     sensor);
    if (count <= 0 || (size_t)count >= sizeof(pattern))
        return -1;
    memset(&matches, 0, sizeof(matches));
    if (glob(pattern, 0, NULL, &matches) != 0)
        return -1;
    for (index = 0U; index < matches.gl_pathc; ++index) {
        memset(&status, 0, sizeof(status));
        if (statx(AT_FDCWD, matches.gl_pathv[index], AT_SYMLINK_NOFOLLOW,
                  STATX_MTIME, &status) != 0 ||
            (status.stx_mode & 0170000U) != 0040000U)
            continue;
        if (selected == NULL || status.stx_mtime.tv_sec > newest.tv_sec ||
            (status.stx_mtime.tv_sec == newest.tv_sec &&
             status.stx_mtime.tv_nsec > newest.tv_nsec)) {
            selected = matches.gl_pathv[index];
            newest = status.stx_mtime;
        }
    }
    if (selected == NULL || strlen(selected) >= directory_size) {
        globfree(&matches);
        return -1;
    }
    memcpy(directory, selected, strlen(selected) + 1U);
    globfree(&matches);
    return 0;
}

static int same_layout(const unsigned char *first,
                       const unsigned char *candidate)
{
    return get_u16(first) == get_u16(candidate) &&
           get_u16(first + 2U) == get_u16(candidate + 2U) &&
           get_u16(first + 8U) == get_u16(candidate + 8U) &&
           get_u16(first + 10U) == get_u16(candidate + 10U) &&
           first[12] == candidate[12] && first[13] == candidate[13] &&
           first[14] == candidate[14] && first[16] == candidate[16] &&
           get_u16(first + 17U) == get_u16(candidate + 17U) &&
           get_u16(first + 19U) == get_u16(candidate + 19U);
}

static unsigned int interpolated_black(unsigned int iso, unsigned int cfa)
{
    static const unsigned int iso_points[5] = {50U, 100U, 200U, 400U, 800U};
    /* Factory IQ BLC values in sixteenths, ordered R, Gr, Gb, B. */
    static const unsigned int black[4][5] = {
        {4103U, 4103U, 4104U, 4106U, 4114U},
        {4103U, 4102U, 4102U, 4102U, 4106U},
        {4103U, 4102U, 4102U, 4103U, 4108U},
        {4103U, 4103U, 4103U, 4105U, 4112U},
    };
    unsigned int index;
    if (cfa >= 4U)
        cfa = 0U;
    if (iso <= iso_points[0])
        return (black[cfa][0] + 8U) / 16U;
    for (index = 1U; index < 5U; ++index) {
        if (iso <= iso_points[index]) {
            unsigned int span = iso_points[index] - iso_points[index - 1U];
            unsigned int offset = iso - iso_points[index - 1U];
            unsigned int value =
                black[cfa][index - 1U] * (span - offset) +
                black[cfa][index] * offset;
            return (value + span * 8U) / (span * 16U);
        }
    }
    return (black[cfa][4] + 8U) / 16U;
}

static unsigned int cfa_channel(unsigned int bayer, unsigned int row,
                                unsigned int column)
{
    /* Values index the R, Gr, Gb, B BLC table above. */
    static const unsigned char map[4][4] = {
        {3, 2, 1, 0}, /* BGGR */
        {2, 3, 0, 1}, /* GBRG */
        {1, 0, 3, 2}, /* GRBG */
        {0, 1, 2, 3}, /* RGGB */
    };
    return map[bayer][((row & 1U) << 1U) | (column & 1U)];
}

static int open_inputs(const char *directory, unsigned int count,
                       FILE **inputs, unsigned char *header,
                       uint32_t *width, uint32_t *height, uint32_t *stride,
                       uint32_t *active_stride, unsigned int *bayer)
{
    glob_t matches;
    char pattern[512];
    unsigned int index;
    int written;
    memset(&matches, 0, sizeof(matches));
    written = snprintf(pattern, sizeof(pattern), "%s/frame*_normal.raw",
                       directory);
    if (written <= 0 || (size_t)written >= sizeof(pattern) ||
        glob(pattern, 0, NULL, &matches) != 0 || matches.gl_pathc != count) {
        globfree(&matches);
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        unsigned char candidate[RAW_HEADER_SIZE];
        long size;
        inputs[index] = fopen(matches.gl_pathv[index], "rb");
        if (inputs[index] == NULL ||
            fread(candidate, 1U, sizeof(candidate), inputs[index]) !=
                sizeof(candidate))
            goto fail;
        if (index == 0U) {
            memcpy(header, candidate, RAW_HEADER_SIZE);
            *width = get_u16(header + 8U);
            *height = get_u16(header + 10U);
            *stride = get_u16(header + 17U);
            *active_stride = get_u16(header + 19U);
            *bayer = header[13];
        } else if (!same_layout(header, candidate)) {
            goto fail;
        }
        if (fseek(inputs[index], 0L, SEEK_END) != 0 ||
            (size = ftell(inputs[index])) < 0 ||
            (uint64_t)size != RAW_HEADER_SIZE +
                                  (uint64_t)*stride * (uint64_t)*height ||
            fseek(inputs[index], RAW_HEADER_SIZE, SEEK_SET) != 0)
            goto fail;
    }
    globfree(&matches);
    if (get_u16(header) != 0x8080U || get_u16(header + 2U) != 128U ||
        *width == 0U || *height == 0U || (*width & 1U) != 0U ||
        header[12] != 12U || *bayer > 3U || header[14] != 1U ||
        header[16] != 0U || *active_stride != *width * 3U / 2U ||
        *stride < *active_stride || *stride > 8192U)
        return -1;
    return 0;
fail:
    globfree(&matches);
    return -1;
}

static void close_inputs(FILE **inputs, unsigned int count)
{
    unsigned int index;
    for (index = 0U; index < count; ++index) {
        if (inputs[index] != NULL) {
            fclose(inputs[index]);
            inputs[index] = NULL;
        }
    }
}

int ngcd_rkraw_build_stack(const char *directory, unsigned int count,
                           const char *sensor_name,
                           const struct ngcd_rkraw_metadata *metadata,
                           struct ngcd_rkraw_buffer *output)
{
    FILE *inputs[24] = {NULL};
    unsigned char header[RAW_HEADER_SIZE];
    unsigned char *rows = NULL;
    int32_t *accumulated = NULL;
    unsigned char *raw;
    unsigned char *format;
    unsigned char *block;
    unsigned char *stats;
    uint32_t width = 0U, height = 0U, stride = 0U, active = 0U;
    unsigned int black_level[4];
    unsigned int bayer = 0U;
    size_t raw_bytes;
    size_t total;
    unsigned int row;
    unsigned int input;
    int result = -1;
    if (directory == NULL || sensor_name == NULL || metadata == NULL ||
        output == NULL || !valid_count(count))
        return -1;
    memset(output, 0, sizeof(*output));
    if (open_inputs(directory, count, inputs, header, &width, &height,
                    &stride, &active, &bayer) != 0)
        goto done;
    /* Rockchip's rawrd nodes retain the CSI-packed RAW12 layout, including
     * their 96-byte line alignment.  The vendor file parser copies the block
     * into a fixed-size V4L2 output buffer, so both stride and total length
     * must match the capture layout exactly. */
    raw_bytes = (size_t)stride * height;
    total = 2U + RKRAW_FORMAT_SIZE + RKRAW_BLOCK_HEADER_SIZE + 16U +
            RKRAW_FRAME_INFO_SIZE + 2U;
    output->data = calloc(1U, total);
    if (posix_memalign((void **)&output->raw_data, 4096U, raw_bytes) != 0)
        output->raw_data = NULL;
    rows = malloc((size_t)stride * count);
    accumulated = malloc((size_t)width * sizeof(*accumulated));
    if (output->data == NULL || output->raw_data == NULL || rows == NULL ||
        accumulated == NULL)
        goto done;
    memset(output->raw_data, 0, raw_bytes);
    for (input = 0U; input < 4U; ++input)
        black_level[input] = interpolated_black(metadata->iso, input);

    put_u16(output->data, RKRAW_START_TAG);
    format = output->data + 2U;
    put_u16(format, RKRAW_FORMAT_TAG);
    put_u32(format + 2U, RKRAW_FORMAT_SIZE - RKRAW_BLOCK_HEADER_SIZE);
    put_u16(format + 6U, 1U);
    memcpy(format + 8U, sensor_name,
           strlen(sensor_name) < 31U ? strlen(sensor_name) : 31U);
    memcpy(format + 40U, "normal", 6U);
    put_u32(format + 72U, metadata->frame_id != 0U ? metadata->frame_id :
                                                    get_u32(header + 4U));
    put_u16(format + 76U, (uint16_t)width);
    put_u16(format + 78U, (uint16_t)height);
    format[80] = 12U;
    format[81] = (unsigned char)bayer;
    format[82] = 1U;
    format[83] = 0U; /* address descriptor */
    put_u16(format + 84U, (uint16_t)stride);
    put_u16(format + 86U, (uint16_t)active);
    format[88] = 0U;

    block = format + RKRAW_FORMAT_SIZE;
    put_u16(block, RKRAW_NORMAL_TAG);
    put_u32(block + 2U, 16U);
    raw = output->raw_data;
    {
        uintptr_t address = (uintptr_t)raw;
        unsigned char *address_info = block + RKRAW_BLOCK_HEADER_SIZE;
        put_u32(address_info, 0U);
        put_u32(address_info + 4U, (uint32_t)(address >> 32U));
        put_u32(address_info + 8U, (uint32_t)address);
        put_u32(address_info + 12U, (uint32_t)raw_bytes);
    }
    for (row = 0U; row < height; ++row) {
        unsigned int black_first =
            black_level[cfa_channel(bayer, row, 0U)];
        unsigned int black_second =
            black_level[cfa_channel(bayer, row, 1U)];
        unsigned int pixel;
        unsigned int offset;
        memset(accumulated, 0, (size_t)width * sizeof(*accumulated));
        for (input = 0U; input < count; ++input) {
            unsigned char *source = rows + (size_t)input * stride;
            if (fread(source, 1U, stride, inputs[input]) != stride)
                goto done;
            pixel = 0U;
            for (offset = 0U; offset < active; offset += 3U) {
                unsigned int first = source[offset] |
                    ((unsigned int)(source[offset + 1U] & 15U) << 8U);
                unsigned int second = (source[offset + 1U] >> 4U) |
                    ((unsigned int)source[offset + 2U] << 4U);
                /* Accumulate signed signal around the calibrated CFA black
                 * point.  Deferring the clamp avoids the black lift produced
                 * by clipping each noisy input independently. */
                accumulated[pixel] += (int32_t)first - (int32_t)black_first;
                accumulated[pixel + 1U] +=
                    (int32_t)second - (int32_t)black_second;
                pixel += 2U;
            }
        }
        for (pixel = 0U, offset = 0U; pixel < width;
             pixel += 2U, offset += 3U) {
            int64_t first = (int64_t)black_first + accumulated[pixel];
            int64_t second = (int64_t)black_second +
                             accumulated[pixel + 1U];
            if (first < 0) first = 0;
            if (second < 0) second = 0;
            if (first > 4095) first = 4095;
            if (second > 4095) second = 4095;
            raw[(size_t)row * stride + offset] = (unsigned char)first;
            raw[(size_t)row * stride + offset + 1U] =
                (unsigned char)((first >> 8U) | ((second & 15) << 4U));
            raw[(size_t)row * stride + offset + 2U] =
                (unsigned char)(second >> 4U);
        }
    }

    stats = block + RKRAW_BLOCK_HEADER_SIZE + 16U;
    put_u16(stats, RKRAW_STATS_TAG);
    put_u32(stats + 2U, RKRAW_FRAME_INFO_SIZE - RKRAW_BLOCK_HEADER_SIZE);
    put_u16(stats + 6U, 1U);
    put_u32(stats + 8U, metadata->frame_id != 0U ? metadata->frame_id :
                                                 get_u32(header + 4U));
    put_float(stats + 12U, metadata->exposure_seconds);
    put_float(stats + 16U, metadata->gain);
    put_u32(stats + 20U, metadata->exposure_register != 0U
                              ? metadata->exposure_register : 1U);
    put_u32(stats + 24U, metadata->gain_register != 0U
                              ? metadata->gain_register : 1U);
    put_float(stats + 76U, metadata->white_balance_red);
    put_float(stats + 80U, metadata->white_balance_blue);
    put_u16(stats + RKRAW_FRAME_INFO_SIZE, RKRAW_END_TAG);
    output->size = total;
    output->raw_size = raw_bytes;
    output->width = width;
    output->height = height;
    output->format = bayer_format(bayer);
    result = output->format != 0U ? 0 : -1;

done:
    close_inputs(inputs, count);
    free(rows);
    free(accumulated);
    if (result != 0)
        ngcd_rkraw_free(output);
    return result;
}

void ngcd_rkraw_free(struct ngcd_rkraw_buffer *buffer)
{
    if (buffer == NULL)
        return;
    free(buffer->data);
    free(buffer->raw_data);
    memset(buffer, 0, sizeof(*buffer));
}

int ngcd_rkraw_set_frame_id(struct ngcd_rkraw_buffer *buffer,
                            uint32_t frame_id)
{
    unsigned char *format;
    unsigned char *raw_block;
    uint32_t raw_length;
    unsigned char *stats;
    if (buffer == NULL || buffer->data == NULL || frame_id == 0U ||
        buffer->size < 2U + RKRAW_FORMAT_SIZE + RKRAW_BLOCK_HEADER_SIZE +
                           RKRAW_FRAME_INFO_SIZE + 2U ||
        get_u16(buffer->data) != RKRAW_START_TAG)
        return -1;
    format = buffer->data + 2U;
    if (get_u16(format) != RKRAW_FORMAT_TAG)
        return -1;
    raw_block = format + RKRAW_FORMAT_SIZE;
    if (get_u16(raw_block) != RKRAW_NORMAL_TAG)
        return -1;
    raw_length = get_u32(raw_block + 2U);
    if ((size_t)raw_length > buffer->size -
            (2U + RKRAW_FORMAT_SIZE + RKRAW_BLOCK_HEADER_SIZE +
             RKRAW_FRAME_INFO_SIZE + 2U))
        return -1;
    stats = raw_block + RKRAW_BLOCK_HEADER_SIZE + raw_length;
    if (get_u16(stats) != RKRAW_STATS_TAG)
        return -1;
    put_u32(format + 72U, frame_id);
    put_u32(stats + 8U, frame_id);
    return 0;
}

int ngcd_rkraw_write_file(const struct ngcd_rkraw_buffer *buffer,
                          const char *path)
{
    unsigned char raw_header[RKRAW_BLOCK_HEADER_SIZE];
    const size_t prefix = 2U + RKRAW_FORMAT_SIZE;
    const size_t suffix_offset = prefix + RKRAW_BLOCK_HEADER_SIZE + 16U;
    const size_t suffix = RKRAW_FRAME_INFO_SIZE + 2U;
    FILE *file;
    int result = -1;
    if (buffer == NULL || buffer->data == NULL || buffer->raw_data == NULL ||
        buffer->raw_size == 0U || buffer->raw_size > UINT32_MAX ||
        buffer->size != suffix_offset + suffix || path == NULL ||
        path[0] == '\0')
        return -1;
    file = fopen(path, "wb");
    if (file == NULL)
        return -1;
    put_u16(raw_header, RKRAW_NORMAL_TAG);
    put_u32(raw_header + 2U, (uint32_t)buffer->raw_size);
    if (fwrite(buffer->data, 1U, prefix, file) == prefix &&
        fwrite(raw_header, 1U, sizeof(raw_header), file) ==
            sizeof(raw_header) &&
        fwrite(buffer->raw_data, 1U, buffer->raw_size, file) ==
            buffer->raw_size &&
        fwrite(buffer->data + suffix_offset, 1U, suffix, file) == suffix &&
        fflush(file) == 0)
        result = 0;
    if (fclose(file) != 0)
        result = -1;
    if (result != 0)
        (void)unlink(path);
    return result;
}
