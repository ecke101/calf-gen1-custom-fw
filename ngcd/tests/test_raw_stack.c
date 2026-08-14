#include "ngcd_raw.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put16(unsigned char *data, uint16_t value)
{
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8U);
}

static void write_raw(const char *path, unsigned int frame,
                      unsigned int first, unsigned int second)
{
    unsigned char bytes[128 + 12];
    FILE *file;
    unsigned int row;
    memset(bytes, 0, sizeof(bytes));
    put16(bytes, 0x8080U);
    put16(bytes + 2U, 128U);
    bytes[4] = (unsigned char)frame;
    put16(bytes + 8U, 4U);
    put16(bytes + 10U, 2U);
    bytes[12] = 12U;
    bytes[13] = 3U;
    bytes[14] = 1U;
    put16(bytes + 17U, 6U);
    put16(bytes + 19U, 6U);
    for (row = 0U; row < 2U; ++row) {
        unsigned char *packed = bytes + 128U + row * 6U;
        packed[0] = (unsigned char)first;
        packed[1] = (unsigned char)((first >> 8U) | (second << 4U));
        packed[2] = (unsigned char)(second >> 4U);
        memcpy(packed + 3U, packed, 3U);
    }
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes));
    assert(fclose(file) == 0);
}

static uint32_t get32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void assert_rkraw_frame_id(const char *path, size_t raw_size,
                                  uint32_t frame_id)
{
    unsigned char bytes[4];
    FILE *file = fopen(path, "rb");
    long stats_frame_offset = (long)(2U + 89U + 6U + raw_size + 8U);
    assert(file != NULL);
    assert(fseek(file, 2L + 72L, SEEK_SET) == 0);
    assert(fread(bytes, 1U, sizeof(bytes), file) == sizeof(bytes));
    assert(get32(bytes) == frame_id);
    assert(fseek(file, stats_frame_offset, SEEK_SET) == 0);
    assert(fread(bytes, 1U, sizeof(bytes), file) == sizeof(bytes));
    assert(get32(bytes) == frame_id);
    assert(fclose(file) == 0);
}

int main(void)
{
    char root[] = "/tmp/ngcd-raw-test-XXXXXX";
    char directory[512];
    char first_path[512];
    char second_path[512];
    char rkraw_path[512];
    struct ngcd_rkraw_metadata metadata;
    struct ngcd_rkraw_buffer output;
    unsigned char *packed;
    assert(mkdtemp(root) != NULL);
    assert(snprintf(directory, sizeof(directory), "%s/Cam0-raw_1", root) > 0);
    assert(mkdir(directory, 0700) == 0);
    assert(snprintf(first_path, sizeof(first_path),
                    "%s/frame0001_normal.raw", directory) > 0);
    assert(snprintf(second_path, sizeof(second_path),
                    "%s/frame0002_normal.raw", directory) > 0);
    write_raw(first_path, 1U, 250U, 300U);
    write_raw(second_path, 2U, 250U, 300U);
    memset(&metadata, 0, sizeof(metadata));
    metadata.frame_id = 12U;
    metadata.exposure_seconds = 1.0f;
    metadata.gain = 1.0f;
    metadata.exposure_register = 20U;
    metadata.gain_register = 30U;
    metadata.white_balance_red = 2.0f;
    metadata.white_balance_blue = 1.5f;
    metadata.iso = 100U;
    assert(ngcd_rkraw_build_stack(directory, 2U,
                                  "m00_imx577 2-001a", &metadata,
                                  &output) == 0);
    assert(output.width == 4U && output.height == 2U);
    assert(output.size == 2U + 89U + 6U + 16U + 84U + 2U);
    assert(output.raw_size == 12U);
    packed = output.raw_data;
    assert(((unsigned int)packed[0] |
            ((unsigned int)(packed[1] & 15U) << 8U)) == 244U);
    assert(((unsigned int)(packed[1] >> 4U) |
            ((unsigned int)packed[2] << 4U)) == 344U);
    assert(ngcd_rkraw_set_frame_id(&output, 99U) == 0);
    assert(output.data[74] == 99U);
    assert(snprintf(rkraw_path, sizeof(rkraw_path), "%s/stack.rkraw", root) > 0);
    assert(ngcd_rkraw_write_file(&output, rkraw_path) == 0);
    assert_rkraw_frame_id(rkraw_path, output.raw_size, 99U);
    assert(ngcd_rkraw_set_frame_id(&output, 100U) == 0);
    assert(ngcd_rkraw_write_file(&output, rkraw_path) == 0);
    assert_rkraw_frame_id(rkraw_path, output.raw_size, 100U);
    {
        struct stat status;
        assert(stat(rkraw_path, &status) == 0);
        assert((size_t)status.st_size ==
               2U + 89U + 6U + output.raw_size + 84U + 2U);
    }
    ngcd_rkraw_free(&output);
    assert(unlink(rkraw_path) == 0);
    assert(unlink(first_path) == 0);
    assert(unlink(second_path) == 0);
    assert(rmdir(directory) == 0);
    assert(rmdir(root) == 0);
    puts("raw stack tests passed");
    return 0;
}
