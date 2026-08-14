#ifndef CALF_NGCD_RAW_H
#define CALF_NGCD_RAW_H

#include <stddef.h>
#include <stdint.h>

struct ngcd_rkraw_metadata {
    uint32_t frame_id;
    float exposure_seconds;
    float gain;
    uint32_t exposure_register;
    uint32_t gain_register;
    float white_balance_red;
    float white_balance_blue;
    unsigned int iso;
};

struct ngcd_rkraw_buffer {
    unsigned char *data;
    size_t size;
    unsigned char *raw_data;
    size_t raw_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

int ngcd_rkraw_latest_capture(const char *root, int sensor,
                              char *directory, size_t directory_size);
int ngcd_rkraw_build_stack(const char *directory, unsigned int count,
                           const char *sensor_name,
                           const struct ngcd_rkraw_metadata *metadata,
                           struct ngcd_rkraw_buffer *output);
int ngcd_rkraw_set_frame_id(struct ngcd_rkraw_buffer *buffer,
                            uint32_t frame_id);
int ngcd_rkraw_write_file(const struct ngcd_rkraw_buffer *buffer,
                          const char *path);
void ngcd_rkraw_free(struct ngcd_rkraw_buffer *buffer);

#endif
