#include "ngcd_mp4.h"
#include "ngcd_aac_decoder.h"
#include "ngcd_playback.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static uint32_t get_u32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static uint64_t get_u64(const unsigned char *bytes)
{
    return ((uint64_t)get_u32(bytes) << 32U) | get_u32(bytes + 4U);
}

static const unsigned char *find_bytes(const unsigned char *bytes,
                                       size_t size, const void *needle,
                                       size_t needle_size)
{
    size_t index;
    for (index = 0; index + needle_size <= size; ++index)
        if (memcmp(bytes + index, needle, needle_size) == 0)
            return bytes + index;
    return NULL;
}

static int mux_fixture(const char *input_path, const char *output_path)
{
    struct ngcd_mp4_writer *writer = NULL;
    unsigned char pcm[4096] = {0};
    unsigned char *bytes;
    FILE *file = fopen(input_path, "rb");
    long length;
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 || fseek(file, 0L, SEEK_SET) != 0)
        return 1;
    bytes = malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1U, (size_t)length, file) !=
                             (size_t)length || fclose(file) != 0) {
        free(bytes);
        return 1;
    }
    if (ngcd_mp4_open(&writer, output_path, 320U, 160U, 30U) != 0 ||
        ngcd_mp4_write_h264(writer, bytes, (size_t)length, 0U) != 0 ||
        ngcd_mp4_write_pcm_s16le(writer, pcm, sizeof(pcm), 0U,
                                 2U, 48000U) != 0 ||
        ngcd_mp4_write_camm_gyro(writer, 0U, 0.1f, 0.2f, 0.3f) != 0 ||
        ngcd_mp4_close(writer) != 0) {
        free(bytes);
        return 1;
    }
    free(bytes);
    return 0;
}

static int inspect_fixture(const char *path)
{
    struct ngcd_mp4_reader *reader = NULL;
    unsigned char *bytes = NULL;
    size_t capacity;
    size_t count;
    size_t index;
    if (ngcd_mp4_reader_open(&reader, path) != 0)
        return 1;
    capacity = ngcd_mp4_reader_max_sample_size(reader) * 4U;
    bytes = malloc(capacity);
    if (bytes == NULL) {
        ngcd_mp4_reader_close(reader);
        return 1;
    }
    count = ngcd_mp4_reader_sample_count(reader);
    if (count > 16U)
        count = 16U;
    for (index = 0U; index < count; ++index) {
        const struct ngcd_playback_sample *sample =
            ngcd_mp4_reader_sample(reader, index);
        uint64_t hash = UINT64_C(1469598103934665603);
        size_t written = 0U;
        size_t byte;
        if (sample == NULL ||
            ngcd_mp4_reader_read_sample(reader, index, bytes, capacity,
                                        &written) != 0) {
            free(bytes);
            ngcd_mp4_reader_close(reader);
            return 1;
        }
        for (byte = 0U; byte < written; ++byte) {
            hash ^= bytes[byte];
            hash *= UINT64_C(1099511628211);
        }
        printf("%lu offset=%llu size=%u annexb=%lu pts=%llu key=%d "
               "fnv=%016llx\n",
               (unsigned long)index,
               (unsigned long long)sample->offset, sample->size,
               (unsigned long)written,
               (unsigned long long)sample->pts_us,
               sample->key_frame ? 1 : 0,
               (unsigned long long)hash);
    }
    free(bytes);
    ngcd_mp4_reader_close(reader);
    return 0;
}

int main(int argc, char **argv)
{
    static const char path[] = "/tmp/calf-ngcd-mp4-test.mp4";
    static const char aborted[] = "/tmp/calf-ngcd-mp4-abort.mp4";
    static const char invalid[] = "/tmp/calf-ngcd-mp4-invalid.mp4";
    static const char h265_path[] = "/tmp/calf-ngcd-mp4-h265.mp4";
    static const char leading_p_path[] = "/tmp/calf-ngcd-mp4-leading-p.mp4";
    static const char recovery[] = "/tmp/calf-ngcd-mp4-recovery.mp4";
    static const char recovery_index[] =
        "/tmp/calf-ngcd-mp4-recovery.mp4.idx";
    static const unsigned char first_frame[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xb7,
        0xfe, 0x5c, 0x05, 0x05, 0x05, 0x02,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    static const unsigned char second_frame[] = {
        0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11,
    };
    static const unsigned char configured_inter_frame[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xb7,
        0xfe, 0x5c, 0x05, 0x05, 0x05, 0x02,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
        0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11,
    };
    static const unsigned char camm_header[] = {
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc0, 0x3f,
    };
    static const unsigned char high_profile_extension[] = {
        0xfd, 0xf8, 0xf8, 0x00,
    };
    static const unsigned char pcm[4096] = {
        0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00,
        0x40, 0x00, 0x50, 0x00, 0x60, 0x00, 0x70, 0x00,
    };
    static const unsigned char h265_frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
        0x60, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x99, 0xa0,
        0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0, 0xf1,
        0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x09, 0x40,
    };
    struct ngcd_mp4_writer *writer = NULL;
    unsigned char *bytes;
    const unsigned char *mdat;
    const unsigned char *moov;
    FILE *file;
    long length;
    size_t track_count = 0U;
    size_t index;
    if (argc == 3)
        return mux_fixture(argv[1], argv[2]);
    if (argc == 2)
        return inspect_fixture(argv[1]);
    assert(argc == 1);
    (void)unlink(path);
    (void)unlink(aborted);
    (void)unlink(invalid);
    (void)unlink(h265_path);
    (void)unlink(leading_p_path);
    (void)unlink(recovery);
    (void)unlink(recovery_index);
    assert(ngcd_mp4_open(&writer, path, 3840U, 1920U, 30U) == 0);
    assert(ngcd_mp4_write_h264(writer, first_frame,
                               sizeof(first_frame), 1000000U) == 0);
    {
        uint64_t bytes;
        uint64_t duration;
        assert(ngcd_mp4_current_size(writer, &bytes) == 0);
        assert(bytes > sizeof(first_frame));
        assert(ngcd_mp4_duration(writer, &duration) == 0);
        assert(duration == UINT64_C(1000000) / 30U);
    }
    assert(ngcd_mp4_write_camm_gyro(writer, 999000U,
                                    1.0f, -2.0f, 0.5f) == 0);
    assert(ngcd_mp4_write_camm_gyro(writer, 1000000U,
                                    1.5f, -2.5f, 0.75f) == 0);
    assert(ngcd_mp4_write_camm_gyro(writer, 998000U,
                                    0.0f, 0.0f, 0.0f) == 0);
    assert(ngcd_mp4_write_camm_gyro(writer, 1001000U,
                                    2.0f, -3.0f, 1.0f) == 0);
    assert(ngcd_mp4_write_camm_gyro(writer, 1000500U,
                                    0.0f, 0.0f, 0.0f) != 0);
    assert(ngcd_mp4_write_pcm_s16le(writer, pcm, sizeof(pcm), 1000500U,
                                    2U, 48000U) == 0);
    assert(ngcd_mp4_write_h264(writer, second_frame,
                               sizeof(second_frame), 1033333U) == 0);
    assert(ngcd_mp4_write_h264(writer, second_frame,
                               sizeof(second_frame), 1030000U) != 0);
    assert(ngcd_mp4_close(writer) == 0);

    {
        struct ngcd_mp4_reader *reader = NULL;
        const struct ngcd_playback_sample *sample;
        const unsigned char *configuration;
        unsigned char decoded[128];
        unsigned char encoded_audio[2048];
        short decoded_audio[2048];
        size_t decoded_size = 0U;
        size_t encoded_audio_size = 0U;
        size_t decoded_audio_samples = 0U;
        struct ngcd_aac_decoder *audio_decoder = NULL;
        assert(ngcd_mp4_reader_open(&reader, path) == 0);
        assert(ngcd_mp4_reader_codec(reader) == NGCD_PLAYBACK_H264);
        assert(ngcd_mp4_reader_width(reader) == 3840U);
        assert(ngcd_mp4_reader_height(reader) == 1920U);
        assert(ngcd_mp4_reader_create_time(reader) > 0U);
        assert(ngcd_mp4_reader_sample_count(reader) == 2U);
        assert(ngcd_mp4_reader_duration_us(reader) > 60000U);
        assert(ngcd_mp4_reader_decoder_config(reader, &configuration) > 8U);
        assert(configuration != NULL && configuration[3] == 1U);
        sample = ngcd_mp4_reader_sample(reader, 0U);
        assert(sample != NULL && sample->key_frame && sample->size > 0U);
        assert(ngcd_mp4_reader_read_sample(reader, 0U, decoded,
                                           sizeof(decoded),
                                           &decoded_size) == 0);
        assert(decoded_size > 8U && decoded[0] == 0U && decoded[3] == 1U);
        assert(ngcd_mp4_reader_key_frame_at_or_before(reader, 1U) == 0U);
        assert(ngcd_mp4_reader_audio_codec(reader) ==
               NGCD_PLAYBACK_AUDIO_AAC);
        assert(ngcd_mp4_reader_audio_channels(reader) == 2U);
        assert(ngcd_mp4_reader_audio_sample_rate(reader) == 48000U);
        assert(ngcd_mp4_reader_audio_object_type(reader) == 2U);
        assert(ngcd_mp4_reader_audio_sample_count(reader) == 2U);
        assert(ngcd_mp4_reader_audio_max_sample_size(reader) > 0U);
        sample = ngcd_mp4_reader_audio_sample(reader, 0U);
        assert(sample != NULL && sample->pts_us == 0U &&
               sample->size > 0U);
        sample = ngcd_mp4_reader_audio_sample(reader, 1U);
        assert(sample != NULL && sample->pts_us == 21333U &&
               sample->size > 0U);
        assert(ngcd_mp4_reader_read_audio_sample(
                   reader, 0U, encoded_audio, sizeof(encoded_audio),
                   &encoded_audio_size) == 0);
        assert(ngcd_aac_decoder_open(&audio_decoder, 2U, 48000U, 2U) == 0);
        assert(ngcd_aac_decoder_decode(
                   audio_decoder, encoded_audio, encoded_audio_size,
                   decoded_audio,
                   sizeof(decoded_audio) / sizeof(decoded_audio[0]),
                   &decoded_audio_samples) == 0);
        /* libxaac removes its initial 240-frame synthesis delay. */
        assert(decoded_audio_samples == 1568U);
        assert(ngcd_mp4_reader_read_audio_sample(
                   reader, 1U, encoded_audio, sizeof(encoded_audio),
                   &encoded_audio_size) == 0);
        assert(ngcd_aac_decoder_decode(
                   audio_decoder, encoded_audio, encoded_audio_size,
                   decoded_audio,
                   sizeof(decoded_audio) / sizeof(decoded_audio[0]),
                   &decoded_audio_samples) == 0);
        assert(decoded_audio_samples == 2048U);
        assert(ngcd_aac_decoder_reset(audio_decoder) == 0);
        ngcd_aac_decoder_close(audio_decoder);
        {
            unsigned int stress;
            for (stress = 0U; stress < 32U; ++stress) {
                audio_decoder = NULL;
                decoded_audio_samples = 0U;
                assert(ngcd_aac_decoder_open(
                           &audio_decoder, 2U, 48000U, 2U) == 0);
                assert(ngcd_aac_decoder_decode(
                           audio_decoder, encoded_audio, encoded_audio_size,
                           decoded_audio,
                           sizeof(decoded_audio) / sizeof(decoded_audio[0]),
                           &decoded_audio_samples) == 0);
                assert(decoded_audio_samples == 1568U);
                ngcd_aac_decoder_close(audio_decoder);
            }
        }
        ngcd_mp4_reader_close(reader);
    }

    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 0 && fseek(file, 0L, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    assert(bytes != NULL);
    assert(fread(bytes, 1U, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    assert(get_u32(bytes) == 32U && memcmp(bytes + 4U, "ftyp", 4U) == 0);
    mdat = find_bytes(bytes, (size_t)length, "mdat", 4U);
    moov = find_bytes(bytes, (size_t)length, "moov", 4U);
    assert(mdat != NULL && moov != NULL && mdat < moov);
    assert(get_u32(mdat - 4U) == 1U);
    assert(get_u64(mdat + 4U) == (uint64_t)((moov - 4U) - (mdat - 4U)));
    assert(find_bytes(bytes, (size_t)length, "avcC", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, high_profile_extension,
                      sizeof(high_profile_extension)) != NULL);
    assert(find_bytes(bytes, (size_t)length, "st3d", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "sv3d", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "camm", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "meta", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "mp4a", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "esds", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "soun", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "edts", 4U) == NULL);
    assert(find_bytes(bytes, (size_t)length, camm_header,
                      sizeof(camm_header)) != NULL);
    for (index = 0; index + 4U <= (size_t)length; ++index)
        if (memcmp(bytes + index, "trak", 4U) == 0)
            ++track_count;
    assert(track_count == 3U);
    free(bytes);
    assert(unlink(path) == 0);

    /* Older replacement recordings can contain encoder output queued before
     * the requested IDR.  The reader must keep the file and direct preview,
     * replay, and an early seek to its first decodable sync sample. */
    assert(ngcd_mp4_open(&writer, leading_p_path,
                         3840U, 1920U, 30U) == 0);
    assert(ngcd_mp4_write_h264(writer, configured_inter_frame,
                               sizeof(configured_inter_frame), 0U) == 0);
    assert(ngcd_mp4_write_h264(writer, first_frame,
                               sizeof(first_frame), 33333U) == 0);
    assert(ngcd_mp4_close(writer) == 0);
    {
        struct ngcd_mp4_reader *reader = NULL;
        const struct ngcd_playback_sample *sample;
        assert(ngcd_mp4_reader_open(&reader, leading_p_path) == 0);
        assert(ngcd_mp4_reader_sample_count(reader) == 2U);
        sample = ngcd_mp4_reader_sample(reader, 0U);
        assert(sample != NULL && !sample->key_frame);
        sample = ngcd_mp4_reader_sample(reader, 1U);
        assert(sample != NULL && sample->key_frame);
        assert(ngcd_mp4_reader_first_key_frame(reader) == 1U);
        assert(ngcd_mp4_reader_key_frame_at_or_before(reader, 0U) == 1U);
        ngcd_mp4_reader_close(reader);
    }
    assert(unlink(leading_p_path) == 0);

    assert(ngcd_mp4_open(&writer, aborted, 1920U, 1080U, 30U) == 0);
    ngcd_mp4_abort(writer);
    assert(access(aborted, F_OK) != 0);
    assert(ngcd_mp4_open(&writer, invalid, 1920U, 1080U, 30U) == 0);
    assert(ngcd_mp4_write_h264(writer, second_frame,
                               sizeof(second_frame), 0U) == 0);
    assert(ngcd_mp4_close(writer) != 0);
    assert(access(invalid, F_OK) != 0);
    assert(ngcd_mp4_open_h265(&writer, h265_path,
                              3840U, 1920U, 30U) == 0);
    assert(ngcd_mp4_write_h265(writer, h265_frame,
                               sizeof(h265_frame), 0U) == 0);
    assert(ngcd_mp4_close(writer) == 0);
    {
        struct ngcd_mp4_reader *reader = NULL;
        assert(ngcd_mp4_reader_open(&reader, h265_path) == 0);
        assert(ngcd_mp4_reader_codec(reader) == NGCD_PLAYBACK_H265);
        assert(ngcd_mp4_reader_width(reader) == 3840U);
        assert(ngcd_mp4_reader_height(reader) == 1920U);
        assert(ngcd_mp4_reader_sample_count(reader) == 1U);
        ngcd_mp4_reader_close(reader);
    }
    file = fopen(h265_path, "rb");
    assert(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 0 && fseek(file, 0L, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    assert(bytes != NULL &&
           fread(bytes, 1U, (size_t)length, file) == (size_t)length &&
           fclose(file) == 0);
    assert(find_bytes(bytes, (size_t)length, "hvc1", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "hvcC", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "st3d", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "sv3d", 4U) != NULL);
    assert(find_bytes(bytes, (size_t)length, "moov", 4U) != NULL);
    free(bytes);
    assert(unlink(h265_path) == 0);

    {
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (child == 0) {
            struct ngcd_mp4_writer *crashed = NULL;
            if (ngcd_mp4_open(&crashed, recovery,
                              3840U, 1920U, 30U) != 0 ||
                ngcd_mp4_write_h264(crashed, first_frame,
                                    sizeof(first_frame), 1000000U) != 0 ||
                ngcd_mp4_write_pcm_s16le(crashed, pcm, sizeof(pcm),
                                         1000000U, 2U, 48000U) != 0 ||
                ngcd_mp4_write_h264(crashed, second_frame,
                                    sizeof(second_frame), 1033333U) != 0)
                _exit(1);
            _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(access(recovery, F_OK) == 0);
        assert(access(recovery_index, F_OK) == 0);
        assert(ngcd_mp4_recover(recovery) == 0);
        assert(access(recovery_index, F_OK) != 0);
        file = fopen(recovery, "rb");
        assert(file != NULL && fseek(file, 0L, SEEK_END) == 0);
        length = ftell(file);
        assert(length > 0 && fseek(file, 0L, SEEK_SET) == 0);
        bytes = malloc((size_t)length);
        assert(bytes != NULL &&
               fread(bytes, 1U, (size_t)length, file) == (size_t)length &&
               fclose(file) == 0);
        assert(find_bytes(bytes, (size_t)length, "avcC", 4U) != NULL);
        assert(find_bytes(bytes, (size_t)length, "mp4a", 4U) != NULL);
        assert(find_bytes(bytes, (size_t)length, "moov", 4U) != NULL);
        free(bytes);
        assert(unlink(recovery) == 0);
    }
    puts("ngcd MP4/audio/CAMM tests passed");
    return 0;
}
