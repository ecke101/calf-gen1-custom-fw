#include "ngcd_mp4.h"
#include "ngcd_aac.h"

#include <stdbool.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "CAMM requires 32-bit IEEE floating point");

struct mp4_sample {
    uint64_t offset;
    uint64_t pts_us;
    uint32_t size;
    bool key_frame;
};

struct camm_sample {
    uint64_t offset;
    uint64_t pts_us;
};

struct audio_sample {
    uint64_t offset;
    uint64_t pts_us;
    uint32_t size;
    uint32_t frames;
};

struct ngcd_mp4_writer {
    FILE *file;
    FILE *journal;
    char *path;
    char *journal_path;
    struct mp4_sample *sample;
    size_t sample_count;
    size_t sample_capacity;
    struct camm_sample *camm_sample;
    size_t camm_sample_count;
    size_t camm_sample_capacity;
    struct audio_sample *audio_sample;
    size_t audio_sample_count;
    size_t audio_sample_capacity;
    unsigned char *sps;
    size_t sps_size;
    unsigned char *pps;
    size_t pps_size;
    unsigned char *vps;
    size_t vps_size;
    uint64_t mdat_offset;
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int audio_channels;
    unsigned int audio_sample_rate;
    struct ngcd_aac_encoder *audio_encoder;
    unsigned char audio_pcm[NGCD_AAC_FRAME_SAMPLES * 2U * 2U];
    size_t audio_pcm_size;
    bool audio_aac;
    bool h265;
    bool failed;
};

#define JOURNAL_HEADER_SIZE 32U
#define JOURNAL_RECORD_SIZE 32U
#define JOURNAL_VIDEO 1U
#define JOURNAL_CAMM 2U
#define JOURNAL_AUDIO 3U
#define JOURNAL_AUDIO_CONFIG 4U
#define JOURNAL_AUDIO_CONFIG_AAC 5U

static int write_bytes(FILE *file, const void *data, size_t size)
{
    return size == 0U || fwrite(data, 1U, size, file) == size ? 0 : -1;
}

static int write_u16(FILE *file, uint16_t value)
{
    unsigned char bytes[2] = {
        (unsigned char)(value >> 8U), (unsigned char)value,
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u32(FILE *file, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24U), (unsigned char)(value >> 16U),
        (unsigned char)(value >> 8U), (unsigned char)value,
    };
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u64(FILE *file, uint64_t value)
{
    return write_u32(file, (uint32_t)(value >> 32U)) == 0 &&
                   write_u32(file, (uint32_t)value) == 0
               ? 0 : -1;
}

static int read_bytes(FILE *file, void *data, size_t size)
{
    return size == 0U || fread(data, 1U, size, file) == size ? 0 : -1;
}

static int read_u32(FILE *file, uint32_t *value)
{
    unsigned char bytes[4];
    if (read_bytes(file, bytes, sizeof(bytes)) != 0)
        return -1;
    *value = ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
             ((uint32_t)bytes[2] << 8U) | bytes[3];
    return 0;
}

static int read_u64(FILE *file, uint64_t *value)
{
    uint32_t upper;
    uint32_t lower;
    if (read_u32(file, &upper) != 0 || read_u32(file, &lower) != 0)
        return -1;
    *value = ((uint64_t)upper << 32U) | lower;
    return 0;
}

static long box_begin(FILE *file, const char type[4])
{
    long offset = ftell(file);
    if (offset < 0 || write_u32(file, 0U) != 0 ||
        write_bytes(file, type, 4U) != 0)
        return -1;
    return offset;
}

static int box_end(FILE *file, long offset)
{
    long end = ftell(file);
    uint64_t size;
    if (offset < 0 || end < offset ||
        (size = (uint64_t)(end - offset)) > UINT32_MAX ||
        fseek(file, offset, SEEK_SET) != 0 ||
        write_u32(file, (uint32_t)size) != 0 ||
        fseek(file, end, SEEK_SET) != 0)
        return -1;
    return 0;
}

static int copy_parameter_set(unsigned char **destination,
                              size_t *destination_size,
                              const unsigned char *source, size_t size)
{
    unsigned char *copy;
    if (size == 0U || size > UINT16_MAX)
        return -1;
    if (*destination != NULL && *destination_size == size &&
        memcmp(*destination, source, size) == 0)
        return 0;
    copy = malloc(size);
    if (copy == NULL)
        return -1;
    memcpy(copy, source, size);
    free(*destination);
    *destination = copy;
    *destination_size = size;
    return 0;
}

static int next_start_code(const unsigned char *data, size_t size,
                           size_t from, size_t *position,
                           size_t *prefix_size)
{
    size_t index;
    for (index = from; index + 3U <= size; ++index) {
        if (data[index] != 0U || data[index + 1U] != 0U)
            continue;
        if (data[index + 2U] == 1U) {
            *position = index;
            *prefix_size = 3U;
            return 0;
        }
        if (index + 4U <= size && data[index + 2U] == 0U &&
            data[index + 3U] == 1U) {
            *position = index;
            *prefix_size = 4U;
            return 0;
        }
    }
    return -1;
}

static int reserve_sample(struct ngcd_mp4_writer *writer)
{
    struct mp4_sample *resized;
    size_t capacity;
    if (writer->sample_count < writer->sample_capacity)
        return 0;
    capacity = writer->sample_capacity == 0U ? 256U
                                             : writer->sample_capacity * 2U;
    if (capacity < writer->sample_capacity ||
        capacity > SIZE_MAX / sizeof(*writer->sample))
        return -1;
    resized = realloc(writer->sample, capacity * sizeof(*writer->sample));
    if (resized == NULL)
        return -1;
    writer->sample = resized;
    writer->sample_capacity = capacity;
    return 0;
}

static int reserve_camm_sample(struct ngcd_mp4_writer *writer)
{
    struct camm_sample *resized;
    size_t capacity;
    if (writer->camm_sample_count < writer->camm_sample_capacity)
        return 0;
    capacity = writer->camm_sample_capacity == 0U
                   ? 1024U : writer->camm_sample_capacity * 2U;
    if (capacity < writer->camm_sample_capacity ||
        capacity > SIZE_MAX / sizeof(*writer->camm_sample))
        return -1;
    resized = realloc(writer->camm_sample,
                      capacity * sizeof(*writer->camm_sample));
    if (resized == NULL)
        return -1;
    writer->camm_sample = resized;
    writer->camm_sample_capacity = capacity;
    return 0;
}

static int reserve_audio_sample(struct ngcd_mp4_writer *writer)
{
    struct audio_sample *resized;
    size_t capacity;
    if (writer->audio_sample_count < writer->audio_sample_capacity)
        return 0;
    capacity = writer->audio_sample_capacity == 0U
                   ? 256U : writer->audio_sample_capacity * 2U;
    if (capacity < writer->audio_sample_capacity ||
        capacity > SIZE_MAX / sizeof(*writer->audio_sample))
        return -1;
    resized = realloc(writer->audio_sample,
                      capacity * sizeof(*writer->audio_sample));
    if (resized == NULL)
        return -1;
    writer->audio_sample = resized;
    writer->audio_sample_capacity = capacity;
    return 0;
}

static int write_f32_le(FILE *file, float value)
{
    uint32_t bits;
    unsigned char bytes[4];
    memcpy(&bits, &value, sizeof(bits));
    bytes[0] = (unsigned char)bits;
    bytes[1] = (unsigned char)(bits >> 8U);
    bytes[2] = (unsigned char)(bits >> 16U);
    bytes[3] = (unsigned char)(bits >> 24U);
    return write_bytes(file, bytes, sizeof(bytes));
}

static int open_journal(struct ngcd_mp4_writer *writer)
{
    static const unsigned char magic[8] = {
        'N', 'G', 'C', 'D', 'M', 'P', '4', 'J',
    };
    size_t length = strlen(writer->path);
    if (length > SIZE_MAX - 5U)
        return -1;
    writer->journal_path = malloc(length + 5U);
    if (writer->journal_path == NULL)
        return -1;
    memcpy(writer->journal_path, writer->path, length);
    memcpy(writer->journal_path + length, ".idx", 5U);
    writer->journal = fopen(writer->journal_path, "wbx");
    if (writer->journal == NULL ||
        write_bytes(writer->journal, magic, sizeof(magic)) != 0 ||
        write_u32(writer->journal, 1U) != 0 ||
        write_u32(writer->journal, writer->h265 ? 2U : 1U) != 0 ||
        write_u32(writer->journal, writer->width) != 0 ||
        write_u32(writer->journal, writer->height) != 0 ||
        write_u32(writer->journal, writer->fps) != 0 ||
        write_u32(writer->journal, JOURNAL_HEADER_SIZE) != 0 ||
        fflush(writer->journal) != 0)
        return -1;
    return 0;
}

static int journal_sample(struct ngcd_mp4_writer *writer, uint32_t kind,
                          uint32_t flags, uint64_t offset, uint64_t pts_us,
                          uint32_t size, bool flush)
{
    if (writer->journal == NULL ||
        (flush && fflush(writer->file) != 0) ||
        write_u32(writer->journal, kind) != 0 ||
        write_u32(writer->journal, flags) != 0 ||
        write_u64(writer->journal, offset) != 0 ||
        write_u64(writer->journal, pts_us) != 0 ||
        write_u32(writer->journal, size) != 0 ||
        write_u32(writer->journal, JOURNAL_RECORD_SIZE) != 0 ||
        (flush && fflush(writer->journal) != 0))
        return -1;
    return 0;
}

static int open_writer(struct ngcd_mp4_writer **output, const char *path,
                       unsigned int width, unsigned int height,
                       unsigned int fps, bool h265)
{
    static const unsigned char ftyp_h264[] = {
        0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p',
        'i', 's', 'o', 'm', 0x00, 0x00, 0x02, 0x00,
        'i', 's', 'o', 'm', 'i', 's', 'o', '2',
        'a', 'v', 'c', '1', 'm', 'p', '4', '1',
    };
    static const unsigned char ftyp_h265[] = {
        0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p',
        'i', 's', 'o', 'm', 0x00, 0x00, 0x02, 0x00,
        'i', 's', 'o', 'm', 'i', 's', 'o', '2',
        'h', 'v', 'c', '1', 'm', 'p', '4', '1',
    };
    const unsigned char *ftyp = h265 ? ftyp_h265 : ftyp_h264;
    struct ngcd_mp4_writer *writer;
    long mdat_offset;
    size_t path_size;
    if (output == NULL || path == NULL || path[0] == '\0' || width == 0U ||
        height == 0U || width > UINT16_MAX || height > UINT16_MAX ||
        fps == 0U || fps > 240U)
        return -1;
    *output = NULL;
    writer = calloc(1U, sizeof(*writer));
    if (writer == NULL)
        return -1;
    path_size = strlen(path) + 1U;
    writer->path = malloc(path_size);
    if (writer->path == NULL) {
        free(writer);
        return -1;
    }
    memcpy(writer->path, path, path_size);
    writer->width = width;
    writer->height = height;
    writer->fps = fps;
    writer->h265 = h265;
    writer->file = fopen(path, "wb+");
    if (writer->file == NULL || write_bytes(writer->file, ftyp,
                                             sizeof(ftyp_h264)) != 0) {
        ngcd_mp4_abort(writer);
        return -1;
    }
    mdat_offset = ftell(writer->file);
    if (mdat_offset < 0 || write_u32(writer->file, 1U) != 0 ||
        write_bytes(writer->file, "mdat", 4U) != 0 ||
        write_u64(writer->file, 0U) != 0) {
        ngcd_mp4_abort(writer);
        return -1;
    }
    writer->mdat_offset = (uint64_t)mdat_offset;
    if (open_journal(writer) != 0) {
        ngcd_mp4_abort(writer);
        return -1;
    }
    *output = writer;
    return 0;
}

int ngcd_mp4_open(struct ngcd_mp4_writer **output, const char *path,
                  unsigned int width, unsigned int height,
                  unsigned int fps)
{
    return open_writer(output, path, width, height, fps, false);
}

int ngcd_mp4_open_h265(struct ngcd_mp4_writer **output, const char *path,
                       unsigned int width, unsigned int height,
                       unsigned int fps)
{
    return open_writer(output, path, width, height, fps, true);
}

int ngcd_mp4_write_h264(struct ngcd_mp4_writer *writer,
                        const unsigned char *data, size_t size,
                        uint64_t pts_us)
{
    struct mp4_sample sample;
    size_t start;
    size_t prefix;
    size_t cursor;
    long offset;
    if (writer == NULL || writer->file == NULL || writer->h265 ||
        writer->failed ||
        data == NULL || size == 0U || reserve_sample(writer) != 0 ||
        (writer->sample_count > 0U &&
         pts_us < writer->sample[writer->sample_count - 1U].pts_us) ||
        next_start_code(data, size, 0U, &start, &prefix) != 0)
        return -1;
    offset = ftell(writer->file);
    if (offset < 0)
        return -1;
    memset(&sample, 0, sizeof(sample));
    sample.offset = (uint64_t)offset;
    sample.pts_us = pts_us;
    cursor = start;
    while (next_start_code(data, size, cursor, &start, &prefix) == 0) {
        size_t nal_start = start + prefix;
        size_t next = size;
        size_t next_prefix;
        size_t nal_end = size;
        size_t nal_size;
        unsigned int type;
        if (next_start_code(data, size, nal_start, &next,
                            &next_prefix) == 0)
            nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1U] == 0U)
            --nal_end;
        if (nal_end <= nal_start) {
            cursor = next < size ? next : size;
            continue;
        }
        nal_size = nal_end - nal_start;
        if (nal_size > UINT32_MAX ||
            sample.size > UINT32_MAX - 4U - (uint32_t)nal_size) {
            writer->failed = true;
            return -1;
        }
        type = data[nal_start] & 0x1fU;
        if ((type == 7U && copy_parameter_set(&writer->sps,
                                              &writer->sps_size,
                                              data + nal_start,
                                              nal_size) != 0) ||
            (type == 8U && copy_parameter_set(&writer->pps,
                                              &writer->pps_size,
                                              data + nal_start,
                                              nal_size) != 0) ||
            write_u32(writer->file, (uint32_t)nal_size) != 0 ||
            write_bytes(writer->file, data + nal_start, nal_size) != 0) {
            writer->failed = true;
            return -1;
        }
        if (type == 5U)
            sample.key_frame = true;
        sample.size += 4U + (uint32_t)nal_size;
        if (nal_end == size)
            break;
        cursor = next;
    }
    if (sample.size == 0U) {
        writer->failed = true;
        return -1;
    }
    if (journal_sample(writer, JOURNAL_VIDEO,
                       sample.key_frame ? 1U : 0U, sample.offset,
                       sample.pts_us, sample.size, true) != 0) {
        writer->failed = true;
        return -1;
    }
    writer->sample[writer->sample_count++] = sample;
    return 0;
}

int ngcd_mp4_write_h265(struct ngcd_mp4_writer *writer,
                        const unsigned char *data, size_t size,
                        uint64_t pts_us)
{
    struct mp4_sample sample;
    size_t start;
    size_t prefix;
    size_t cursor;
    long offset;
    if (writer == NULL || writer->file == NULL || !writer->h265 ||
        writer->failed || data == NULL || size == 0U ||
        reserve_sample(writer) != 0 ||
        (writer->sample_count > 0U &&
         pts_us < writer->sample[writer->sample_count - 1U].pts_us) ||
        next_start_code(data, size, 0U, &start, &prefix) != 0)
        return -1;
    offset = ftell(writer->file);
    if (offset < 0)
        return -1;
    memset(&sample, 0, sizeof(sample));
    sample.offset = (uint64_t)offset;
    sample.pts_us = pts_us;
    cursor = start;
    while (next_start_code(data, size, cursor, &start, &prefix) == 0) {
        size_t nal_start = start + prefix;
        size_t next = size;
        size_t next_prefix;
        size_t nal_end = size;
        size_t nal_size;
        unsigned int type;
        if (next_start_code(data, size, nal_start, &next,
                            &next_prefix) == 0)
            nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1U] == 0U)
            --nal_end;
        if (nal_end <= nal_start) {
            cursor = next < size ? next : size;
            continue;
        }
        nal_size = nal_end - nal_start;
        if (nal_size > UINT32_MAX ||
            sample.size > UINT32_MAX - 4U - (uint32_t)nal_size) {
            writer->failed = true;
            return -1;
        }
        type = (data[nal_start] >> 1U) & 0x3fU;
        if ((type == 32U && copy_parameter_set(&writer->vps,
                                               &writer->vps_size,
                                               data + nal_start,
                                               nal_size) != 0) ||
            (type == 33U && copy_parameter_set(&writer->sps,
                                               &writer->sps_size,
                                               data + nal_start,
                                               nal_size) != 0) ||
            (type == 34U && copy_parameter_set(&writer->pps,
                                               &writer->pps_size,
                                               data + nal_start,
                                               nal_size) != 0) ||
            write_u32(writer->file, (uint32_t)nal_size) != 0 ||
            write_bytes(writer->file, data + nal_start, nal_size) != 0) {
            writer->failed = true;
            return -1;
        }
        if (type >= 16U && type <= 23U)
            sample.key_frame = true;
        sample.size += 4U + (uint32_t)nal_size;
        if (nal_end == size)
            break;
        cursor = next;
    }
    if (sample.size == 0U) {
        writer->failed = true;
        return -1;
    }
    if (journal_sample(writer, JOURNAL_VIDEO,
                       sample.key_frame ? 1U : 0U, sample.offset,
                       sample.pts_us, sample.size, true) != 0) {
        writer->failed = true;
        return -1;
    }
    writer->sample[writer->sample_count++] = sample;
    return 0;
}

static int write_aac_frame(struct ngcd_mp4_writer *writer)
{
    unsigned char encoded[NGCD_AAC_MAX_OUTPUT_SIZE];
    struct audio_sample sample;
    size_t encoded_size;
    long offset;
    if (writer->audio_encoder == NULL ||
        writer->audio_pcm_size != NGCD_AAC_FRAME_SAMPLES *
                                      writer->audio_channels * 2U ||
        reserve_audio_sample(writer) != 0 ||
        ngcd_aac_encoder_encode(writer->audio_encoder, writer->audio_pcm,
                                writer->audio_pcm_size, encoded,
                                sizeof(encoded), &encoded_size) != 0 ||
        encoded_size > UINT32_MAX)
        return -1;
    offset = ftell(writer->file);
    sample.offset = offset >= 0 ? (uint64_t)offset : 0U;
    sample.pts_us = writer->sample[0].pts_us +
                    (uint64_t)writer->audio_sample_count *
                        NGCD_AAC_FRAME_SAMPLES * UINT64_C(1000000) /
                        writer->audio_sample_rate;
    sample.size = (uint32_t)encoded_size;
    sample.frames = NGCD_AAC_FRAME_SAMPLES;
    if (offset < 0 || write_bytes(writer->file, encoded, encoded_size) != 0 ||
        journal_sample(writer, JOURNAL_AUDIO, sample.frames, sample.offset,
                       sample.pts_us, sample.size, true) != 0) {
        writer->failed = true;
        return -1;
    }
    writer->audio_sample[writer->audio_sample_count++] = sample;
    writer->audio_pcm_size = 0U;
    return 0;
}

static int feed_aac_pcm(struct ngcd_mp4_writer *writer,
                        const unsigned char *pcm, size_t size)
{
    size_t frame_bytes = NGCD_AAC_FRAME_SAMPLES *
                         writer->audio_channels * 2U;
    while (size > 0U) {
        size_t available = frame_bytes - writer->audio_pcm_size;
        size_t copy = size < available ? size : available;
        memcpy(writer->audio_pcm + writer->audio_pcm_size, pcm, copy);
        writer->audio_pcm_size += copy;
        pcm += copy;
        size -= copy;
        if (writer->audio_pcm_size == frame_bytes &&
            write_aac_frame(writer) != 0)
            return -1;
    }
    return 0;
}

int ngcd_mp4_write_pcm_s16le(struct ngcd_mp4_writer *writer,
                             const unsigned char *pcm, size_t size,
                             uint64_t pts_us, unsigned int channels,
                             unsigned int sample_rate)
{
    static const unsigned char silence[4096] = {0};
    uint64_t video_pts;
    uint64_t frames;
    size_t frame_size;
    if (writer == NULL || writer->file == NULL || writer->failed ||
        pcm == NULL || size == 0U || size > UINT32_MAX ||
        (channels != 1U && channels != 2U) || sample_rate != 48000U)
        return -1;
    frame_size = channels * 2U;
    if (size % frame_size != 0U ||
        (writer->audio_encoder != NULL &&
         (channels != writer->audio_channels ||
          sample_rate != writer->audio_sample_rate)))
        return -1;
    if (writer->sample_count == 0U)
        return 0;
    video_pts = writer->sample[0].pts_us;
    if (writer->audio_encoder == NULL && pts_us < video_pts) {
        uint64_t delta = video_pts - pts_us;
        if (delta > (UINT64_MAX - 999999U) / sample_rate)
            return -1;
        frames = (delta * sample_rate + 999999U) / 1000000U;
        if (frames >= size / frame_size)
            return 0;
        pcm += (size_t)frames * frame_size;
        size -= (size_t)frames * frame_size;
        pts_us = video_pts;
    }
    if (writer->audio_encoder == NULL) {
        writer->audio_channels = channels;
        writer->audio_sample_rate = sample_rate;
        writer->audio_aac = true;
        if (ngcd_aac_encoder_open(&writer->audio_encoder, channels,
                                  sample_rate, 128000U) != 0 ||
            journal_sample(writer, JOURNAL_AUDIO_CONFIG_AAC, channels,
                           sample_rate, 0U, 0U, true) != 0) {
            writer->failed = true;
            return -1;
        }
        if (pts_us > video_pts) {
            uint64_t delta = pts_us - video_pts;
            uint64_t remaining;
            if (delta > (UINT64_MAX - 500000U) / sample_rate)
                return -1;
            frames = (delta * sample_rate + 500000U) / 1000000U;
            if (frames > UINT64_MAX / frame_size)
                return -1;
            remaining = frames * frame_size;
            while (remaining > 0U) {
                size_t chunk = remaining < sizeof(silence)
                                   ? (size_t)remaining : sizeof(silence);
                if (feed_aac_pcm(writer, silence, chunk) != 0)
                    return -1;
                remaining -= chunk;
            }
        }
    }
    return feed_aac_pcm(writer, pcm, size);
}

int ngcd_mp4_write_camm_gyro(struct ngcd_mp4_writer *writer,
                             uint64_t pts_us, float x, float y, float z)
{
    static const unsigned char header[4] = {0U, 0U, 2U, 0U};
    struct camm_sample sample;
    long offset;
    if (writer == NULL || writer->file == NULL || writer->failed ||
        !(x >= -FLT_MAX && x <= FLT_MAX) ||
        !(y >= -FLT_MAX && y <= FLT_MAX) ||
        !(z >= -FLT_MAX && z <= FLT_MAX))
        return -1;
    /* CAMM may begin as soon as record is armed, while VENC starts at the
     * first IDR. Never let that metadata move the MP4 movie origin. */
    if (writer->sample_count == 0U || pts_us < writer->sample[0].pts_us)
        return 0;
    if (writer->camm_sample_count > 0U &&
        pts_us < writer->camm_sample[writer->camm_sample_count - 1U].pts_us)
        return -1;
    if (reserve_camm_sample(writer) != 0)
        return -1;
    offset = ftell(writer->file);
    if (offset < 0 || write_bytes(writer->file, header, sizeof(header)) != 0 ||
        write_f32_le(writer->file, x) != 0 ||
        write_f32_le(writer->file, y) != 0 ||
        write_f32_le(writer->file, z) != 0) {
        writer->failed = true;
        return -1;
    }
    sample.offset = (uint64_t)offset;
    sample.pts_us = pts_us;
    if (journal_sample(writer, JOURNAL_CAMM, 0U, sample.offset,
                       sample.pts_us, 16U, false) != 0) {
        writer->failed = true;
        return -1;
    }
    writer->camm_sample[writer->camm_sample_count++] = sample;
    return 0;
}

int ngcd_mp4_current_size(struct ngcd_mp4_writer *writer, uint64_t *bytes)
{
    long offset;
    if (writer == NULL || writer->file == NULL || writer->failed ||
        bytes == NULL || (offset = ftell(writer->file)) < 0)
        return -1;
    *bytes = (uint64_t)offset;
    return 0;
}

int ngcd_mp4_duration(struct ngcd_mp4_writer *writer, uint64_t *microseconds)
{
    uint64_t first;
    uint64_t last;
    if (writer == NULL || writer->file == NULL || writer->failed ||
        microseconds == NULL)
        return -1;
    if (writer->sample_count == 0U) {
        *microseconds = 0U;
        return 0;
    }
    first = writer->sample[0].pts_us;
    last = writer->sample[writer->sample_count - 1U].pts_us;
    *microseconds = last - first + UINT64_C(1000000) / writer->fps;
    return 0;
}

static uint32_t sample_duration(const struct ngcd_mp4_writer *writer,
                                size_t index)
{
    uint64_t difference;
    uint64_t ticks;
    if (index + 1U >= writer->sample_count ||
        writer->sample[index + 1U].pts_us <= writer->sample[index].pts_us)
        return 90000U / writer->fps;
    difference = writer->sample[index + 1U].pts_us -
                 writer->sample[index].pts_us;
    ticks = (difference * 90000U + 500000U) / 1000000U;
    return ticks == 0U || ticks > UINT32_MAX ? 90000U / writer->fps
                                             : (uint32_t)ticks;
}

static uint64_t media_duration(const struct ngcd_mp4_writer *writer)
{
    uint64_t duration = 0U;
    size_t index;
    for (index = 0; index < writer->sample_count; ++index)
        duration += sample_duration(writer, index);
    return duration;
}

static int write_matrix(FILE *file)
{
    static const uint32_t matrix[9] = {
        0x00010000U, 0U, 0U, 0U, 0x00010000U, 0U, 0U, 0U, 0x40000000U,
    };
    size_t index;
    for (index = 0; index < 9U; ++index)
        if (write_u32(file, matrix[index]) != 0)
            return -1;
    return 0;
}

static int write_mvhd(FILE *file, uint64_t duration, uint32_t next_track_id)
{
    long box = box_begin(file, "mvhd");
    unsigned char reserved[10] = {0};
    unsigned char tail[24] = {0};
    if (box < 0 || write_u32(file, 0x01000000U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, 1000000U) != 0 || write_u64(file, duration) != 0 ||
        write_u32(file, 0x00010000U) != 0 ||
        write_u16(file, 0x0100U) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_matrix(file) != 0 || write_bytes(file, tail, sizeof(tail)) != 0 ||
        write_u32(file, next_track_id) != 0)
        return -1;
    return box_end(file, box);
}

static int write_edts(FILE *file, uint64_t delay, uint64_t duration)
{
    if (delay == 0U)
        return 0;
    long edts = box_begin(file, "edts");
    long elst;
    if (edts < 0)
        return -1;
    elst = box_begin(file, "elst");
    if (elst < 0 || write_u32(file, 0x01000000U) != 0 ||
        write_u32(file, 2U) != 0)
        return -1;
    if (write_u64(file, delay) != 0 || write_u64(file, UINT64_MAX) != 0 ||
        write_u16(file, 1U) != 0 || write_u16(file, 0U) != 0)
        return -1;
    if (write_u64(file, duration) != 0 || write_u64(file, 0U) != 0 ||
        write_u16(file, 1U) != 0 || write_u16(file, 0U) != 0 ||
        box_end(file, elst) != 0 || box_end(file, edts) != 0)
        return -1;
    return 0;
}

static uint32_t camm_sample_duration(const struct ngcd_mp4_writer *writer,
                                     size_t index)
{
    uint64_t difference;
    if (index + 1U < writer->camm_sample_count &&
        writer->camm_sample[index + 1U].pts_us >
            writer->camm_sample[index].pts_us) {
        difference = writer->camm_sample[index + 1U].pts_us -
                     writer->camm_sample[index].pts_us;
        if (difference <= UINT32_MAX)
            return (uint32_t)difference;
    }
    if (index > 0U && writer->camm_sample[index].pts_us >
                         writer->camm_sample[index - 1U].pts_us) {
        difference = writer->camm_sample[index].pts_us -
                     writer->camm_sample[index - 1U].pts_us;
        if (difference <= UINT32_MAX)
            return (uint32_t)difference;
    }
    return 1000U;
}

static uint64_t camm_duration(const struct ngcd_mp4_writer *writer)
{
    uint64_t duration = 0U;
    size_t index;
    for (index = 0; index < writer->camm_sample_count; ++index)
        duration += camm_sample_duration(writer, index);
    return duration;
}

static int write_tkhd(FILE *file, const struct ngcd_mp4_writer *writer,
                      uint64_t duration)
{
    long box = box_begin(file, "tkhd");
    unsigned char reserved[16] = {0};
    if (box < 0 || write_u32(file, 0x01000007U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, 1U) != 0 || write_u32(file, 0U) != 0 ||
        write_u64(file, duration) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_matrix(file) != 0 ||
        write_u32(file, writer->width << 16U) != 0 ||
        write_u32(file, writer->height << 16U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_mdhd(FILE *file, uint64_t duration)
{
    long box = box_begin(file, "mdhd");
    if (box < 0 || write_u32(file, 0x01000000U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, 90000U) != 0 || write_u64(file, duration) != 0 ||
        write_u16(file, 0x55c4U) != 0 || write_u16(file, 0U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_camm_tkhd(FILE *file, uint64_t duration, uint32_t track_id)
{
    long box = box_begin(file, "tkhd");
    unsigned char reserved[16] = {0};
    if (box < 0 || write_u32(file, 0x01000007U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, track_id) != 0 || write_u32(file, 0U) != 0 ||
        write_u64(file, duration) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_matrix(file) != 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, 0U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_audio_tkhd(FILE *file, uint64_t duration)
{
    long box = box_begin(file, "tkhd");
    unsigned char reserved[8] = {0};
    if (box < 0 || write_u32(file, 0x01000007U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, 2U) != 0 || write_u32(file, 0U) != 0 ||
        write_u64(file, duration) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_u16(file, 0U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0x0100U) != 0 || write_u16(file, 0U) != 0 ||
        write_matrix(file) != 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, 0U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_camm_mdhd(FILE *file, uint64_t duration)
{
    long box = box_begin(file, "mdhd");
    if (box < 0 || write_u32(file, 0x01000000U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, 1000000U) != 0 || write_u64(file, duration) != 0 ||
        write_u16(file, 0x55c4U) != 0 || write_u16(file, 0U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_audio_mdhd(FILE *file,
                            const struct ngcd_mp4_writer *writer,
                            uint64_t duration)
{
    long box = box_begin(file, "mdhd");
    if (box < 0 || write_u32(file, 0x01000000U) != 0 ||
        write_u64(file, 0U) != 0 || write_u64(file, 0U) != 0 ||
        write_u32(file, writer->audio_sample_rate) != 0 ||
        write_u64(file, duration) != 0 || write_u16(file, 0x55c4U) != 0 ||
        write_u16(file, 0U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_hdlr(FILE *file)
{
    static const unsigned char reserved[12] = {0};
    static const char name[] = "CALF Video";
    long box = box_begin(file, "hdlr");
    if (box < 0 || write_u32(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_bytes(file, "vide", 4U) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_bytes(file, name, sizeof(name)) != 0)
        return -1;
    return box_end(file, box);
}

static int write_camm_hdlr(FILE *file)
{
    static const unsigned char reserved[12] = {0};
    static const char name[] = "Camera Motion Metadata";
    long box = box_begin(file, "hdlr");
    if (box < 0 || write_u32(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_bytes(file, "meta", 4U) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_bytes(file, name, sizeof(name)) != 0)
        return -1;
    return box_end(file, box);
}

static int write_audio_hdlr(FILE *file)
{
    static const unsigned char reserved[12] = {0};
    static const char name[] = "CALF Audio";
    long box = box_begin(file, "hdlr");
    if (box < 0 || write_u32(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_bytes(file, "soun", 4U) != 0 ||
        write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_bytes(file, name, sizeof(name)) != 0)
        return -1;
    return box_end(file, box);
}

static int write_avcc(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "avcC");
    unsigned char header[6];
    if (writer->sps_size < 4U || writer->sps_size > UINT16_MAX ||
        writer->pps_size == 0U || writer->pps_size > UINT16_MAX)
        return -1;
    header[0] = 1U;
    header[1] = writer->sps[1];
    header[2] = writer->sps[2];
    header[3] = writer->sps[3];
    header[4] = 0xffU;
    header[5] = 0xe1U;
    if (box < 0 || write_bytes(file, header, sizeof(header)) != 0 ||
        write_u16(file, (uint16_t)writer->sps_size) != 0 ||
        write_bytes(file, writer->sps, writer->sps_size) != 0 ||
        fputc(1, file) == EOF ||
        write_u16(file, (uint16_t)writer->pps_size) != 0 ||
        write_bytes(file, writer->pps, writer->pps_size) != 0)
        return -1;
    /* AVCDecoderConfigurationRecord has four additional fields for the High
     * profiles. The stock muxer writes 4:2:0, 8-bit and no SPS extensions. */
    if ((header[1] == 100U || header[1] == 110U || header[1] == 122U ||
         header[1] == 144U) &&
        (fputc(0xfd, file) == EOF || fputc(0xf8, file) == EOF ||
         fputc(0xf8, file) == EOF || fputc(0, file) == EOF))
        return -1;
    return box_end(file, box);
}

static int write_stereo_metadata(FILE *file)
{
    static const unsigned char st3d_payload[5] = {0U, 0U, 0U, 0U, 2U};
    static const unsigned char sv3d_payload[] = {
        0x00, 0x00, 0x00, 0x0d, 's', 'v', 'h', 'd',
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x3c, 'p', 'r', 'o', 'j',
        0x00, 0x00, 0x00, 0x18, 'p', 'r', 'h', 'd',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x1c, 'e', 'q', 'u', 'i',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x3f, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xff,
    };
    long st3d = box_begin(file, "st3d");
    long sv3d;
    if (st3d < 0 || write_bytes(file, st3d_payload,
                                sizeof(st3d_payload)) != 0 ||
        box_end(file, st3d) != 0)
        return -1;
    sv3d = box_begin(file, "sv3d");
    if (sv3d < 0 || write_bytes(file, sv3d_payload,
                                sizeof(sv3d_payload)) != 0)
        return -1;
    return box_end(file, sv3d);
}

static int write_hvcc_array(FILE *file, unsigned int type,
                            const unsigned char *data, size_t size)
{
    if (data == NULL || size == 0U || size > UINT16_MAX || type > 63U ||
        fputc((int)(0x80U | type), file) == EOF ||
        write_u16(file, 1U) != 0 || write_u16(file, (uint16_t)size) != 0 ||
        write_bytes(file, data, size) != 0)
        return -1;
    return 0;
}

static int write_hvcc(FILE *file, const struct ngcd_mp4_writer *writer)
{
    unsigned char header[23] = {
        1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xf0U, 0U, 0xfcU, 0xfdU, 0xf8U, 0xf8U, 0U, 0U, 0x0fU, 3U,
    };
    long box = box_begin(file, "hvcC");
    if (writer->vps_size == 0U || writer->sps_size < 15U ||
        writer->pps_size == 0U)
        return -1;
    /* The general profile-tier-level fields occupy fixed bytes at the start
     * of an SPS RBSP, before any emulation-prevention sequence can occur. */
    memcpy(header + 1U, writer->sps + 3U, 12U);
    header[21] = (unsigned char)((((writer->sps[2] >> 1U) & 7U) + 1U) << 3U);
    header[21] |= (unsigned char)((writer->sps[2] & 1U) << 2U);
    header[21] |= 3U;
    if (box < 0 || write_bytes(file, header, sizeof(header)) != 0 ||
        write_hvcc_array(file, 32U, writer->vps, writer->vps_size) != 0 ||
        write_hvcc_array(file, 33U, writer->sps, writer->sps_size) != 0 ||
        write_hvcc_array(file, 34U, writer->pps, writer->pps_size) != 0)
        return -1;
    return box_end(file, box);
}

static int write_stsd(FILE *file, const struct ngcd_mp4_writer *writer)
{
    unsigned char reserved6[6] = {0};
    unsigned char reserved16[16] = {0};
    unsigned char compressor[32] = {0};
    long stsd = box_begin(file, "stsd");
    long sample_entry;
    static const char h264_name[] = "CALF H.264";
    static const char h265_name[] = "CALF H.265";
    const char *name = writer->h265 ? h265_name : h264_name;
    size_t name_size = writer->h265 ? sizeof(h265_name) : sizeof(h264_name);
    compressor[0] = (unsigned char)(name_size - 1U);
    memcpy(compressor + 1U, name, name_size - 1U);
    if (stsd < 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, 1U) != 0)
        return -1;
    sample_entry = box_begin(file, writer->h265 ? "hvc1" : "avc1");
    if (sample_entry < 0 ||
        write_bytes(file, reserved6, sizeof(reserved6)) != 0 ||
        write_u16(file, 1U) != 0 ||
        write_bytes(file, reserved16, sizeof(reserved16)) != 0 ||
        write_u16(file, (uint16_t)writer->width) != 0 ||
        write_u16(file, (uint16_t)writer->height) != 0 ||
        write_u32(file, 0x00480000U) != 0 ||
        write_u32(file, 0x00480000U) != 0 || write_u32(file, 0U) != 0 ||
        write_u16(file, 1U) != 0 ||
        write_bytes(file, compressor, sizeof(compressor)) != 0 ||
        write_u16(file, 0x0018U) != 0 || write_u16(file, 0xffffU) != 0 ||
        (writer->h265 ? write_hvcc(file, writer)
                      : write_avcc(file, writer)) != 0 ||
        write_stereo_metadata(file) != 0 ||
        box_end(file, sample_entry) != 0)
        return -1;
    return box_end(file, stsd);
}

static int write_stts(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stts");
    size_t index = 0U;
    uint32_t entries = 0U;
    long entries_offset;
    if (box < 0 || write_u32(file, 0U) != 0)
        return -1;
    entries_offset = ftell(file);
    if (entries_offset < 0 || write_u32(file, 0U) != 0)
        return -1;
    while (index < writer->sample_count) {
        uint32_t duration = sample_duration(writer, index);
        uint32_t count = 1U;
        while (index + count < writer->sample_count &&
               sample_duration(writer, index + count) == duration)
            ++count;
        if (write_u32(file, count) != 0 || write_u32(file, duration) != 0)
            return -1;
        index += count;
        ++entries;
    }
    {
        long end = ftell(file);
        if (end < 0 || fseek(file, entries_offset, SEEK_SET) != 0 ||
            write_u32(file, entries) != 0 || fseek(file, end, SEEK_SET) != 0)
            return -1;
    }
    return box_end(file, box);
}

static int write_stsc(FILE *file)
{
    long box = box_begin(file, "stsc");
    if (box < 0 || write_u32(file, 0U) != 0 || write_u32(file, 1U) != 0 ||
        write_u32(file, 1U) != 0 || write_u32(file, 1U) != 0 ||
        write_u32(file, 1U) != 0)
        return -1;
    return box_end(file, box);
}

static int write_stsz(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stsz");
    size_t index;
    if (box < 0 || writer->sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, (uint32_t)writer->sample_count) != 0)
        return -1;
    for (index = 0; index < writer->sample_count; ++index)
        if (write_u32(file, writer->sample[index].size) != 0)
            return -1;
    return box_end(file, box);
}

static int write_co64(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "co64");
    size_t index;
    if (box < 0 || writer->sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 ||
        write_u32(file, (uint32_t)writer->sample_count) != 0)
        return -1;
    for (index = 0; index < writer->sample_count; ++index)
        if (write_u64(file, writer->sample[index].offset) != 0)
            return -1;
    return box_end(file, box);
}

static int write_stss(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stss");
    uint32_t count = 0U;
    size_t index;
    for (index = 0; index < writer->sample_count; ++index)
        if (writer->sample[index].key_frame)
            ++count;
    if (box < 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, count) != 0)
        return -1;
    for (index = 0; index < writer->sample_count; ++index)
        if (writer->sample[index].key_frame &&
            write_u32(file, (uint32_t)index + 1U) != 0)
            return -1;
    return box_end(file, box);
}

static int write_stbl(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stbl");
    if (box < 0 || write_stsd(file, writer) != 0 ||
        write_stts(file, writer) != 0 || write_stsc(file) != 0 ||
        write_stsz(file, writer) != 0 || write_co64(file, writer) != 0 ||
        write_stss(file, writer) != 0)
        return -1;
    return box_end(file, box);
}

static int write_camm_stsd(FILE *file)
{
    unsigned char reserved[6] = {0};
    long stsd = box_begin(file, "stsd");
    long camm;
    if (stsd < 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, 1U) != 0)
        return -1;
    camm = box_begin(file, "camm");
    if (camm < 0 || write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_u16(file, 1U) != 0 || box_end(file, camm) != 0)
        return -1;
    return box_end(file, stsd);
}

static int write_camm_stts(FILE *file,
                           const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stts");
    size_t index = 0U;
    uint32_t entries = 0U;
    long entries_offset;
    if (box < 0 || write_u32(file, 0U) != 0)
        return -1;
    entries_offset = ftell(file);
    if (entries_offset < 0 || write_u32(file, 0U) != 0)
        return -1;
    while (index < writer->camm_sample_count) {
        uint32_t duration = camm_sample_duration(writer, index);
        uint32_t count = 1U;
        while (index + count < writer->camm_sample_count &&
               camm_sample_duration(writer, index + count) == duration)
            ++count;
        if (write_u32(file, count) != 0 || write_u32(file, duration) != 0)
            return -1;
        index += count;
        ++entries;
    }
    {
        long end = ftell(file);
        if (end < 0 || fseek(file, entries_offset, SEEK_SET) != 0 ||
            write_u32(file, entries) != 0 || fseek(file, end, SEEK_SET) != 0)
            return -1;
    }
    return box_end(file, box);
}

static int write_camm_stsz(FILE *file,
                           const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stsz");
    if (box < 0 || writer->camm_sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 || write_u32(file, 16U) != 0 ||
        write_u32(file, (uint32_t)writer->camm_sample_count) != 0)
        return -1;
    return box_end(file, box);
}

static int write_camm_co64(FILE *file,
                           const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "co64");
    size_t index;
    if (box < 0 || writer->camm_sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 ||
        write_u32(file, (uint32_t)writer->camm_sample_count) != 0)
        return -1;
    for (index = 0; index < writer->camm_sample_count; ++index)
        if (write_u64(file, writer->camm_sample[index].offset) != 0)
            return -1;
    return box_end(file, box);
}

static int write_camm_stbl(FILE *file,
                           const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stbl");
    if (box < 0 || write_camm_stsd(file) != 0 ||
        write_camm_stts(file, writer) != 0 || write_stsc(file) != 0 ||
        write_camm_stsz(file, writer) != 0 ||
        write_camm_co64(file, writer) != 0)
        return -1;
    return box_end(file, box);
}

static uint64_t audio_media_duration(const struct ngcd_mp4_writer *writer)
{
    uint64_t duration = 0U;
    size_t index;
    for (index = 0U; index < writer->audio_sample_count; ++index)
        duration += writer->audio_sample[index].frames;
    return duration;
}

static uint64_t audio_duration_us(uint64_t frames, unsigned int sample_rate)
{
    uint64_t seconds = frames / sample_rate;
    uint64_t remainder = frames % sample_rate;
    if (seconds > UINT64_MAX / UINT64_C(1000000))
        return UINT64_MAX;
    return seconds * UINT64_C(1000000) +
           (remainder * UINT64_C(1000000) + sample_rate / 2U) /
               sample_rate;
}

static int write_audio_stsd(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    unsigned char esds_payload[] = {
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x19, 0x00, 0x00, 0x00,
        0x04, 0x11, 0x40, 0x15, 0x00, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x05, 0x02, 0x11, 0x90,
        0x06, 0x01, 0x02,
    };
    unsigned char reserved[6] = {0};
    long stsd = box_begin(file, "stsd");
    long entry;
    long esds;
    uint16_t audio_specific_config =
        (uint16_t)((2U << 11U) | (3U << 7U) |
                   (writer->audio_channels << 3U));
    esds_payload[26] = (unsigned char)(audio_specific_config >> 8U);
    esds_payload[27] = (unsigned char)audio_specific_config;
    if (stsd < 0 || write_u32(file, 0U) != 0 || write_u32(file, 1U) != 0)
        return -1;
    entry = box_begin(file, writer->audio_aac ? "mp4a" : "sowt");
    if (entry < 0 || write_bytes(file, reserved, sizeof(reserved)) != 0 ||
        write_u16(file, 1U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_u16(file, (uint16_t)writer->audio_channels) != 0 ||
        write_u16(file, 16U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0U) != 0 ||
        write_u32(file, writer->audio_sample_rate << 16U) != 0)
        return -1;
    if (writer->audio_aac) {
        esds = box_begin(file, "esds");
        if (esds < 0 || write_bytes(file, esds_payload,
                                    sizeof(esds_payload)) != 0 ||
            box_end(file, esds) != 0)
            return -1;
    }
    if (box_end(file, entry) != 0)
        return -1;
    return box_end(file, stsd);
}

static int write_audio_stts(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stts");
    size_t index = 0U;
    uint32_t entries = 0U;
    long entries_offset;
    if (box < 0 || write_u32(file, 0U) != 0)
        return -1;
    entries_offset = ftell(file);
    if (entries_offset < 0 || write_u32(file, 0U) != 0)
        return -1;
    while (index < writer->audio_sample_count) {
        uint32_t frames = writer->audio_sample[index].frames;
        uint32_t count = 1U;
        while (index + count < writer->audio_sample_count &&
               writer->audio_sample[index + count].frames == frames)
            ++count;
        if (write_u32(file, count) != 0 || write_u32(file, frames) != 0)
            return -1;
        index += count;
        ++entries;
    }
    {
        long end = ftell(file);
        if (end < 0 || fseek(file, entries_offset, SEEK_SET) != 0 ||
            write_u32(file, entries) != 0 || fseek(file, end, SEEK_SET) != 0)
            return -1;
    }
    return box_end(file, box);
}

static int write_audio_stsz(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stsz");
    size_t index;
    if (box < 0 || writer->audio_sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, (uint32_t)writer->audio_sample_count) != 0)
        return -1;
    for (index = 0U; index < writer->audio_sample_count; ++index)
        if (write_u32(file, writer->audio_sample[index].size) != 0)
            return -1;
    return box_end(file, box);
}

static int write_audio_co64(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "co64");
    size_t index;
    if (box < 0 || writer->audio_sample_count > UINT32_MAX ||
        write_u32(file, 0U) != 0 ||
        write_u32(file, (uint32_t)writer->audio_sample_count) != 0)
        return -1;
    for (index = 0U; index < writer->audio_sample_count; ++index)
        if (write_u64(file, writer->audio_sample[index].offset) != 0)
            return -1;
    return box_end(file, box);
}

static int write_audio_stbl(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    long box = box_begin(file, "stbl");
    if (box < 0 || write_audio_stsd(file, writer) != 0 ||
        write_audio_stts(file, writer) != 0 || write_stsc(file) != 0 ||
        write_audio_stsz(file, writer) != 0 ||
        write_audio_co64(file, writer) != 0)
        return -1;
    return box_end(file, box);
}

static int write_dinf(FILE *file)
{
    long dinf = box_begin(file, "dinf");
    long dref;
    long url;
    if (dinf < 0)
        return -1;
    dref = box_begin(file, "dref");
    if (dref < 0 || write_u32(file, 0U) != 0 ||
        write_u32(file, 1U) != 0)
        return -1;
    url = box_begin(file, "url ");
    if (url < 0 || write_u32(file, 1U) != 0 || box_end(file, url) != 0 ||
        box_end(file, dref) != 0 || box_end(file, dinf) != 0)
        return -1;
    return 0;
}

static int write_minf(FILE *file, const struct ngcd_mp4_writer *writer)
{
    long minf = box_begin(file, "minf");
    long vmhd;
    if (minf < 0)
        return -1;
    vmhd = box_begin(file, "vmhd");
    if (vmhd < 0 || write_u32(file, 1U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0U) != 0 || box_end(file, vmhd) != 0 ||
        write_dinf(file) != 0 || write_stbl(file, writer) != 0)
        return -1;
    return box_end(file, minf);
}

static int write_camm_minf(FILE *file,
                           const struct ngcd_mp4_writer *writer)
{
    long minf = box_begin(file, "minf");
    long nmhd;
    if (minf < 0)
        return -1;
    nmhd = box_begin(file, "nmhd");
    if (nmhd < 0 || write_u32(file, 0U) != 0 || box_end(file, nmhd) != 0 ||
        write_dinf(file) != 0 || write_camm_stbl(file, writer) != 0)
        return -1;
    return box_end(file, minf);
}

static int write_audio_minf(FILE *file,
                            const struct ngcd_mp4_writer *writer)
{
    long minf = box_begin(file, "minf");
    long smhd;
    if (minf < 0)
        return -1;
    smhd = box_begin(file, "smhd");
    if (smhd < 0 || write_u32(file, 0U) != 0 || write_u16(file, 0U) != 0 ||
        write_u16(file, 0U) != 0 || box_end(file, smhd) != 0 ||
        write_dinf(file) != 0 || write_audio_stbl(file, writer) != 0)
        return -1;
    return box_end(file, minf);
}

static int write_camm_trak(FILE *file,
                           const struct ngcd_mp4_writer *writer,
                           uint64_t track_duration, uint64_t start_delay,
                           uint64_t metadata_duration, uint32_t track_id)
{
    long trak = box_begin(file, "trak");
    long mdia;
    if (trak < 0 || write_camm_tkhd(file, track_duration, track_id) != 0 ||
        write_edts(file, start_delay, metadata_duration) != 0)
        return -1;
    mdia = box_begin(file, "mdia");
    if (mdia < 0 || write_camm_mdhd(file, metadata_duration) != 0 ||
        write_camm_hdlr(file) != 0 || write_camm_minf(file, writer) != 0 ||
        box_end(file, mdia) != 0 || box_end(file, trak) != 0)
        return -1;
    return 0;
}

static int write_audio_trak(FILE *file,
                            const struct ngcd_mp4_writer *writer,
                            uint64_t track_duration, uint64_t start_delay,
                            uint64_t media_duration_us,
                            uint64_t media_duration_samples)
{
    long trak = box_begin(file, "trak");
    long mdia;
    if (trak < 0 || write_audio_tkhd(file, track_duration) != 0 ||
        write_edts(file, start_delay, media_duration_us) != 0)
        return -1;
    mdia = box_begin(file, "mdia");
    if (mdia < 0 ||
        write_audio_mdhd(file, writer, media_duration_samples) != 0 ||
        write_audio_hdlr(file) != 0 || write_audio_minf(file, writer) != 0 ||
        box_end(file, mdia) != 0 || box_end(file, trak) != 0)
        return -1;
    return 0;
}

static void trim_auxiliary_samples_to_video(
    struct ngcd_mp4_writer *writer, uint64_t video_duration_us)
{
    uint64_t first_pts = writer->sample[0].pts_us;
    uint64_t end_pts = video_duration_us > UINT64_MAX - first_pts
                           ? UINT64_MAX : first_pts + video_duration_us;
    size_t source;
    size_t destination = 0U;
    for (source = 0U; source < writer->camm_sample_count; ++source)
        if (writer->camm_sample[source].pts_us >= first_pts &&
            writer->camm_sample[source].pts_us < end_pts)
            writer->camm_sample[destination++] = writer->camm_sample[source];
    writer->camm_sample_count = destination;
    destination = 0U;
    for (source = 0U; source < writer->audio_sample_count; ++source)
        if (writer->audio_sample[source].pts_us >= first_pts &&
            writer->audio_sample[source].pts_us < end_pts)
            writer->audio_sample[destination++] =
                writer->audio_sample[source];
    writer->audio_sample_count = destination;
}

static int write_moov(struct ngcd_mp4_writer *writer)
{
    uint64_t media;
    uint64_t video_media_us;
    uint64_t metadata;
    uint64_t audio_samples;
    uint64_t audio_media_us;
    uint64_t first_pts;
    uint64_t video_delay = 0U;
    uint64_t metadata_delay = 0U;
    uint64_t audio_delay = 0U;
    uint64_t video_track;
    uint64_t metadata_track = 0U;
    uint64_t audio_track = 0U;
    uint64_t movie;
    uint32_t next_track_id = 2U;
    FILE *file = writer->file;
    long moov;
    long trak;
    long mdia;

    /* Recovery journals made by older builds may contain auxiliary samples
     * outside the video interval. Leave their mdat bytes orphaned and keep
     * them out of the rebuilt track tables. Video owns the movie timeline. */
    media = media_duration(writer);
    video_media_us = (media * 1000000U + 45000U) / 90000U;
    trim_auxiliary_samples_to_video(writer, video_media_us);
    metadata = camm_duration(writer);
    audio_samples = audio_media_duration(writer);
    audio_media_us = writer->audio_sample_count > 0U
                         ? audio_duration_us(audio_samples,
                                             writer->audio_sample_rate)
                         : 0U;
    first_pts = writer->sample[0].pts_us;
    moov = box_begin(file, "moov");
    video_track = video_delay + video_media_us;
    if (writer->camm_sample_count > 0U) {
        metadata_delay = writer->camm_sample[0].pts_us - first_pts;
        metadata_track = metadata_delay + metadata;
    }
    if (writer->audio_sample_count > 0U) {
        audio_delay = writer->audio_sample[0].pts_us - first_pts;
        audio_track = audio_delay + audio_media_us;
        ++next_track_id;
    }
    if (writer->camm_sample_count > 0U)
        ++next_track_id;
    movie = video_track > metadata_track ? video_track : metadata_track;
    if (audio_track > movie)
        movie = audio_track;
    if (moov < 0 || write_mvhd(file, movie, next_track_id) != 0)
        return -1;
    trak = box_begin(file, "trak");
    if (trak < 0 || write_tkhd(file, writer, video_track) != 0 ||
        write_edts(file, video_delay, video_media_us) != 0)
        return -1;
    mdia = box_begin(file, "mdia");
    if (mdia < 0 || write_mdhd(file, media) != 0 || write_hdlr(file) != 0 ||
        write_minf(file, writer) != 0 || box_end(file, mdia) != 0 ||
        box_end(file, trak) != 0 ||
        (writer->audio_sample_count > 0U &&
         write_audio_trak(file, writer, audio_track, audio_delay,
                          audio_media_us, audio_samples) != 0) ||
        (writer->camm_sample_count > 0U &&
         write_camm_trak(file, writer, metadata_track, metadata_delay,
                         metadata,
                         writer->audio_sample_count > 0U ? 3U : 2U) != 0) ||
        box_end(file, moov) != 0)
        return -1;
    return 0;
}

static void free_writer(struct ngcd_mp4_writer *writer);

static int recover_video_sample(struct ngcd_mp4_writer *writer,
                                uint64_t offset, uint32_t size,
                                uint64_t pts_us, bool key_frame)
{
    unsigned char *data;
    size_t cursor = 0U;
    struct mp4_sample sample;
    if (size == 0U || size > UINT32_C(64) * 1024U * 1024U ||
        offset > (uint64_t)INT64_MAX || reserve_sample(writer) != 0)
        return -1;
    data = malloc(size);
    if (data == NULL || fseek(writer->file, (long)offset, SEEK_SET) != 0 ||
        read_bytes(writer->file, data, size) != 0) {
        free(data);
        return -1;
    }
    while (cursor < size) {
        uint32_t nal_size;
        unsigned int type;
        if (size - cursor < 4U) {
            free(data);
            return -1;
        }
        nal_size = ((uint32_t)data[cursor] << 24U) |
                   ((uint32_t)data[cursor + 1U] << 16U) |
                   ((uint32_t)data[cursor + 2U] << 8U) |
                   data[cursor + 3U];
        cursor += 4U;
        if (nal_size == 0U || nal_size > size - cursor) {
            free(data);
            return -1;
        }
        type = writer->h265 ? (data[cursor] >> 1U) & 0x3fU
                            : data[cursor] & 0x1fU;
        if ((writer->h265 && type == 32U &&
             copy_parameter_set(&writer->vps, &writer->vps_size,
                                data + cursor, nal_size) != 0) ||
            (type == (writer->h265 ? 33U : 7U) &&
             copy_parameter_set(&writer->sps, &writer->sps_size,
                                data + cursor, nal_size) != 0) ||
            (type == (writer->h265 ? 34U : 8U) &&
             copy_parameter_set(&writer->pps, &writer->pps_size,
                                data + cursor, nal_size) != 0)) {
            free(data);
            return -1;
        }
        cursor += nal_size;
    }
    free(data);
    memset(&sample, 0, sizeof(sample));
    sample.offset = offset;
    sample.pts_us = pts_us;
    sample.size = size;
    sample.key_frame = key_frame;
    writer->sample[writer->sample_count++] = sample;
    return 0;
}

static void close_recovery_writer(struct ngcd_mp4_writer *writer)
{
    if (writer == NULL)
        return;
    if (writer->file != NULL)
        (void)fclose(writer->file);
    if (writer->journal != NULL)
        (void)fclose(writer->journal);
    writer->file = NULL;
    writer->journal = NULL;
}

int ngcd_mp4_recover(const char *temporary_path)
{
    static const unsigned char expected_magic[8] = {
        'N', 'G', 'C', 'D', 'M', 'P', '4', 'J',
    };
    struct ngcd_mp4_writer *writer = NULL;
    unsigned char magic[8];
    unsigned char mdat[8];
    uint32_t version;
    uint32_t codec;
    uint32_t header_size;
    uint64_t file_size;
    uint64_t valid_end = 48U;
    uint64_t last_end = 48U;
    size_t path_length;
    long end;
    int result = -1;
    if (temporary_path == NULL || temporary_path[0] == '\0')
        return -1;
    writer = calloc(1U, sizeof(*writer));
    if (writer == NULL)
        return -1;
    path_length = strlen(temporary_path);
    if (path_length > SIZE_MAX - 5U)
        goto done;
    writer->path = malloc(path_length + 1U);
    writer->journal_path = malloc(path_length + 5U);
    if (writer->path == NULL || writer->journal_path == NULL)
        goto done;
    memcpy(writer->path, temporary_path, path_length + 1U);
    memcpy(writer->journal_path, temporary_path, path_length);
    memcpy(writer->journal_path + path_length, ".idx", 5U);
    writer->file = fopen(writer->path, "rb+");
    writer->journal = fopen(writer->journal_path, "rb");
    if (writer->file == NULL || writer->journal == NULL ||
        read_bytes(writer->journal, magic, sizeof(magic)) != 0 ||
        memcmp(magic, expected_magic, sizeof(magic)) != 0 ||
        read_u32(writer->journal, &version) != 0 || version != 1U ||
        read_u32(writer->journal, &codec) != 0 ||
        (codec != 1U && codec != 2U) ||
        read_u32(writer->journal, &writer->width) != 0 ||
        read_u32(writer->journal, &writer->height) != 0 ||
        read_u32(writer->journal, &writer->fps) != 0 ||
        read_u32(writer->journal, &header_size) != 0 ||
        header_size != JOURNAL_HEADER_SIZE || writer->width == 0U ||
        writer->width > UINT16_MAX || writer->height == 0U ||
        writer->height > UINT16_MAX || writer->fps == 0U ||
        writer->fps > 240U || fseek(writer->file, 32L, SEEK_SET) != 0 ||
        read_bytes(writer->file, mdat, sizeof(mdat)) != 0 ||
        mdat[0] != 0U || mdat[1] != 0U || mdat[2] != 0U ||
        mdat[3] != 1U || memcmp(mdat + 4U, "mdat", 4U) != 0 ||
        fseek(writer->file, 0L, 2) != 0 ||
        (end = ftell(writer->file)) < 48L)
        goto done;
    writer->h265 = codec == 2U;
    writer->mdat_offset = 32U;
    file_size = (uint64_t)end;
    for (;;) {
        uint32_t kind;
        uint32_t flags;
        uint32_t size;
        uint32_t record_size;
        uint64_t offset;
        uint64_t pts_us;
        unsigned char kind_bytes[4];
        size_t kind_count = fread(kind_bytes, 1U, sizeof(kind_bytes),
                                  writer->journal);
        if (kind_count == 0U)
            break;
        if (kind_count != sizeof(kind_bytes))
            break;
        kind = ((uint32_t)kind_bytes[0] << 24U) |
               ((uint32_t)kind_bytes[1] << 16U) |
               ((uint32_t)kind_bytes[2] << 8U) | kind_bytes[3];
        if (
            read_u32(writer->journal, &flags) != 0 ||
            read_u64(writer->journal, &offset) != 0 ||
            read_u64(writer->journal, &pts_us) != 0 ||
            read_u32(writer->journal, &size) != 0 ||
            read_u32(writer->journal, &record_size) != 0)
            break;
        if (record_size != JOURNAL_RECORD_SIZE)
            break;
        if (kind == JOURNAL_AUDIO_CONFIG ||
            kind == JOURNAL_AUDIO_CONFIG_AAC) {
            if (writer->audio_sample_rate != 0U ||
                (flags != 1U && flags != 2U) || offset < 8000U ||
                offset > 192000U || pts_us != 0U || size != 0U)
                break;
            writer->audio_channels = flags;
            writer->audio_sample_rate = (unsigned int)offset;
            writer->audio_aac = kind == JOURNAL_AUDIO_CONFIG_AAC;
            continue;
        }
        if ((kind != JOURNAL_VIDEO && kind != JOURNAL_CAMM &&
             kind != JOURNAL_AUDIO) || size == 0U ||
            offset < last_end || offset > UINT64_MAX - size ||
            offset + size > file_size)
            break;
        if (kind == JOURNAL_VIDEO) {
            if ((writer->sample_count > 0U &&
                 pts_us < writer->sample[writer->sample_count - 1U].pts_us) ||
                recover_video_sample(writer, offset, size, pts_us,
                                     (flags & 1U) != 0U) != 0)
                goto done;
        } else if (kind == JOURNAL_CAMM) {
            struct camm_sample sample;
            if (size != 16U ||
                (writer->camm_sample_count > 0U &&
                 pts_us < writer->camm_sample[
                              writer->camm_sample_count - 1U].pts_us) ||
                reserve_camm_sample(writer) != 0)
                goto done;
            sample.offset = offset;
            sample.pts_us = pts_us;
            writer->camm_sample[writer->camm_sample_count++] = sample;
        } else {
            struct audio_sample sample;
            uint64_t expected_size;
            expected_size = (uint64_t)flags * writer->audio_channels * 2U;
            if (writer->audio_sample_rate == 0U || flags == 0U ||
                (!writer->audio_aac && expected_size != size) ||
                (writer->audio_aac &&
                 flags != NGCD_AAC_FRAME_SAMPLES) ||
                (writer->audio_sample_count > 0U &&
                 pts_us < writer->audio_sample[
                              writer->audio_sample_count - 1U].pts_us) ||
                reserve_audio_sample(writer) != 0)
                goto done;
            sample.offset = offset;
            sample.pts_us = pts_us;
            sample.size = size;
            sample.frames = flags;
            writer->audio_sample[writer->audio_sample_count++] = sample;
        }
        last_end = offset + size;
        valid_end = last_end;
    }
    if (writer->sample_count == 0U || writer->sps == NULL ||
        writer->pps == NULL || (writer->h265 && writer->vps == NULL) ||
        valid_end > (uint64_t)INT64_MAX ||
        ftruncate(fileno(writer->file), (off_t)valid_end) != 0 ||
        fseek(writer->file, (long)valid_end, SEEK_SET) != 0)
        goto done;
    result = ngcd_mp4_close(writer);
    writer = NULL;
done:
    if (writer != NULL) {
        close_recovery_writer(writer);
        free_writer(writer);
    }
    return result;
}

static void free_writer(struct ngcd_mp4_writer *writer)
{
    if (writer == NULL)
        return;
    free(writer->sample);
    free(writer->camm_sample);
    free(writer->audio_sample);
    ngcd_aac_encoder_close(writer->audio_encoder);
    free(writer->sps);
    free(writer->pps);
    free(writer->vps);
    free(writer->path);
    free(writer->journal_path);
    free(writer);
}

int ngcd_mp4_close(struct ngcd_mp4_writer *writer)
{
    long end;
    uint64_t mdat_size;
    int result = -1;
    if (writer == NULL || writer->file == NULL || writer->failed ||
        writer->sample_count == 0U || writer->sps == NULL ||
        writer->pps == NULL || (writer->h265 && writer->vps == NULL))
        goto done;
    if (writer->audio_encoder != NULL && writer->audio_pcm_size > 0U) {
        size_t frame_bytes = NGCD_AAC_FRAME_SAMPLES *
                             writer->audio_channels * 2U;
        memset(writer->audio_pcm + writer->audio_pcm_size, 0,
               frame_bytes - writer->audio_pcm_size);
        writer->audio_pcm_size = frame_bytes;
        if (write_aac_frame(writer) != 0)
            goto done;
    }
    end = ftell(writer->file);
    if (end < 0 || (uint64_t)end < writer->mdat_offset ||
        (mdat_size = (uint64_t)end - writer->mdat_offset) < 16U)
        goto done;
    if (fseek(writer->file, (long)writer->mdat_offset + 8L, SEEK_SET) != 0 ||
        write_u64(writer->file, mdat_size) != 0 ||
        fseek(writer->file, end, SEEK_SET) != 0 || write_moov(writer) != 0 ||
        fflush(writer->file) != 0 || fdatasync(fileno(writer->file)) != 0)
        goto done;
    if (fclose(writer->file) != 0) {
        writer->file = NULL;
        goto done;
    }
    writer->file = NULL;
    result = 0;
done:
    if (writer != NULL && writer->file != NULL) {
        (void)fclose(writer->file);
        writer->file = NULL;
    }
    if (writer != NULL && writer->journal != NULL) {
        (void)fclose(writer->journal);
        writer->journal = NULL;
    }
    if (result != 0 && writer != NULL && writer->path != NULL)
        (void)unlink(writer->path);
    if (writer != NULL && writer->journal_path != NULL)
        (void)unlink(writer->journal_path);
    free_writer(writer);
    return result;
}

void ngcd_mp4_abort(struct ngcd_mp4_writer *writer)
{
    if (writer == NULL)
        return;
    if (writer->file != NULL)
        (void)fclose(writer->file);
    if (writer->journal != NULL)
        (void)fclose(writer->journal);
    if (writer->path != NULL)
        (void)unlink(writer->path);
    if (writer->journal_path != NULL)
        (void)unlink(writer->journal_path);
    free_writer(writer);
}
