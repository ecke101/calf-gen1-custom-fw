#ifndef CALF_NGCD_MP4_H
#define CALF_NGCD_MP4_H

#include <stddef.h>
#include <stdint.h>

struct ngcd_mp4_writer;

int ngcd_mp4_open(struct ngcd_mp4_writer **writer, const char *path,
                  unsigned int width, unsigned int height,
                  unsigned int fps);
int ngcd_mp4_open_h265(struct ngcd_mp4_writer **writer, const char *path,
                       unsigned int width, unsigned int height,
                       unsigned int fps);
int ngcd_mp4_write_h264(struct ngcd_mp4_writer *writer,
                        const unsigned char *annex_b, size_t size,
                        uint64_t pts_us);
int ngcd_mp4_write_h265(struct ngcd_mp4_writer *writer,
                        const unsigned char *annex_b, size_t size,
                        uint64_t pts_us);
int ngcd_mp4_write_pcm_s16le(struct ngcd_mp4_writer *writer,
                             const unsigned char *pcm, size_t size,
                             uint64_t pts_us, unsigned int channels,
                             unsigned int sample_rate);
int ngcd_mp4_write_camm_gyro(struct ngcd_mp4_writer *writer,
                             uint64_t pts_us, float x_radians_per_second,
                             float y_radians_per_second,
                             float z_radians_per_second);
int ngcd_mp4_current_size(struct ngcd_mp4_writer *writer, uint64_t *bytes);
int ngcd_mp4_duration(struct ngcd_mp4_writer *writer, uint64_t *microseconds);
int ngcd_mp4_recover(const char *temporary_path);
int ngcd_mp4_close(struct ngcd_mp4_writer *writer);
void ngcd_mp4_abort(struct ngcd_mp4_writer *writer);

#endif
