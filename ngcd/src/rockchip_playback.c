#include "ngcd_rk.h"
#include "ngcd_aac_decoder.h"
#include "ngcd_playback.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RK_MODULE_VDEC = 5,
    RK_MODULE_VPSS = 6,
    RK_MODULE_VO = 9,
    RK_VDEC_CHANNEL = 0,
    RK_VPSS_FINAL_GROUP = 2,
    RK_VPSS_FINAL_CHANNEL = 1,
    RK_VO_DEVICE_LCD = 3,
    RK_VO_VIDEO_LAYER = 3,
    RK_VO_VIDEO_CHANNEL = 1,
    RK_VIDEO_ID_AVC = 8,
    RK_VIDEO_ID_HEVC = 12,
    RK_VIDEO_ID_JPEG = 15,
    RK_VIDEO_MODE_FRAME = 1,
    RK_VIDEO_DISPLAY_PREVIEW = 0,
    PLAYBACK_SEND_LIMIT = 4,
    PLAYBACK_AUDIO_SEND_LIMIT = 4,
    PLAYBACK_PREROLL_SAMPLES = 4,
    PLAYBACK_AUDIO_CHUNK = 4096,
};

#define VIDEO_BUFFER_LIMIT (128U * 1024U * 1024U)
#define RK_VDEC_BUFFER_FULL UINT32_C(0xa005800f)

struct ngcd_rk_playback {
    const struct ngcd_rk_api *api;
    void *api_context;
    struct ngcd_mp4_reader *reader;
    struct ngcd_rk_audio_output *audio_output;
    void *stream_handle;
    unsigned char *stream_address;
    size_t stream_capacity;
    size_t sample_index;
    size_t next_sample;
    size_t seek_target;
    size_t next_audio_sample;
    unsigned char *audio_buffer;
    size_t audio_buffer_capacity;
    short *audio_pcm;
    size_t audio_pcm_samples;
    struct ngcd_aac_decoder *audio_decoder;
    uint64_t anchor_monotonic_us;
    uint64_t anchor_pts_us;
    uint64_t file_size;
    uint64_t create_time;
    uint64_t duration_us;
    uint64_t next_status_report_us;
    uint32_t last_received_frames;
    uint32_t last_decoded_frames;
    uint32_t last_pending_stream;
    uint32_t last_pending_pictures;
    uint32_t last_decoder_errors;
    uint32_t presented_frames;
    uint32_t output_errors;
    unsigned int width;
    unsigned int height;
    bool picture;
    bool paused;
    bool seeking;
    bool status_reported;
    bool decoder_started;
    bool receiving;
    bool layer_bound;
    bool layer_started;
    bool channel_started;
    bool scaler_group_created[3];
    bool scaler_group_started[3];
    bool scaler_channel_started[3];
    bool decoder_scaler_bound;
    bool scaler_output_bound;
};

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static uint32_t get_u32(const unsigned char *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static void put_u64(unsigned char *buffer, size_t offset, uint64_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void put_pointer(unsigned char *buffer, size_t offset,
                        const void *value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static int playback_api_valid(const struct ngcd_rk_api *api)
{
    return api != NULL && api->bind != NULL && api->unbind != NULL &&
           api->vdec_create_channel != NULL &&
           api->vdec_destroy_channel != NULL &&
           api->vdec_start_receive != NULL &&
           api->vdec_stop_receive != NULL &&
           api->vdec_reset_channel != NULL &&
           api->vdec_send_stream != NULL &&
           api->vdec_set_channel_param != NULL &&
           api->vdec_set_display_mode != NULL &&
           api->vpss_create_group != NULL &&
           api->vpss_destroy_group != NULL &&
           api->vpss_set_device != NULL &&
           api->vpss_enable_backup != NULL &&
           api->vpss_start_group != NULL &&
           api->vpss_stop_group != NULL &&
           api->vpss_set_channel_attr != NULL &&
           api->vpss_enable_channel != NULL &&
           api->vpss_disable_channel != NULL &&
           api->vo_bind_layer != NULL && api->vo_unbind_layer != NULL &&
           api->vo_set_layer_buffer_length != NULL &&
           api->vo_set_layer_attr != NULL &&
           api->vo_set_layer_splice_mode != NULL &&
           api->vo_enable_layer != NULL && api->vo_disable_layer != NULL &&
           api->vo_set_channel_attr != NULL &&
           api->vo_enable_channel != NULL &&
           api->vo_disable_channel != NULL &&
           api->mmz_alloc != NULL &&
           api->mmz_free != NULL && api->mb_create != NULL &&
           api->mb_release != NULL && api->mb_handle_to_address != NULL &&
           api->mb_get_size != NULL;
}

static int prepare_jpeg(struct ngcd_rk_playback *playback, const char *path)
{
    FILE *file;
    uint64_t file_size;
    if (ngcd_jpeg_probe(path, &playback->width, &playback->height,
                        &file_size) != 0) {
        fprintf(stderr, "ngcd: playback JPEG header invalid: %s\n", path);
        return -1;
    }
    if (file_size > SIZE_MAX)
        return -1;
    playback->file_size = file_size;
    playback->create_time = ngcd_file_create_time(path);
    playback->stream_capacity = (size_t)file_size;
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "ngcd: playback JPEG open failed: %s\n", path);
        goto fail;
    }
    playback->picture = true;
    playback->paused = true;
    playback->sample_index = 0U;
    playback->next_sample = 1U;
    if (playback->api->mmz_alloc(playback->api_context,
                                 &playback->stream_handle,
                                 playback->stream_capacity) != 0 ||
        playback->stream_handle == NULL ||
        (playback->stream_address =
             playback->api->mb_handle_to_address(
                 playback->api_context,
                 playback->stream_handle)) == NULL ||
        playback->api->mb_get_size(playback->api_context,
                                   playback->stream_handle) <
            playback->stream_capacity) {
        fprintf(stderr, "ngcd: playback JPEG buffer allocation failed: %s\n",
                path);
        (void)fclose(file);
        return -1;
    }
    if (fread(playback->stream_address, 1U, playback->stream_capacity,
              file) != playback->stream_capacity || fclose(file) != 0) {
        fprintf(stderr, "ngcd: playback JPEG read failed: %s\n", path);
        return -1;
    }
    return 0;

fail:
    if (file != NULL)
        (void)fclose(file);
    return -1;
}

static int prepare_video(struct ngcd_rk_playback *playback, const char *path)
{
    size_t max_sample;
    size_t config_size;
    if (ngcd_mp4_reader_open(&playback->reader, path) != 0)
        return -1;
    playback->width = ngcd_mp4_reader_width(playback->reader);
    playback->height = ngcd_mp4_reader_height(playback->reader);
    playback->file_size = ngcd_mp4_reader_file_size(playback->reader);
    playback->create_time = ngcd_mp4_reader_create_time(playback->reader);
    playback->duration_us = ngcd_mp4_reader_duration_us(playback->reader);
    max_sample = ngcd_mp4_reader_max_sample_size(playback->reader);
    config_size = ngcd_mp4_reader_decoder_config(playback->reader, NULL);
    if (max_sample == 0U || config_size >= VIDEO_BUFFER_LIMIT ||
        max_sample > (VIDEO_BUFFER_LIMIT - config_size) / 4U)
        return -1;
    playback->stream_capacity = max_sample * 4U + config_size;
    playback->stream_address = malloc(playback->stream_capacity);
    if (playback->stream_address == NULL)
        return -1;
    if (ngcd_mp4_reader_audio_channels(playback->reader) == 2U &&
        ngcd_mp4_reader_audio_sample_rate(playback->reader) == 48000U &&
        (ngcd_mp4_reader_audio_codec(playback->reader) ==
             NGCD_PLAYBACK_AUDIO_PCM_S16LE ||
         ngcd_mp4_reader_audio_codec(playback->reader) ==
             NGCD_PLAYBACK_AUDIO_AAC)) {
        playback->audio_buffer_capacity =
            ngcd_mp4_reader_audio_max_sample_size(playback->reader);
        playback->audio_buffer = malloc(playback->audio_buffer_capacity);
        if (playback->audio_buffer == NULL)
            return -1;
        if (ngcd_mp4_reader_audio_codec(playback->reader) ==
            NGCD_PLAYBACK_AUDIO_AAC) {
            playback->audio_pcm_samples = 1024U * 2U;
            playback->audio_pcm = malloc(playback->audio_pcm_samples *
                                         sizeof(*playback->audio_pcm));
            if (playback->audio_pcm == NULL ||
                ngcd_aac_decoder_open(
                    &playback->audio_decoder, 2U, 48000U,
                    ngcd_mp4_reader_audio_object_type(playback->reader)) != 0)
                return -1;
        }
    }
    playback->paused = true;
    return 0;
}

static size_t audio_sample_at_or_after(struct ngcd_rk_playback *playback,
                                       uint64_t pts_us)
{
    size_t index = 0U;
    size_t count = ngcd_mp4_reader_audio_sample_count(playback->reader);
    while (index < count) {
        const struct ngcd_playback_sample *sample =
            ngcd_mp4_reader_audio_sample(playback->reader, index);
        if (sample == NULL || sample->pts_us >= pts_us)
            break;
        ++index;
    }
    return index;
}

static int send_audio_sample(struct ngcd_rk_playback *playback, size_t index)
{
    const struct ngcd_playback_sample *sample =
        ngcd_mp4_reader_audio_sample(playback->reader, index);
    size_t bytes = 0U;
    size_t offset = 0U;
    if (sample == NULL || playback->audio_output == NULL ||
        playback->audio_buffer == NULL ||
        ngcd_mp4_reader_read_audio_sample(
            playback->reader, index, playback->audio_buffer,
            playback->audio_buffer_capacity, &bytes) != 0)
        return -1;
    if (playback->audio_decoder != NULL) {
        size_t decoded_samples = 0U;
        if (ngcd_aac_decoder_decode(
                playback->audio_decoder, playback->audio_buffer, bytes,
                playback->audio_pcm, playback->audio_pcm_samples,
                &decoded_samples) != 0 || decoded_samples == 0U)
            return -1;
        bytes = decoded_samples * sizeof(*playback->audio_pcm);
        if (ngcd_rk_audio_output_send_pcm(
                playback->audio_output,
                (const unsigned char *)playback->audio_pcm, bytes,
                sample->pts_us, 20) != 0)
            return -1;
        return 0;
    }
    while (offset < bytes) {
        size_t chunk = bytes - offset;
        uint64_t frames = offset / 4U;
        uint64_t pts = sample->pts_us +
                       frames * UINT64_C(1000000) / 48000U;
        if (chunk > PLAYBACK_AUDIO_CHUNK)
            chunk = PLAYBACK_AUDIO_CHUNK;
        chunk &= ~(size_t)3U;
        if (chunk == 0U ||
            ngcd_rk_audio_output_send_pcm(
                playback->audio_output, playback->audio_buffer + offset,
                chunk, pts, 20) != 0)
            return -1;
        offset += chunk;
    }
    return 0;
}

static int start_video_output(struct ngcd_rk_playback *playback)
{
    unsigned char layer[NGCD_RK_VO_LAYER_ATTR_SIZE];
    unsigned char channel[NGCD_RK_VO_CHN_ATTR_SIZE];
    const struct ngcd_rk_api *api = playback->api;
    if (api->vo_bind_layer(playback->api_context, RK_VO_VIDEO_LAYER,
                           RK_VO_DEVICE_LCD, 2) != 0)
        return -1;
    playback->layer_bound = true;
    if (api->vo_set_layer_buffer_length(playback->api_context,
                                         RK_VO_VIDEO_LAYER, 4) != 0)
        return -1;
    memset(layer, 0, sizeof(layer));
    put_u32(layer, 8U, 480U);
    put_u32(layer, 12U, 800U);
    put_u32(layer, 16U, 480U);
    put_u32(layer, 20U, 800U);
    put_u32(layer, 24U, 25U);
    put_u32(layer, 28U, UINT32_C(0x10001));
    if (api->vo_set_layer_attr(playback->api_context, RK_VO_VIDEO_LAYER,
                               layer) != 0 ||
        api->vo_set_layer_splice_mode(playback->api_context,
                                      RK_VO_VIDEO_LAYER, 0) != 0 ||
        api->vo_enable_layer(playback->api_context,
                             RK_VO_VIDEO_LAYER) != 0)
        return -1;
    playback->layer_started = true;
    memset(channel, 0, sizeof(channel));
    put_u32(channel, 0U, RK_VO_VIDEO_LAYER);
    put_u32(channel, 4U, 0U);
    put_u32(channel, 8U, 0U);
    put_u32(channel, 12U, 480U);
    put_u32(channel, 16U, 800U);
    put_u32(channel, 44U, 1U);
    if (api->vo_set_channel_attr(playback->api_context, RK_VO_VIDEO_LAYER,
                                 RK_VO_VIDEO_CHANNEL, channel) != 0 ||
        api->vo_enable_channel(playback->api_context, RK_VO_VIDEO_LAYER,
                               RK_VO_VIDEO_CHANNEL) != 0)
        return -1;
    playback->channel_started = true;
    return 0;
}

static int start_decoder(struct ngcd_rk_playback *playback)
{
    unsigned char attribute[NGCD_RK_VDEC_CHN_ATTR_SIZE];
    unsigned char parameter[NGCD_RK_VDEC_CHN_PARAM_SIZE];
    int codec = playback->picture
                    ? RK_VIDEO_ID_JPEG
                    : (ngcd_mp4_reader_codec(playback->reader) ==
                               NGCD_PLAYBACK_H265
                           ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC);
    int result;
    memset(attribute, 0, sizeof(attribute));
    /* This 2023 Rockit ABI predates the public structure layout: live probing
     * confirms enMode at offset 0 and enType at offset 4. */
    put_u32(attribute, 0U, RK_VIDEO_MODE_FRAME);
    put_u32(attribute, 4U, (uint32_t)codec);
    put_u32(attribute, 8U, playback->width);
    put_u32(attribute, 12U, playback->height);
    /* The stock vdec_module writes no duplicate geometry or byte size at
     * offsets 16..31. Its 2023 ABI requests 16 decoded-picture buffers and
     * eight stream buffers. Four picture buffers can decode the whole file
     * while starving the downstream VPSS/VO path after its first frames. */
    put_u32(attribute, 32U, 16U);
    put_u32(attribute, 36U, 8U);
    result = playback->api->vdec_create_channel(
        playback->api_context, RK_VDEC_CHANNEL, attribute);
    if (result != 0) {
        fprintf(stderr, "ngcd: playback VDEC create failed (0x%08x)\n",
                (unsigned int)result);
        return -1;
    }
    playback->decoder_started = true;
    memset(parameter, 0, sizeof(parameter));
    /* Match the vendor player exactly.  The 2023 Rockit parameter structure
     * only needs its codec discriminator here; populating newer-header fields
     * changes decoder buffering semantics on this firmware. */
    put_u32(parameter, 0U, (uint32_t)codec);
    result = playback->api->vdec_set_channel_param(
        playback->api_context, RK_VDEC_CHANNEL, parameter);
    if (result != 0) {
        fprintf(stderr, "ngcd: playback VDEC parameters failed (0x%08x)\n",
                (unsigned int)result);
        return -1;
    }
    result = playback->api->vdec_set_display_mode(
        playback->api_context, RK_VDEC_CHANNEL, RK_VIDEO_DISPLAY_PREVIEW);
    if (result != 0) {
        fprintf(stderr, "ngcd: playback VDEC display mode failed (0x%08x)\n",
                (unsigned int)result);
        return -1;
    }
    result = playback->api->vdec_start_receive(
        playback->api_context, RK_VDEC_CHANNEL);
    if (result != 0) {
        fprintf(stderr, "ngcd: playback VDEC receive start failed (0x%08x)\n",
                (unsigned int)result);
        return -1;
    }
    playback->receiving = true;
    return 0;
}

static int start_scaler_group(struct ngcd_rk_playback *playback, int group_id,
                              int channel_id,
                              unsigned int input_width,
                              unsigned int input_height,
                              unsigned int output_width,
                              unsigned int output_height,
                              unsigned int output_mode,
                              unsigned int frame_rate,
                              bool compressed)
{
    unsigned char group[NGCD_RK_VPSS_GRP_ATTR_SIZE];
    unsigned char channel[NGCD_RK_VPSS_CHN_ATTR_SIZE];
    memset(group, 0, sizeof(group));
    put_u32(group, 0U, input_width);
    put_u32(group, 4U, input_height);
    put_u32(group, 16U, frame_rate);
    put_u32(group, 20U, frame_rate);
    put_u32(group, 24U, compressed ? 1U : 0U);
    if (playback->api->vpss_create_group(playback->api_context, group_id,
                                         group) != 0)
        return -1;
    playback->scaler_group_created[group_id] = true;
    if (playback->api->vpss_set_device(playback->api_context,
                                       group_id, 1) != 0 ||
        playback->api->vpss_enable_backup(playback->api_context,
                                          group_id) != 0 ||
        playback->api->vpss_start_group(playback->api_context,
                                        group_id) != 0)
        return -1;
    playback->scaler_group_started[group_id] = true;
    memset(channel, 0, sizeof(channel));
    put_u32(channel, 0U, output_mode);
    put_u32(channel, 4U, output_width);
    put_u32(channel, 8U, output_height);
    put_u32(channel, 24U, compressed ? 1U : 0U);
    put_u32(channel, 28U, frame_rate);
    put_u32(channel, 32U, frame_rate);
    put_u32(channel, 44U, 3U);
    if (playback->api->vpss_set_channel_attr(playback->api_context,
                                             group_id, channel_id,
                                             channel) != 0 ||
        playback->api->vpss_enable_channel(playback->api_context,
                                           group_id, channel_id) != 0)
        return -1;
    playback->scaler_channel_started[group_id] = true;
    return 0;
}

static int start_scaler(struct ngcd_rk_playback *playback)
{
    return start_scaler_group(playback, RK_VPSS_FINAL_GROUP,
                              RK_VPSS_FINAL_CHANNEL,
                              800U, 480U, 800U, 480U,
                              2U, 25U, true);
}

static int bind_scaler_output(struct ngcd_rk_playback *playback)
{
    struct ngcd_rk_channel scaler = {
        RK_MODULE_VPSS, RK_VPSS_FINAL_GROUP, RK_VPSS_FINAL_CHANNEL,
    };
    struct ngcd_rk_channel video_output = {
        RK_MODULE_VO, RK_VO_VIDEO_LAYER, RK_VO_VIDEO_CHANNEL,
    };
    if (playback->api->bind(playback->api_context, &scaler,
                            &video_output) != 0)
        return -1;
    playback->scaler_output_bound = true;
    return 0;
}

static int bind_decoder_input(struct ngcd_rk_playback *playback)
{
    struct ngcd_rk_channel decoder = {
        RK_MODULE_VDEC, 0, RK_VDEC_CHANNEL,
    };
    struct ngcd_rk_channel scaler = {
        RK_MODULE_VPSS, RK_VPSS_FINAL_GROUP, RK_VPSS_FINAL_CHANNEL,
    };
    if (playback->api->bind(playback->api_context, &decoder,
                            &scaler) != 0)
        return -1;
    playback->decoder_scaler_bound = true;
    return 0;
}

static int send_stream_handle(struct ngcd_rk_playback *playback, void *handle,
                              size_t bytes, uint64_t pts_us,
                              bool end_of_stream)
{
    unsigned char stream[NGCD_RK_VDEC_STREAM_SIZE];
    memset(stream, 0, sizeof(stream));
    put_pointer(stream, 0U, handle);
    put_u32(stream, 8U, (uint32_t)bytes);
    put_u64(stream, 16U, pts_us);
    put_u32(stream, 24U, end_of_stream ? 1U : 0U);
    /* Match the vendor player's 2023 ABI: its Annex-B parser receives clear
     * bEndOfFrame/bBypassMbBlk flags and gets 100 ms to copy the reusable MMZ
     * input block. */
    put_u32(stream, 28U, 0U);
    put_u32(stream, 32U, 0U);
    return playback->api->vdec_send_stream(
        playback->api_context, RK_VDEC_CHANNEL, stream, 100);
}

static int send_stream(struct ngcd_rk_playback *playback, size_t bytes,
                       uint64_t pts_us, bool end_of_stream)
{
    return send_stream_handle(playback, playback->stream_handle, bytes,
                              pts_us, end_of_stream);
}

static int send_video_sample(struct ngcd_rk_playback *playback, size_t index,
                             bool include_configuration)
{
    const struct ngcd_playback_sample *sample =
        ngcd_mp4_reader_sample(playback->reader, index);
    const unsigned char *configuration = NULL;
    size_t configuration_size = include_configuration
        ? ngcd_mp4_reader_decoder_config(playback->reader, &configuration)
        : 0U;
    size_t sample_size = 0U;
    if (sample == NULL || configuration_size > playback->stream_capacity)
        return -1;
    if (configuration_size > 0U)
        memcpy(playback->stream_address, configuration, configuration_size);
    if (ngcd_mp4_reader_read_sample(
            playback->reader, index,
            playback->stream_address + configuration_size,
            playback->stream_capacity - configuration_size,
            &sample_size) != 0 ||
        sample_size > UINT32_MAX - configuration_size)
        return -1;
    {
        void *handle = NULL;
        int result;
        int release_result;
        size_t bytes = configuration_size + sample_size;
        if (playback->api->mb_create(playback->api_context, &handle,
                                     playback->stream_address, bytes) != 0 ||
            handle == NULL)
            return -1;
        result = send_stream_handle(playback, handle, bytes,
                                    sample->pts_us, false);
        release_result = playback->api->mb_release(
            playback->api_context, handle);
        return result != 0 ? result : release_result;
    }
}

static void report_decoder_status(struct ngcd_rk_playback *playback,
                                  uint64_t monotonic_us)
{
    unsigned char status[NGCD_RK_VDEC_STATUS_SIZE];
    uint32_t received;
    uint32_t decoded;
    uint32_t pending_stream;
    uint32_t pending_pictures;
    uint32_t error_total = 0U;
    size_t offset;
    if (playback->api->vdec_query_status == NULL ||
        monotonic_us < playback->next_status_report_us)
        return;
    playback->next_status_report_us = monotonic_us + UINT64_C(1000000);
    memset(status, 0, sizeof(status));
    if (playback->api->vdec_query_status(playback->api_context,
                                         RK_VDEC_CHANNEL, status) != 0)
        return;
    received = get_u32(status, 20U);
    decoded = get_u32(status, 24U);
    pending_stream = get_u32(status, 8U);
    pending_pictures = get_u32(status, 12U);
    for (offset = 28U; offset < 64U; offset += sizeof(uint32_t))
        error_total += get_u32(status, offset);
    if (!playback->status_reported ||
        received != playback->last_received_frames ||
        decoded != playback->last_decoded_frames ||
        pending_stream != playback->last_pending_stream ||
        pending_pictures != playback->last_pending_pictures ||
        error_total != playback->last_decoder_errors) {
        fprintf(stderr,
                "ngcd: playback decoder submitted=%lu received=%u "
                "decoded=%u pending_stream=%u pending_pictures=%u "
                "errors=%u\n",
                (unsigned long)(playback->sample_index + 1U), received,
                decoded, pending_stream, pending_pictures, error_total);
    }
    playback->last_received_frames = received;
    playback->last_decoded_frames = decoded;
    playback->last_pending_stream = pending_stream;
    playback->last_pending_pictures = pending_pictures;
    playback->last_decoder_errors = error_total;
    playback->status_reported = true;
}

int ngcd_rk_playback_open(struct ngcd_rk_playback **output,
                          const struct ngcd_rk_api *api, void *api_context,
                          struct ngcd_rk_audio_output *audio_output,
                          const char *path)
{
    struct ngcd_rk_playback *playback;
    size_t length;
    size_t first_sample = 0U;
    int prepare_result;
    if (output == NULL || !playback_api_valid(api) || api_context == NULL ||
        path == NULL || path[0] != '/')
        return -1;
    *output = NULL;
    length = strlen(path);
    playback = calloc(1U, sizeof(*playback));
    if (playback == NULL)
        return -1;
    playback->api = api;
    playback->api_context = api_context;
    playback->audio_output = audio_output;
    if (length >= 4U && path[length - 4U] == '.' &&
        (path[length - 3U] == 'j' || path[length - 3U] == 'J') &&
        (path[length - 2U] == 'p' || path[length - 2U] == 'P') &&
        (path[length - 1U] == 'g' || path[length - 1U] == 'G'))
        prepare_result = prepare_jpeg(playback, path);
    else if (length >= 4U && path[length - 4U] == '.' &&
             (path[length - 3U] == 'm' || path[length - 3U] == 'M') &&
             (path[length - 2U] == 'p' || path[length - 2U] == 'P') &&
             path[length - 1U] == '4')
        prepare_result = prepare_video(playback, path);
    else
        prepare_result = -1;
    if (prepare_result != 0) {
        fprintf(stderr, "ngcd: playback media preparation failed\n");
        goto fail;
    }
    if (start_scaler(playback) != 0) {
        fprintf(stderr, "ngcd: playback scaler setup failed\n");
        goto fail;
    }
    if (start_video_output(playback) != 0) {
        fprintf(stderr, "ngcd: playback video output setup failed\n");
        goto fail;
    }
    if (bind_scaler_output(playback) != 0) {
        fprintf(stderr, "ngcd: playback scaler output bind failed\n");
        goto fail;
    }
    if (start_decoder(playback) != 0)
        goto fail;
    if (bind_decoder_input(playback) != 0) {
        fprintf(stderr, "ngcd: playback decoder bind failed\n");
        goto fail;
    }
    if (!playback->picture)
        first_sample = ngcd_mp4_reader_first_key_frame(playback->reader);
    if ((playback->picture
             ? send_stream(playback, playback->stream_capacity, 0U, false)
             : send_video_sample(playback, first_sample, true)) != 0) {
        fprintf(stderr, "ngcd: playback first sample submission failed\n");
        goto fail;
    }
    playback->sample_index = first_sample;
    playback->next_sample = first_sample + 1U;
    if (!playback->picture) {
        size_t count = ngcd_mp4_reader_sample_count(playback->reader);
        unsigned int submitted = 1U;
        while (playback->next_sample < count &&
               submitted < PLAYBACK_PREROLL_SAMPLES) {
            if (send_video_sample(playback, playback->next_sample,
                                  false) != 0) {
                fprintf(stderr,
                        "ngcd: playback preview pre-roll failed\n");
                goto fail;
            }
            playback->sample_index = playback->next_sample++;
            ++submitted;
        }
    }
    *output = playback;
    return 0;

fail:
    ngcd_rk_playback_close(playback);
    return -1;
}

void ngcd_rk_playback_close(struct ngcd_rk_playback *playback)
{
    struct ngcd_rk_channel decoder = {
        RK_MODULE_VDEC, 0, RK_VDEC_CHANNEL,
    };
    struct ngcd_rk_channel scaler = {
        RK_MODULE_VPSS, RK_VPSS_FINAL_GROUP, RK_VPSS_FINAL_CHANNEL,
    };
    struct ngcd_rk_channel video_output = {
        RK_MODULE_VO, RK_VO_VIDEO_LAYER, RK_VO_VIDEO_CHANNEL,
    };
    if (playback == NULL)
        return;
    if (playback->scaler_output_bound)
        (void)playback->api->unbind(playback->api_context,
                                    &scaler, &video_output);
    if (playback->channel_started)
        (void)playback->api->vo_disable_channel(
            playback->api_context, RK_VO_VIDEO_LAYER, RK_VO_VIDEO_CHANNEL);
    if (playback->layer_started)
        (void)playback->api->vo_disable_layer(
            playback->api_context, RK_VO_VIDEO_LAYER);
    if (playback->layer_bound)
        (void)playback->api->vo_unbind_layer(
            playback->api_context, RK_VO_VIDEO_LAYER, RK_VO_DEVICE_LCD);
    if (playback->decoder_scaler_bound)
        (void)playback->api->unbind(playback->api_context, &decoder,
                                    &scaler);
    if (playback->scaler_channel_started[RK_VPSS_FINAL_GROUP])
        (void)playback->api->vpss_disable_channel(
            playback->api_context, RK_VPSS_FINAL_GROUP,
            RK_VPSS_FINAL_CHANNEL);
    if (playback->scaler_group_started[RK_VPSS_FINAL_GROUP])
        (void)playback->api->vpss_stop_group(
            playback->api_context, RK_VPSS_FINAL_GROUP);
    if (playback->scaler_group_created[RK_VPSS_FINAL_GROUP])
        (void)playback->api->vpss_destroy_group(
            playback->api_context, RK_VPSS_FINAL_GROUP);
    if (playback->receiving)
        (void)playback->api->vdec_stop_receive(
            playback->api_context, RK_VDEC_CHANNEL);
    if (playback->decoder_started)
        (void)playback->api->vdec_destroy_channel(
            playback->api_context, RK_VDEC_CHANNEL);
    if (playback->stream_handle != NULL)
        (void)playback->api->mmz_free(playback->api_context,
                                      playback->stream_handle);
    else
        free(playback->stream_address);
    free(playback->audio_buffer);
    free(playback->audio_pcm);
    ngcd_aac_decoder_close(playback->audio_decoder);
    ngcd_mp4_reader_close(playback->reader);
    free(playback);
}

int ngcd_rk_playback_tick(struct ngcd_rk_playback *playback,
                          uint64_t monotonic_us)
{
    unsigned int sent = 0U;
    unsigned int audio_sent = 0U;
    size_t count;
    if (playback == NULL)
        return -1;
    if (playback->picture || (playback->paused && !playback->seeking)) {
        report_decoder_status(playback, monotonic_us);
        return 0;
    }
    count = ngcd_mp4_reader_sample_count(playback->reader);
    while (playback->next_sample < count && sent < PLAYBACK_SEND_LIMIT) {
        const struct ngcd_playback_sample *sample =
            ngcd_mp4_reader_sample(playback->reader, playback->next_sample);
        uint64_t due;
        if (sample == NULL || sample->pts_us < playback->anchor_pts_us)
            return -1;
        due = playback->anchor_monotonic_us +
              (sample->pts_us - playback->anchor_pts_us);
        if (!playback->seeking && monotonic_us < due)
            break;
        if (playback->seeking && playback->next_sample > playback->seek_target) {
            playback->seeking = false;
            playback->anchor_monotonic_us = monotonic_us;
            playback->anchor_pts_us =
                ngcd_mp4_reader_sample(playback->reader,
                                       playback->sample_index)->pts_us;
            break;
        }
        {
            int send_result = send_video_sample(
                playback, playback->next_sample, false);
            if (send_result != 0) {
                if ((uint32_t)send_result == RK_VDEC_BUFFER_FULL)
                    break;
                return -1;
            }
        }
        playback->sample_index = playback->next_sample++;
        ++sent;
    }
    if (!playback->seeking && playback->audio_buffer != NULL &&
        playback->audio_output != NULL) {
        size_t audio_count =
            ngcd_mp4_reader_audio_sample_count(playback->reader);
        while (playback->next_audio_sample < audio_count &&
               audio_sent < PLAYBACK_AUDIO_SEND_LIMIT) {
            const struct ngcd_playback_sample *sample =
                ngcd_mp4_reader_audio_sample(
                    playback->reader, playback->next_audio_sample);
            uint64_t due;
            if (sample == NULL)
                return -1;
            if (sample->pts_us < playback->anchor_pts_us) {
                ++playback->next_audio_sample;
                continue;
            }
            due = playback->anchor_monotonic_us +
                  (sample->pts_us - playback->anchor_pts_us);
            if (monotonic_us < due)
                break;
            if (send_audio_sample(playback,
                                  playback->next_audio_sample) != 0) {
                fprintf(stderr,
                        "ngcd: playback audio output failed at sample %lu\n",
                        (unsigned long)playback->next_audio_sample);
                free(playback->audio_buffer);
                playback->audio_buffer = NULL;
                free(playback->audio_pcm);
                playback->audio_pcm = NULL;
                ngcd_aac_decoder_close(playback->audio_decoder);
                playback->audio_decoder = NULL;
                break;
            }
            ++playback->next_audio_sample;
            ++audio_sent;
        }
    }
    if (playback->next_sample >= count) {
        playback->paused = true;
        playback->seeking = false;
    }
    report_decoder_status(playback, monotonic_us);
    return 0;
}

int ngcd_rk_playback_pause(struct ngcd_rk_playback *playback, bool pause,
                           uint64_t monotonic_us)
{
    const struct ngcd_playback_sample *sample;
    if (playback == NULL || playback->picture || playback->reader == NULL)
        return -1;
    if (pause) {
        playback->paused = true;
        return 0;
    }
    if (playback->next_sample >=
        ngcd_mp4_reader_sample_count(playback->reader))
        return ngcd_rk_playback_seek(playback, 0U, monotonic_us) == 0
                   ? ngcd_rk_playback_pause(playback, false, monotonic_us) : -1;
    sample = ngcd_mp4_reader_sample(playback->reader,
                                    playback->sample_index);
    if (sample == NULL)
        return -1;
    playback->anchor_monotonic_us = monotonic_us;
    playback->anchor_pts_us = sample->pts_us;
    playback->next_audio_sample =
        audio_sample_at_or_after(playback, sample->pts_us);
    playback->paused = false;
    return 0;
}

int ngcd_rk_playback_seek(struct ngcd_rk_playback *playback, size_t index,
                          uint64_t monotonic_us)
{
    size_t key_frame;
    if (playback == NULL || playback->picture || playback->reader == NULL ||
        index >= ngcd_mp4_reader_sample_count(playback->reader))
        return -1;
    key_frame = ngcd_mp4_reader_key_frame_at_or_before(playback->reader,
                                                        index);
    if (playback->api->vdec_stop_receive(playback->api_context,
                                         RK_VDEC_CHANNEL) != 0)
        return -1;
    playback->receiving = false;
    if (playback->api->vdec_reset_channel(playback->api_context,
                                          RK_VDEC_CHANNEL) != 0 ||
        playback->api->vdec_start_receive(playback->api_context,
                                          RK_VDEC_CHANNEL) != 0)
        return -1;
    playback->receiving = true;
    if (send_video_sample(playback, key_frame, true) != 0)
        return -1;
    playback->sample_index = key_frame;
    playback->next_sample = key_frame + 1U;
    playback->seek_target = index;
    playback->seeking = key_frame < index;
    playback->paused = true;
    playback->anchor_monotonic_us = monotonic_us;
    playback->anchor_pts_us =
        ngcd_mp4_reader_sample(playback->reader, key_frame)->pts_us;
    playback->next_audio_sample =
        audio_sample_at_or_after(playback, playback->anchor_pts_us);
    if (playback->audio_decoder != NULL &&
        ngcd_aac_decoder_reset(playback->audio_decoder) != 0)
        return -1;
    return 0;
}

bool ngcd_rk_playback_is_picture(const struct ngcd_rk_playback *playback)
{
    return playback != NULL && playback->picture;
}

bool ngcd_rk_playback_is_paused(const struct ngcd_rk_playback *playback)
{
    return playback == NULL || playback->paused;
}

size_t ngcd_rk_playback_sample_index(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->sample_index : 0U;
}

size_t ngcd_rk_playback_sample_count(
    const struct ngcd_rk_playback *playback)
{
    return playback == NULL ? 0U
           : playback->picture ? 1U
           : ngcd_mp4_reader_sample_count(playback->reader);
}

uint32_t ngcd_rk_playback_decoder_received(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->last_received_frames : 0U;
}

uint32_t ngcd_rk_playback_decoder_decoded(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->last_decoded_frames : 0U;
}

uint32_t ngcd_rk_playback_decoder_pending_stream(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->last_pending_stream : 0U;
}

uint32_t ngcd_rk_playback_decoder_pending_pictures(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->last_pending_pictures : 0U;
}

uint32_t ngcd_rk_playback_decoder_errors(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->last_decoder_errors : 0U;
}

uint32_t ngcd_rk_playback_presented_frames(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->presented_frames : 0U;
}

uint32_t ngcd_rk_playback_output_errors(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->output_errors : 0U;
}

uint64_t ngcd_rk_playback_duration_us(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->duration_us : 0U;
}

uint64_t ngcd_rk_playback_file_size(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->file_size : 0U;
}

uint64_t ngcd_rk_playback_create_time(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->create_time : 0U;
}

unsigned int ngcd_rk_playback_width(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->width : 0U;
}

unsigned int ngcd_rk_playback_height(
    const struct ngcd_rk_playback *playback)
{
    return playback != NULL ? playback->height : 0U;
}

const char *ngcd_rk_playback_codec_name(
    const struct ngcd_rk_playback *playback)
{
    if (playback == NULL)
        return "";
    if (playback->picture)
        return "JPEG";
    return ngcd_mp4_reader_codec(playback->reader) == NGCD_PLAYBACK_H265
               ? "H265" : "H264";
}
