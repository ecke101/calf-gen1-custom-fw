#include "ngcd_playback.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MP4_MOOV_LIMIT (64U * 1024U * 1024U)
#define MP4_SAMPLE_LIMIT 10000000U
#define JPEG_FILE_LIMIT (128U * 1024U * 1024U)

struct mp4_box {
    const unsigned char *payload;
    size_t payload_size;
};

struct stsc_entry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t description;
};

struct ngcd_mp4_reader {
    FILE *file;
    struct ngcd_playback_sample *samples;
    unsigned char *decoder_config;
    size_t decoder_config_size;
    size_t sample_count;
    size_t max_sample_size;
    uint64_t duration_us;
    uint64_t file_size;
    uint64_t create_time;
    unsigned int width;
    unsigned int height;
    unsigned int nal_length_size;
    enum ngcd_playback_codec codec;
    struct ngcd_playback_sample *audio_samples;
    size_t audio_sample_count;
    size_t audio_max_sample_size;
    unsigned int audio_channels;
    unsigned int audio_sample_rate;
    unsigned int audio_object_type;
    enum ngcd_playback_audio_codec audio_codec;
};

uint64_t ngcd_file_create_time(const char *path)
{
    struct statx status;
    if (path == NULL ||
        statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW,
              STATX_BTIME | STATX_MTIME, &status) != 0)
        return 0U;
    if ((status.stx_mask & STATX_BTIME) != 0U &&
        status.stx_btime.tv_sec > 0)
        return (uint64_t)status.stx_btime.tv_sec;
    return (status.stx_mask & STATX_MTIME) != 0U &&
                   status.stx_mtime.tv_sec > 0
               ? (uint64_t)status.stx_mtime.tv_sec : 0U;
}

int ngcd_jpeg_probe(const char *path, unsigned int *width,
                    unsigned int *height, uint64_t *file_size)
{
    unsigned char marker[4];
    FILE *file;
    long length;
    if (path == NULL || width == NULL || height == NULL || file_size == NULL)
        return -1;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0)
        goto fail;
    length = ftell(file);
    if (length <= 0 || length > JPEG_FILE_LIMIT ||
        fseek(file, 0L, SEEK_SET) != 0 ||
        fread(marker, 1U, 2U, file) != 2U || marker[0] != 0xffU ||
        marker[1] != 0xd8U)
        goto fail;
    for (;;) {
        int prefix;
        int code;
        uint16_t segment_length;
        do {
            prefix = fgetc(file);
        } while (prefix != EOF && prefix != 0xff);
        if (prefix == EOF)
            goto fail;
        do {
            code = fgetc(file);
        } while (code == 0xff);
        if (code == EOF || code == 0xda || code == 0xd9)
            goto fail;
        if (code == 0x01 || (code >= 0xd0 && code <= 0xd7))
            continue;
        if (fread(marker, 1U, 2U, file) != 2U)
            goto fail;
        segment_length = (uint16_t)(((uint16_t)marker[0] << 8U) | marker[1]);
        if (segment_length < 2U)
            goto fail;
        if ((code >= 0xc0 && code <= 0xc3) ||
            (code >= 0xc5 && code <= 0xc7) ||
            (code >= 0xc9 && code <= 0xcb) ||
            (code >= 0xcd && code <= 0xcf)) {
            unsigned char dimensions[5];
            if (segment_length < 7U ||
                fread(dimensions, 1U, 5U, file) != 5U)
                goto fail;
            *height = ((unsigned int)dimensions[1] << 8U) | dimensions[2];
            *width = ((unsigned int)dimensions[3] << 8U) | dimensions[4];
            *file_size = (uint64_t)length;
            if (fclose(file) != 0)
                return -1;
            return *width > 0U && *height > 0U ? 0 : -1;
        }
        if (fseek(file, (long)segment_length - 2L, SEEK_CUR) != 0)
            goto fail;
    }

fail:
    if (file != NULL)
        (void)fclose(file);
    return -1;
}

static uint16_t get_u16(const unsigned char *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t get_u32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static uint64_t get_u64(const unsigned char *data)
{
    return ((uint64_t)get_u32(data) << 32U) | get_u32(data + 4U);
}

static int box_next(const unsigned char *data, size_t size, size_t *cursor,
                    char type[4], struct mp4_box *box)
{
    size_t position = *cursor;
    uint64_t box_size;
    size_t header = 8U;
    if (position > size || size - position < 8U)
        return 0;
    box_size = get_u32(data + position);
    memcpy(type, data + position + 4U, 4U);
    if (box_size == 1U) {
        if (size - position < 16U)
            return -1;
        box_size = get_u64(data + position + 8U);
        header = 16U;
    } else if (box_size == 0U) {
        box_size = size - position;
    }
    if (box_size < header || box_size > size - position)
        return -1;
    box->payload = data + position + header;
    box->payload_size = (size_t)box_size - header;
    *cursor = position + (size_t)box_size;
    return 1;
}

static int box_find(const unsigned char *data, size_t size,
                    const char wanted[4], struct mp4_box *found)
{
    size_t cursor = 0U;
    struct mp4_box box;
    char type[4];
    int result;
    while ((result = box_next(data, size, &cursor, type, &box)) > 0) {
        if (memcmp(type, wanted, 4U) == 0) {
            *found = box;
            return 1;
        }
    }
    return result;
}

static int append_start_code(struct ngcd_mp4_reader *reader,
                             const unsigned char *nal, size_t size)
{
    unsigned char *resized;
    size_t next;
    if (size == 0U || reader->decoder_config_size > SIZE_MAX - 4U - size)
        return -1;
    next = reader->decoder_config_size + 4U + size;
    resized = realloc(reader->decoder_config, next);
    if (resized == NULL)
        return -1;
    reader->decoder_config = resized;
    resized[reader->decoder_config_size] = 0U;
    resized[reader->decoder_config_size + 1U] = 0U;
    resized[reader->decoder_config_size + 2U] = 0U;
    resized[reader->decoder_config_size + 3U] = 1U;
    memcpy(resized + reader->decoder_config_size + 4U, nal, size);
    reader->decoder_config_size = next;
    return 0;
}

static int parse_avcc(struct ngcd_mp4_reader *reader,
                      const struct mp4_box *box)
{
    const unsigned char *data = box->payload;
    size_t size = box->payload_size;
    size_t cursor = 6U;
    unsigned int sps_count;
    unsigned int index;
    if (size < 7U || data[0] != 1U)
        return -1;
    reader->nal_length_size = (data[4] & 3U) + 1U;
    sps_count = data[5] & 0x1fU;
    for (index = 0U; index < sps_count; ++index) {
        uint16_t length;
        if (cursor > size || size - cursor < 2U)
            return -1;
        length = get_u16(data + cursor);
        cursor += 2U;
        if (length == 0U || length > size - cursor ||
            append_start_code(reader, data + cursor, length) != 0)
            return -1;
        cursor += length;
    }
    if (cursor >= size)
        return -1;
    sps_count = data[cursor++];
    for (index = 0U; index < sps_count; ++index) {
        uint16_t length;
        if (cursor > size || size - cursor < 2U)
            return -1;
        length = get_u16(data + cursor);
        cursor += 2U;
        if (length == 0U || length > size - cursor ||
            append_start_code(reader, data + cursor, length) != 0)
            return -1;
        cursor += length;
    }
    return reader->decoder_config_size > 0U ? 0 : -1;
}

static int parse_hvcc(struct ngcd_mp4_reader *reader,
                      const struct mp4_box *box)
{
    const unsigned char *data = box->payload;
    size_t size = box->payload_size;
    size_t cursor = 23U;
    unsigned int array_count;
    unsigned int array;
    if (size < 23U || data[0] != 1U)
        return -1;
    reader->nal_length_size = (data[21] & 3U) + 1U;
    array_count = data[22];
    for (array = 0U; array < array_count; ++array) {
        unsigned int nal_count;
        unsigned int index;
        if (cursor > size || size - cursor < 3U)
            return -1;
        ++cursor;
        nal_count = get_u16(data + cursor);
        cursor += 2U;
        for (index = 0U; index < nal_count; ++index) {
            uint16_t length;
            if (cursor > size || size - cursor < 2U)
                return -1;
            length = get_u16(data + cursor);
            cursor += 2U;
            if (length == 0U || length > size - cursor ||
                append_start_code(reader, data + cursor, length) != 0)
                return -1;
            cursor += length;
        }
    }
    return reader->decoder_config_size > 0U ? 0 : -1;
}

static int parse_stsd(struct ngcd_mp4_reader *reader,
                      const struct mp4_box *stsd)
{
    struct mp4_box entry;
    struct mp4_box configuration;
    char type[4];
    size_t cursor = 8U;
    int result;
    if (stsd->payload_size < 8U || get_u32(stsd->payload + 4U) == 0U ||
        (result = box_next(stsd->payload, stsd->payload_size, &cursor,
                           type, &entry)) <= 0 || entry.payload_size < 78U)
        return -1;
    if (memcmp(type, "avc1", 4U) == 0 || memcmp(type, "avc3", 4U) == 0)
        reader->codec = NGCD_PLAYBACK_H264;
    else if (memcmp(type, "hvc1", 4U) == 0 ||
             memcmp(type, "hev1", 4U) == 0)
        reader->codec = NGCD_PLAYBACK_H265;
    else
        return -1;
    reader->width = get_u16(entry.payload + 24U);
    reader->height = get_u16(entry.payload + 26U);
    if (reader->width == 0U || reader->height == 0U)
        return -1;
    result = box_find(entry.payload + 78U, entry.payload_size - 78U,
                      reader->codec == NGCD_PLAYBACK_H264 ? "avcC" : "hvcC",
                      &configuration);
    if (result != 1)
        return -1;
    return reader->codec == NGCD_PLAYBACK_H264
               ? parse_avcc(reader, &configuration)
               : parse_hvcc(reader, &configuration);
}

static int parse_sizes(struct ngcd_mp4_reader *reader,
                       const struct mp4_box *stsz)
{
    uint32_t fixed_size;
    uint32_t count;
    size_t index;
    if (stsz->payload_size < 12U)
        return -1;
    fixed_size = get_u32(stsz->payload + 4U);
    count = get_u32(stsz->payload + 8U);
    if (count == 0U || count > MP4_SAMPLE_LIMIT ||
        (fixed_size == 0U &&
         (size_t)count > (stsz->payload_size - 12U) / 4U))
        return -1;
    reader->samples = calloc((size_t)count, sizeof(*reader->samples));
    if (reader->samples == NULL)
        return -1;
    reader->sample_count = count;
    for (index = 0U; index < count; ++index) {
        uint32_t size = fixed_size != 0U
                            ? fixed_size
                            : get_u32(stsz->payload + 12U + index * 4U);
        if (size == 0U)
            return -1;
        reader->samples[index].size = size;
        if (size > reader->max_sample_size)
            reader->max_sample_size = size;
    }
    return 0;
}

static int parse_timing(struct ngcd_mp4_reader *reader,
                        const struct mp4_box *stts, uint32_t timescale)
{
    uint32_t entry_count;
    size_t entry;
    size_t sample = 0U;
    uint64_t ticks = 0U;
    if (timescale == 0U || stts->payload_size < 8U)
        return -1;
    entry_count = get_u32(stts->payload + 4U);
    if ((size_t)entry_count > (stts->payload_size - 8U) / 8U)
        return -1;
    for (entry = 0U; entry < entry_count; ++entry) {
        uint32_t count = get_u32(stts->payload + 8U + entry * 8U);
        uint32_t delta = get_u32(stts->payload + 12U + entry * 8U);
        uint32_t index;
        if (count == 0U || delta == 0U || count > reader->sample_count - sample)
            return -1;
        for (index = 0U; index < count; ++index) {
            reader->samples[sample++].pts_us =
                ticks > UINT64_MAX / UINT64_C(1000000)
                    ? UINT64_MAX
                    : ticks * UINT64_C(1000000) / timescale;
            if (ticks > UINT64_MAX - delta)
                return -1;
            ticks += delta;
        }
    }
    if (sample != reader->sample_count ||
        ticks > UINT64_MAX / UINT64_C(1000000))
        return -1;
    reader->duration_us = ticks * UINT64_C(1000000) / timescale;
    return 0;
}

static int parse_offsets(struct ngcd_mp4_reader *reader,
                         const struct mp4_box *chunks, bool chunks_64,
                         const struct mp4_box *stsc)
{
    struct stsc_entry *entries;
    uint32_t chunk_count;
    uint32_t entry_count;
    size_t entry_index = 0U;
    size_t sample_index = 0U;
    uint32_t chunk;
    size_t chunk_width = chunks_64 ? 8U : 4U;
    if (chunks->payload_size < 8U || stsc->payload_size < 8U)
        return -1;
    chunk_count = get_u32(chunks->payload + 4U);
    entry_count = get_u32(stsc->payload + 4U);
    if (chunk_count == 0U || entry_count == 0U ||
        (size_t)chunk_count > (chunks->payload_size - 8U) / chunk_width ||
        (size_t)entry_count > (stsc->payload_size - 8U) / 12U)
        return -1;
    entries = malloc((size_t)entry_count * sizeof(*entries));
    if (entries == NULL)
        return -1;
    for (chunk = 0U; chunk < entry_count; ++chunk) {
        entries[chunk].first_chunk =
            get_u32(stsc->payload + 8U + (size_t)chunk * 12U);
        entries[chunk].samples_per_chunk =
            get_u32(stsc->payload + 12U + (size_t)chunk * 12U);
        entries[chunk].description =
            get_u32(stsc->payload + 16U + (size_t)chunk * 12U);
        if (entries[chunk].first_chunk == 0U ||
            entries[chunk].samples_per_chunk == 0U ||
            entries[chunk].description == 0U ||
            (chunk > 0U && entries[chunk].first_chunk <=
                               entries[chunk - 1U].first_chunk)) {
            free(entries);
            return -1;
        }
    }
    if (entries[0].first_chunk != 1U) {
        free(entries);
        return -1;
    }
    for (chunk = 1U; chunk <= chunk_count; ++chunk) {
        uint64_t offset;
        uint32_t within;
        while (entry_index + 1U < entry_count &&
               entries[entry_index + 1U].first_chunk <= chunk)
            ++entry_index;
        offset = chunks_64
                     ? get_u64(chunks->payload + 8U +
                               (size_t)(chunk - 1U) * 8U)
                     : get_u32(chunks->payload + 8U +
                               (size_t)(chunk - 1U) * 4U);
        for (within = 0U; within < entries[entry_index].samples_per_chunk;
             ++within) {
            uint32_t size;
            if (sample_index >= reader->sample_count) {
                free(entries);
                return -1;
            }
            size = reader->samples[sample_index].size;
            if (offset > reader->file_size || size > reader->file_size - offset) {
                free(entries);
                return -1;
            }
            reader->samples[sample_index++].offset = offset;
            offset += size;
        }
    }
    free(entries);
    return sample_index == reader->sample_count ? 0 : -1;
}

static int parse_sync(struct ngcd_mp4_reader *reader,
                      const struct mp4_box *stss, bool present)
{
    size_t index;
    if (!present) {
        for (index = 0U; index < reader->sample_count; ++index)
            reader->samples[index].key_frame = true;
        return 0;
    }
    if (stss->payload_size < 8U)
        return -1;
    {
        uint32_t count = get_u32(stss->payload + 4U);
        if (count == 0U ||
            (size_t)count > (stss->payload_size - 8U) / 4U)
            return -1;
        for (index = 0U; index < count; ++index) {
            uint32_t number = get_u32(stss->payload + 8U + index * 4U);
            if (number == 0U || number > reader->sample_count)
                return -1;
            reader->samples[number - 1U].key_frame = true;
        }
    }
    /* A few early replacement recordings contain queued P-frames before the
     * first requested IDR.  They are structurally valid and become fully
     * decodable at their first stss entry, so retain them for robust playback
     * instead of rejecting the entire file. */
    return 0;
}

static int parse_video_track(struct ngcd_mp4_reader *reader,
                             const struct mp4_box *trak)
{
    struct mp4_box mdia;
    struct mp4_box hdlr;
    struct mp4_box mdhd;
    struct mp4_box minf;
    struct mp4_box stbl;
    struct mp4_box stsd;
    struct mp4_box stts;
    struct mp4_box stsc;
    struct mp4_box stsz;
    struct mp4_box chunks;
    struct mp4_box stss;
    uint32_t timescale;
    bool chunks_64 = false;
    int sync_result;
    if (box_find(trak->payload, trak->payload_size, "mdia", &mdia) != 1 ||
        box_find(mdia.payload, mdia.payload_size, "hdlr", &hdlr) != 1 ||
        hdlr.payload_size < 12U ||
        memcmp(hdlr.payload + 8U, "vide", 4U) != 0)
        return 1;
    if (box_find(mdia.payload, mdia.payload_size, "mdhd", &mdhd) != 1 ||
        mdhd.payload_size < 20U)
        return -1;
    if (mdhd.payload[0] == 1U) {
        if (mdhd.payload_size < 32U)
            return -1;
        timescale = get_u32(mdhd.payload + 20U);
    } else {
        timescale = get_u32(mdhd.payload + 12U);
    }
    if (box_find(mdia.payload, mdia.payload_size, "minf", &minf) != 1 ||
        box_find(minf.payload, minf.payload_size, "stbl", &stbl) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsd", &stsd) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stts", &stts) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsc", &stsc) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsz", &stsz) != 1)
        return -1;
    if (box_find(stbl.payload, stbl.payload_size, "co64", &chunks) == 1)
        chunks_64 = true;
    else if (box_find(stbl.payload, stbl.payload_size, "stco", &chunks) != 1)
        return -1;
    sync_result = box_find(stbl.payload, stbl.payload_size, "stss", &stss);
    if (sync_result < 0 || parse_stsd(reader, &stsd) != 0 ||
        parse_sizes(reader, &stsz) != 0 ||
        parse_timing(reader, &stts, timescale) != 0 ||
        parse_offsets(reader, &chunks, chunks_64, &stsc) != 0 ||
        parse_sync(reader, &stss, sync_result == 1) != 0)
        return -1;
    return 0;
}

static int parse_audio_stsd(struct ngcd_mp4_reader *reader,
                            const struct mp4_box *stsd)
{
    static const unsigned int sample_rates[] = {
        96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
        22050U, 16000U, 12000U, 11025U, 8000U, 7350U,
    };
    struct mp4_box entry;
    struct mp4_box esds;
    char type[4];
    size_t cursor = 8U;
    size_t offset;
    int result;
    if (stsd->payload_size < 8U || get_u32(stsd->payload + 4U) == 0U ||
        (result = box_next(stsd->payload, stsd->payload_size, &cursor,
                           type, &entry)) <= 0 || entry.payload_size < 28U)
        return -1;
    if (memcmp(type, "sowt", 4U) == 0)
        reader->audio_codec = NGCD_PLAYBACK_AUDIO_PCM_S16LE;
    else if (memcmp(type, "mp4a", 4U) == 0)
        reader->audio_codec = NGCD_PLAYBACK_AUDIO_AAC;
    else
        return 1;
    reader->audio_channels = get_u16(entry.payload + 16U);
    reader->audio_sample_rate = get_u32(entry.payload + 24U) >> 16U;
    if (reader->audio_channels == 0U || reader->audio_channels > 8U ||
        reader->audio_sample_rate < 8000U ||
        reader->audio_sample_rate > 192000U ||
        (reader->audio_codec == NGCD_PLAYBACK_AUDIO_PCM_S16LE &&
         get_u16(entry.payload + 18U) != 16U))
        return -1;
    if (reader->audio_codec == NGCD_PLAYBACK_AUDIO_AAC) {
        bool found = false;
        if (entry.payload_size <= 28U ||
            box_find(entry.payload + 28U, entry.payload_size - 28U,
                     "esds", &esds) != 1 || esds.payload_size < 8U)
            return -1;
        for (offset = 4U; offset + 3U <= esds.payload_size; ++offset) {
            size_t length = 0U;
            size_t length_bytes;
            size_t config;
            unsigned int frequency_index;
            unsigned int channels;
            if (esds.payload[offset] != 0x05U)
                continue;
            config = offset + 1U;
            for (length_bytes = 0U; length_bytes < 4U; ++length_bytes) {
                unsigned char value;
                if (config >= esds.payload_size)
                    break;
                value = esds.payload[config++];
                if (length > (SIZE_MAX >> 7U))
                    return -1;
                length = (length << 7U) | (value & 0x7fU);
                if ((value & 0x80U) == 0U)
                    break;
            }
            if (length_bytes == 4U || length < 2U ||
                length > esds.payload_size - config)
                continue;
            reader->audio_object_type = esds.payload[config] >> 3U;
            frequency_index =
                ((unsigned int)(esds.payload[config] & 0x07U) << 1U) |
                (esds.payload[config + 1U] >> 7U);
            channels = (esds.payload[config + 1U] >> 3U) & 0x0fU;
            if (reader->audio_object_type != 2U ||
                frequency_index >= sizeof(sample_rates) /
                                       sizeof(sample_rates[0]) ||
                sample_rates[frequency_index] != reader->audio_sample_rate ||
                channels != reader->audio_channels)
                return -1;
            found = true;
            break;
        }
        if (!found)
            return -1;
    }
    return 0;
}

static int parse_audio_sizes(struct ngcd_mp4_reader *reader,
                             const struct mp4_box *stsz)
{
    uint32_t fixed_size;
    uint32_t count;
    size_t index;
    if (stsz->payload_size < 12U)
        return -1;
    fixed_size = get_u32(stsz->payload + 4U);
    count = get_u32(stsz->payload + 8U);
    if (count == 0U || count > MP4_SAMPLE_LIMIT ||
        (fixed_size == 0U &&
         (size_t)count > (stsz->payload_size - 12U) / 4U))
        return -1;
    reader->audio_samples = calloc((size_t)count,
                                   sizeof(*reader->audio_samples));
    if (reader->audio_samples == NULL)
        return -1;
    reader->audio_sample_count = count;
    for (index = 0U; index < count; ++index) {
        uint32_t size = fixed_size != 0U
                            ? fixed_size
                            : get_u32(stsz->payload + 12U + index * 4U);
        if (size == 0U)
            return -1;
        reader->audio_samples[index].size = size;
        reader->audio_samples[index].key_frame = true;
        if (size > reader->audio_max_sample_size)
            reader->audio_max_sample_size = size;
    }
    return 0;
}

static int parse_audio_timing(struct ngcd_mp4_reader *reader,
                              const struct mp4_box *stts,
                              uint32_t timescale)
{
    uint32_t entry_count;
    size_t entry;
    size_t sample = 0U;
    uint64_t ticks = 0U;
    if (timescale == 0U || stts->payload_size < 8U)
        return -1;
    entry_count = get_u32(stts->payload + 4U);
    if ((size_t)entry_count > (stts->payload_size - 8U) / 8U)
        return -1;
    for (entry = 0U; entry < entry_count; ++entry) {
        uint32_t count = get_u32(stts->payload + 8U + entry * 8U);
        uint32_t delta = get_u32(stts->payload + 12U + entry * 8U);
        uint32_t index;
        if (count == 0U || delta == 0U ||
            count > reader->audio_sample_count - sample)
            return -1;
        for (index = 0U; index < count; ++index) {
            reader->audio_samples[sample++].pts_us =
                ticks > UINT64_MAX / UINT64_C(1000000)
                    ? UINT64_MAX
                    : ticks * UINT64_C(1000000) / timescale;
            if (ticks > UINT64_MAX - delta)
                return -1;
            ticks += delta;
        }
    }
    return sample == reader->audio_sample_count ? 0 : -1;
}

static int parse_audio_offsets(struct ngcd_mp4_reader *reader,
                               const struct mp4_box *chunks,
                               bool chunks_64,
                               const struct mp4_box *stsc)
{
    struct stsc_entry *entries;
    uint32_t chunk_count;
    uint32_t entry_count;
    size_t entry_index = 0U;
    size_t sample_index = 0U;
    uint32_t chunk;
    size_t chunk_width = chunks_64 ? 8U : 4U;
    if (chunks->payload_size < 8U || stsc->payload_size < 8U)
        return -1;
    chunk_count = get_u32(chunks->payload + 4U);
    entry_count = get_u32(stsc->payload + 4U);
    if (chunk_count == 0U || entry_count == 0U ||
        (size_t)chunk_count > (chunks->payload_size - 8U) / chunk_width ||
        (size_t)entry_count > (stsc->payload_size - 8U) / 12U)
        return -1;
    entries = malloc((size_t)entry_count * sizeof(*entries));
    if (entries == NULL)
        return -1;
    for (chunk = 0U; chunk < entry_count; ++chunk) {
        entries[chunk].first_chunk =
            get_u32(stsc->payload + 8U + (size_t)chunk * 12U);
        entries[chunk].samples_per_chunk =
            get_u32(stsc->payload + 12U + (size_t)chunk * 12U);
        entries[chunk].description =
            get_u32(stsc->payload + 16U + (size_t)chunk * 12U);
        if (entries[chunk].first_chunk == 0U ||
            entries[chunk].samples_per_chunk == 0U ||
            entries[chunk].description == 0U ||
            (chunk > 0U && entries[chunk].first_chunk <=
                               entries[chunk - 1U].first_chunk)) {
            free(entries);
            return -1;
        }
    }
    if (entries[0].first_chunk != 1U) {
        free(entries);
        return -1;
    }
    for (chunk = 1U; chunk <= chunk_count; ++chunk) {
        uint64_t offset;
        uint32_t within;
        while (entry_index + 1U < entry_count &&
               entries[entry_index + 1U].first_chunk <= chunk)
            ++entry_index;
        offset = chunks_64
                     ? get_u64(chunks->payload + 8U +
                               (size_t)(chunk - 1U) * 8U)
                     : get_u32(chunks->payload + 8U +
                               (size_t)(chunk - 1U) * 4U);
        for (within = 0U; within < entries[entry_index].samples_per_chunk;
             ++within) {
            uint32_t size;
            if (sample_index >= reader->audio_sample_count) {
                free(entries);
                return -1;
            }
            size = reader->audio_samples[sample_index].size;
            if (offset > reader->file_size || size > reader->file_size - offset) {
                free(entries);
                return -1;
            }
            reader->audio_samples[sample_index++].offset = offset;
            offset += size;
        }
    }
    free(entries);
    return sample_index == reader->audio_sample_count ? 0 : -1;
}

static int parse_audio_track(struct ngcd_mp4_reader *reader,
                             const struct mp4_box *trak)
{
    struct mp4_box mdia, hdlr, mdhd, minf, stbl;
    struct mp4_box stsd, stts, stsc, stsz, chunks;
    uint32_t timescale;
    bool chunks_64 = false;
    int description_result;
    if (box_find(trak->payload, trak->payload_size, "mdia", &mdia) != 1 ||
        box_find(mdia.payload, mdia.payload_size, "hdlr", &hdlr) != 1 ||
        hdlr.payload_size < 12U ||
        memcmp(hdlr.payload + 8U, "soun", 4U) != 0)
        return 1;
    if (box_find(mdia.payload, mdia.payload_size, "mdhd", &mdhd) != 1 ||
        mdhd.payload_size < 20U)
        return -1;
    if (mdhd.payload[0] == 1U) {
        if (mdhd.payload_size < 32U)
            return -1;
        timescale = get_u32(mdhd.payload + 20U);
    } else {
        timescale = get_u32(mdhd.payload + 12U);
    }
    if (box_find(mdia.payload, mdia.payload_size, "minf", &minf) != 1 ||
        box_find(minf.payload, minf.payload_size, "stbl", &stbl) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsd", &stsd) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stts", &stts) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsc", &stsc) != 1 ||
        box_find(stbl.payload, stbl.payload_size, "stsz", &stsz) != 1)
        return -1;
    if (box_find(stbl.payload, stbl.payload_size, "co64", &chunks) == 1)
        chunks_64 = true;
    else if (box_find(stbl.payload, stbl.payload_size, "stco", &chunks) != 1)
        return -1;
    description_result = parse_audio_stsd(reader, &stsd);
    if (description_result != 0)
        return description_result;
    if (parse_audio_sizes(reader, &stsz) != 0 ||
        parse_audio_timing(reader, &stts, timescale) != 0 ||
        parse_audio_offsets(reader, &chunks, chunks_64, &stsc) != 0)
        return -1;
    return 0;
}

static int load_moov(FILE *file, uint64_t file_size,
                     unsigned char **data, size_t *size)
{
    unsigned char header[16];
    uint64_t offset = 0U;
    while (offset <= file_size && file_size - offset >= 8U) {
        uint64_t box_size;
        size_t header_size = 8U;
        if (offset > (uint64_t)LLONG_MAX ||
            fseek(file, (long)offset, SEEK_SET) != 0 ||
            fread(header, 1U, 8U, file) != 8U)
            return -1;
        box_size = get_u32(header);
        if (box_size == 1U) {
            if (file_size - offset < 16U ||
                fread(header + 8U, 1U, 8U, file) != 8U)
                return -1;
            box_size = get_u64(header + 8U);
            header_size = 16U;
        } else if (box_size == 0U) {
            box_size = file_size - offset;
        }
        if (box_size < header_size || box_size > file_size - offset)
            return -1;
        if (memcmp(header + 4U, "moov", 4U) == 0) {
            uint64_t payload = box_size - header_size;
            if (payload == 0U || payload > MP4_MOOV_LIMIT ||
                payload > SIZE_MAX)
                return -1;
            *data = malloc((size_t)payload);
            if (*data == NULL ||
                fread(*data, 1U, (size_t)payload, file) != (size_t)payload) {
                free(*data);
                *data = NULL;
                return -1;
            }
            *size = (size_t)payload;
            return 0;
        }
        offset += box_size;
    }
    return -1;
}

int ngcd_mp4_reader_open(struct ngcd_mp4_reader **output, const char *path)
{
    struct ngcd_mp4_reader *reader;
    unsigned char *moov = NULL;
    size_t moov_size = 0U;
    size_t cursor = 0U;
    struct mp4_box box;
    char type[4];
    int result;
    bool found = false;
    if (output == NULL || path == NULL || path[0] != '/')
        return -1;
    *output = NULL;
    reader = calloc(1U, sizeof(*reader));
    if (reader == NULL)
        return -1;
    reader->file = fopen(path, "rb");
    if (reader->file == NULL || fseek(reader->file, 0L, SEEK_END) != 0) {
        ngcd_mp4_reader_close(reader);
        return -1;
    }
    {
        long length = ftell(reader->file);
        if (length <= 0 || fseek(reader->file, 0L, SEEK_SET) != 0) {
            ngcd_mp4_reader_close(reader);
            return -1;
        }
        reader->file_size = (uint64_t)length;
        reader->create_time = ngcd_file_create_time(path);
    }
    if (
        load_moov(reader->file, reader->file_size, &moov, &moov_size) != 0)
        goto fail;
    while ((result = box_next(moov, moov_size, &cursor, type, &box)) > 0) {
        if (memcmp(type, "trak", 4U) == 0) {
            result = parse_video_track(reader, &box);
            if (result < 0)
                goto fail;
            if (result == 0)
                found = true;
            else if (reader->audio_samples == NULL) {
                result = parse_audio_track(reader, &box);
                if (result < 0)
                    goto fail;
            }
        }
    }
    if (!found || result < 0)
        goto fail;
    free(moov);
    *output = reader;
    return 0;

fail:
    free(moov);
    ngcd_mp4_reader_close(reader);
    return -1;
}

void ngcd_mp4_reader_close(struct ngcd_mp4_reader *reader)
{
    if (reader == NULL)
        return;
    if (reader->file != NULL)
        (void)fclose(reader->file);
    free(reader->samples);
    free(reader->audio_samples);
    free(reader->decoder_config);
    free(reader);
}

enum ngcd_playback_codec ngcd_mp4_reader_codec(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->codec : 0;
}

unsigned int ngcd_mp4_reader_width(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->width : 0U;
}

unsigned int ngcd_mp4_reader_height(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->height : 0U;
}

uint64_t ngcd_mp4_reader_duration_us(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->duration_us : 0U;
}

uint64_t ngcd_mp4_reader_file_size(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->file_size : 0U;
}

uint64_t ngcd_mp4_reader_create_time(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->create_time : 0U;
}

size_t ngcd_mp4_reader_sample_count(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->sample_count : 0U;
}

size_t ngcd_mp4_reader_max_sample_size(const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->max_sample_size : 0U;
}

const struct ngcd_playback_sample *ngcd_mp4_reader_sample(
    const struct ngcd_mp4_reader *reader, size_t index)
{
    return reader != NULL && index < reader->sample_count
               ? &reader->samples[index] : NULL;
}

size_t ngcd_mp4_reader_key_frame_at_or_before(
    const struct ngcd_mp4_reader *reader, size_t index)
{
    if (reader == NULL || reader->sample_count == 0U)
        return 0U;
    if (index >= reader->sample_count)
        index = reader->sample_count - 1U;
    while (index > 0U && !reader->samples[index].key_frame)
        --index;
    return reader->samples[index].key_frame
               ? index : ngcd_mp4_reader_first_key_frame(reader);
}

size_t ngcd_mp4_reader_first_key_frame(
    const struct ngcd_mp4_reader *reader)
{
    size_t index;
    if (reader == NULL)
        return 0U;
    for (index = 0U; index < reader->sample_count; ++index)
        if (reader->samples[index].key_frame)
            return index;
    return 0U;
}

size_t ngcd_mp4_reader_decoder_config(
    const struct ngcd_mp4_reader *reader, const unsigned char **data)
{
    if (data != NULL)
        *data = reader != NULL ? reader->decoder_config : NULL;
    return reader != NULL ? reader->decoder_config_size : 0U;
}

int ngcd_mp4_reader_read_sample(const struct ngcd_mp4_reader *reader,
                                size_t index, unsigned char *destination,
                                size_t capacity, size_t *written)
{
    const struct ngcd_playback_sample *sample;
    unsigned char *source;
    size_t input = 0U;
    size_t output = 0U;
    if (reader == NULL || destination == NULL || written == NULL ||
        (sample = ngcd_mp4_reader_sample(reader, index)) == NULL)
        return -1;
    source = malloc(sample->size);
    if (source == NULL || sample->offset > (uint64_t)LONG_MAX ||
        fseek(reader->file, (long)sample->offset, SEEK_SET) != 0 ||
        fread(source, 1U, sample->size, reader->file) != sample->size) {
        free(source);
        return -1;
    }
    while (input < sample->size) {
        uint32_t nal_size = 0U;
        unsigned int byte;
        if (reader->nal_length_size == 0U ||
            reader->nal_length_size > sample->size - input) {
            free(source);
            return -1;
        }
        for (byte = 0U; byte < reader->nal_length_size; ++byte)
            nal_size = (nal_size << 8U) | source[input + byte];
        input += reader->nal_length_size;
        if (nal_size == 0U || nal_size > sample->size - input ||
            output > capacity || capacity - output < 4U ||
            nal_size > capacity - output - 4U) {
            free(source);
            return -1;
        }
        destination[output++] = 0U;
        destination[output++] = 0U;
        destination[output++] = 0U;
        destination[output++] = 1U;
        memcpy(destination + output, source + input, nal_size);
        output += nal_size;
        input += nal_size;
    }
    free(source);
    *written = output;
    return 0;
}

enum ngcd_playback_audio_codec ngcd_mp4_reader_audio_codec(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_codec : NGCD_PLAYBACK_AUDIO_NONE;
}

unsigned int ngcd_mp4_reader_audio_channels(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_channels : 0U;
}

unsigned int ngcd_mp4_reader_audio_sample_rate(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_sample_rate : 0U;
}

unsigned int ngcd_mp4_reader_audio_object_type(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_object_type : 0U;
}

size_t ngcd_mp4_reader_audio_sample_count(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_sample_count : 0U;
}

size_t ngcd_mp4_reader_audio_max_sample_size(
    const struct ngcd_mp4_reader *reader)
{
    return reader != NULL ? reader->audio_max_sample_size : 0U;
}

const struct ngcd_playback_sample *ngcd_mp4_reader_audio_sample(
    const struct ngcd_mp4_reader *reader, size_t index)
{
    return reader != NULL && index < reader->audio_sample_count
               ? &reader->audio_samples[index] : NULL;
}

int ngcd_mp4_reader_read_audio_sample(const struct ngcd_mp4_reader *reader,
                                      size_t index,
                                      unsigned char *destination,
                                      size_t capacity, size_t *written)
{
    const struct ngcd_playback_sample *sample;
    if (reader == NULL || destination == NULL || written == NULL ||
        (sample = ngcd_mp4_reader_audio_sample(reader, index)) == NULL ||
        sample->size > capacity || sample->offset > (uint64_t)LONG_MAX ||
        fseek(reader->file, (long)sample->offset, SEEK_SET) != 0 ||
        fread(destination, 1U, sample->size, reader->file) != sample->size)
        return -1;
    *written = sample->size;
    return 0;
}
