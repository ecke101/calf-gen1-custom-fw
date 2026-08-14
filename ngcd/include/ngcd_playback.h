#ifndef CALF_NGCD_PLAYBACK_H
#define CALF_NGCD_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ngcd_playback_codec {
    NGCD_PLAYBACK_H264 = 1,
    NGCD_PLAYBACK_H265 = 2,
};

enum ngcd_playback_audio_codec {
    NGCD_PLAYBACK_AUDIO_NONE = 0,
    NGCD_PLAYBACK_AUDIO_PCM_S16LE = 1,
    NGCD_PLAYBACK_AUDIO_AAC = 2,
};

struct ngcd_playback_sample {
    uint64_t offset;
    uint64_t pts_us;
    uint32_t size;
    bool key_frame;
};

struct ngcd_mp4_reader;

int ngcd_jpeg_probe(const char *path, unsigned int *width,
                    unsigned int *height, uint64_t *file_size);
uint64_t ngcd_file_create_time(const char *path);
int ngcd_mp4_reader_open(struct ngcd_mp4_reader **reader, const char *path);
void ngcd_mp4_reader_close(struct ngcd_mp4_reader *reader);
enum ngcd_playback_codec ngcd_mp4_reader_codec(
    const struct ngcd_mp4_reader *reader);
unsigned int ngcd_mp4_reader_width(const struct ngcd_mp4_reader *reader);
unsigned int ngcd_mp4_reader_height(const struct ngcd_mp4_reader *reader);
uint64_t ngcd_mp4_reader_duration_us(const struct ngcd_mp4_reader *reader);
uint64_t ngcd_mp4_reader_file_size(const struct ngcd_mp4_reader *reader);
uint64_t ngcd_mp4_reader_create_time(const struct ngcd_mp4_reader *reader);
size_t ngcd_mp4_reader_sample_count(const struct ngcd_mp4_reader *reader);
size_t ngcd_mp4_reader_max_sample_size(const struct ngcd_mp4_reader *reader);
const struct ngcd_playback_sample *ngcd_mp4_reader_sample(
    const struct ngcd_mp4_reader *reader, size_t index);
size_t ngcd_mp4_reader_key_frame_at_or_before(
    const struct ngcd_mp4_reader *reader, size_t index);
size_t ngcd_mp4_reader_first_key_frame(
    const struct ngcd_mp4_reader *reader);
size_t ngcd_mp4_reader_decoder_config(
    const struct ngcd_mp4_reader *reader, const unsigned char **data);
int ngcd_mp4_reader_read_sample(const struct ngcd_mp4_reader *reader,
                                size_t index, unsigned char *destination,
                                size_t capacity, size_t *written);
enum ngcd_playback_audio_codec ngcd_mp4_reader_audio_codec(
    const struct ngcd_mp4_reader *reader);
unsigned int ngcd_mp4_reader_audio_channels(
    const struct ngcd_mp4_reader *reader);
unsigned int ngcd_mp4_reader_audio_sample_rate(
    const struct ngcd_mp4_reader *reader);
unsigned int ngcd_mp4_reader_audio_object_type(
    const struct ngcd_mp4_reader *reader);
size_t ngcd_mp4_reader_audio_sample_count(
    const struct ngcd_mp4_reader *reader);
size_t ngcd_mp4_reader_audio_max_sample_size(
    const struct ngcd_mp4_reader *reader);
const struct ngcd_playback_sample *ngcd_mp4_reader_audio_sample(
    const struct ngcd_mp4_reader *reader, size_t index);
int ngcd_mp4_reader_read_audio_sample(const struct ngcd_mp4_reader *reader,
                                      size_t index,
                                      unsigned char *destination,
                                      size_t capacity, size_t *written);

#endif
