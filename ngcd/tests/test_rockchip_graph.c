#include "ngcd_rk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fake_context {
    int calls;
    int fail_at;
    int expected_bitrate;
    int expected_codec;
    int expected_rc_mode;
    int system_balance;
    int sensor_balance;
    int sensor_start_count;
    int sensor_start_order[2];
    enum ngcd_rk_sensor_sync_mode sensor_sync_mode[2];
    int vi_device_balance;
    int vi_channel_balance;
    int vpss_group_balance;
    int vpss_channel_balance;
    int avs_group_balance;
    int avs_channel_balance;
    int venc_channel_balance;
    int venc_receive_balance;
    int audio_device_balance;
    int audio_channel_balance;
    int audio_resample_balance;
    int bind_balance;
    int vo_layer_bind_balance;
    int vo_layer_balance;
    int vo_channel_balance;
    const unsigned char *record_data;
    size_t record_size;
    uint64_t record_pts;
    const unsigned char *audio_data;
    size_t audio_size;
    uint64_t audio_pts;
    unsigned int audio_frames;
    volatile unsigned int raw_capture_mask;
    volatile unsigned int raw_capture_calls;
    unsigned char stack_frames[24][6];
    unsigned int stack_count;
    unsigned int stack_frame_index;
    void *stack_output;
    size_t stack_output_size;
    unsigned char histogram_luma[640U * 528U];
    unsigned int histogram_frames;
    unsigned int histogram_releases;
};

static uint32_t get_u32(const void *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)buffer + offset, sizeof(value));
    return value;
}

static void put_u32(void *buffer, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)buffer + offset, &value, sizeof(value));
}

static void put_pointer(void *buffer, size_t offset, void *value)
{
    memcpy((unsigned char *)buffer + offset, &value, sizeof(value));
}

static int step(struct fake_context *context)
{
    ++context->calls;
    return context->calls == context->fail_at ? -1 : 0;
}

static int fake_system_init(void *opaque)
{
    struct fake_context *context = opaque;
    if (step(context) != 0) return -1;
    ++context->system_balance;
    return 0;
}

static int fake_system_exit(void *opaque)
{
    struct fake_context *context = opaque;
    --context->system_balance;
    return 0;
}

static int fake_bind(void *opaque, const struct ngcd_rk_channel *source,
                     const struct ngcd_rk_channel *destination)
{
    struct fake_context *context = opaque;
    assert(source != NULL && destination != NULL);
    if (destination->module == 4) {
        assert(source->module == 17 && source->device == 0 &&
               source->channel == 0);
        assert(destination->device == 0 && destination->channel == 0);
    }
    if (step(context) != 0) return -1;
    ++context->bind_balance;
    return 0;
}

static int fake_unbind(void *opaque, const struct ngcd_rk_channel *source,
                       const struct ngcd_rk_channel *destination)
{
    struct fake_context *context = opaque;
    assert(source != NULL && destination != NULL);
    --context->bind_balance;
    return 0;
}

static int fake_sensor_start(void *opaque, int sensor, int width, int height,
                             int fps, int crop_width, int crop_height,
                             enum ngcd_rk_sensor_sync_mode sync_mode,
                             void **handle)
{
    struct fake_context *context = opaque;
    assert(sensor >= 0 && sensor < 2);
    assert(width > 0 && height > 0 && fps > 0 && handle != NULL);
    assert(crop_width == 3520 && crop_height == 2880);
    assert(sync_mode >= NGCD_RK_SENSOR_NO_SYNC &&
           sync_mode <= NGCD_RK_SENSOR_INTERNAL_MASTER);
    if (step(context) != 0) return -1;
    if (context->sensor_start_count < 2) {
        context->sensor_start_order[context->sensor_start_count] = sensor;
        context->sensor_sync_mode[context->sensor_start_count] = sync_mode;
    }
    ++context->sensor_start_count;
    *handle = (void *)(uintptr_t)(unsigned int)(sensor + 1);
    ++context->sensor_balance;
    return 0;
}

static void fake_sensor_stop(void *opaque, void *handle)
{
    struct fake_context *context = opaque;
    assert(handle != NULL);
    --context->sensor_balance;
}

static int fake_sensor_synchronize(void *opaque)
{
    struct fake_context *context = opaque;
    return step(context);
}

static int fake_aiq_capture_raw(void *opaque, void *sensor_context,
                                int count, const char *capture_directory,
                                char *output_directory)
{
    struct fake_context *context = opaque;
    uintptr_t sensor = (uintptr_t)sensor_context;
    assert(sensor == 1U || sensor == 2U);
    assert(count == 3);
    assert(strcmp(capture_directory, "/tmp") == 0);
    assert(output_directory != NULL && output_directory[0] == '\0');
    (void)__sync_fetch_and_or(&context->raw_capture_mask,
                              1U << (unsigned int)(sensor - 1U));
    (void)__sync_fetch_and_add(&context->raw_capture_calls, 1U);
    return 0;
}

static int fake_prepare_directory(void *opaque, const char *path)
{
    struct fake_context *context = opaque;
    assert(strcmp(path, "/tmp/vr180_7680x3840/") == 0);
    return step(context);
}

static int fake_wait_output(void *opaque, bool stitched, int device,
                            int channel, int timeout_ms)
{
    struct fake_context *context = opaque;
    if (stitched)
        assert(device == 0 && channel == 0 && timeout_ms == 15000);
    else
        assert(device >= 0 && device < 2 && channel == 0 &&
               timeout_ms == 5000);
    return step(context);
}

static int fake_vi_get_dev_attr(void *opaque, int device, void *attribute)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && attribute != NULL);
    if (step(context) != 0) return -1;
    return (int)0xa0088007U;
}

static int fake_vi_set_dev_attr(void *opaque, int device,
                                const void *attribute)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && attribute != NULL);
    return step(context);
}

static int fake_vi_get_dev_enabled(void *opaque, int device)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2);
    if (step(context) != 0) return -2;
    return -1;
}

static int fake_vi_enable_dev(void *opaque, int device)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2);
    if (step(context) != 0) return -1;
    ++context->vi_device_balance;
    return 0;
}

static int fake_vi_disable_dev(void *opaque, int device)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2);
    --context->vi_device_balance;
    return 0;
}

static int fake_vi_bind_pipe(void *opaque, int device, const void *binding)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && binding != NULL);
    assert(get_u32(binding, 0) == 1);
    assert(get_u32(binding, 4) == (uint32_t)device);
    return step(context);
}

static int fake_vi_set_channel_attr(void *opaque, int device, int channel,
                                    const void *attribute)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && channel == 0 && attribute != NULL);
    assert(get_u32(attribute, 0) == 3520);
    assert(get_u32(attribute, 4) == 2880);
    assert(get_u32(attribute, 20) == 0);
    assert(get_u32(attribute, 32) == 2);
    assert(get_u32(attribute, 48) == 8);
    assert(get_u32(attribute, 60) == 1);
    return step(context);
}

static int fake_vi_enable_channel(void *opaque, int device, int channel)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && channel == 0);
    if (step(context) != 0) return -1;
    ++context->vi_channel_balance;
    return 0;
}

static int fake_vi_disable_channel(void *opaque, int device, int channel)
{
    struct fake_context *context = opaque;
    assert(device >= 0 && device < 2 && channel == 0);
    --context->vi_channel_balance;
    return 0;
}

static int fake_vpss_create_group(void *opaque, int group,
                                  const void *attribute)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2 && attribute != NULL);
    assert(get_u32(attribute, 0) == 3520);
    assert(get_u32(attribute, 4) == 2880);
    assert(get_u32(attribute, 24) == 0);
    if (step(context) != 0) return -1;
    ++context->vpss_group_balance;
    return 0;
}

static int fake_vpss_destroy_group(void *opaque, int group)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2);
    --context->vpss_group_balance;
    return 0;
}

static int fake_vpss_group_int(void *opaque, int group, int value)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2);
    (void)value;
    return step(context);
}

static int fake_vpss_group(void *opaque, int group)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2);
    return step(context);
}

static int fake_vpss_stop_group(void *opaque, int group)
{
    (void)opaque;
    assert(group >= 0 && group < 2);
    return 0;
}

static int fake_vpss_set_channel_attr(void *opaque, int group, int channel,
                                      const void *attribute)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2 && channel >= 0 && channel <= 2 &&
           attribute != NULL);
    assert(get_u32(attribute, 0) == 2);
    assert(get_u32(attribute, 4) ==
           (channel == 0 ? 3520U : channel == 1 ? 400U : 640U));
    assert(get_u32(attribute, 8) ==
           (channel == 0 ? 2880U : channel == 1 ? 400U : 528U));
    assert(get_u32(attribute, 24) ==
           (channel == 1 ? 1U : 0U));
    assert(get_u32(attribute, 28) ==
           (channel == 0 ? 30U : channel == 1 ? 25U : 10U));
    assert(get_u32(attribute, 32) ==
           (channel == 0 ? 30U : channel == 1 ? 25U : 10U));
    assert(get_u32(attribute, 44) == 3);
    return step(context);
}

static int fake_vpss_enable_channel(void *opaque, int group, int channel)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2 && channel >= 0 && channel <= 2);
    if (step(context) != 0) return -1;
    ++context->vpss_channel_balance;
    return 0;
}

static int fake_vpss_disable_channel(void *opaque, int group, int channel)
{
    struct fake_context *context = opaque;
    assert(group >= 0 && group < 2 && channel >= 0 && channel <= 2);
    --context->vpss_channel_balance;
    return 0;
}

static int fake_vpss_get_channel_frame(void *opaque, int group, int channel,
                                       void *frame, int timeout_ms)
{
    struct fake_context *context = opaque;
    unsigned int index;
    assert(group == 0 && channel == 2 && frame != NULL && timeout_ms == 250);
    if (step(context) != 0)
        return -1;
    for (index = 0U; index < sizeof(context->histogram_luma); ++index)
        context->histogram_luma[index] = (unsigned char)(index & 0xffU);
    put_u32(frame, 8U, 640U);
    put_u32(frame, 12U, 528U);
    put_u32(frame, 16U, 640U);
    put_pointer(frame, 48U, context->histogram_luma);
    ++context->histogram_frames;
    return 0;
}

static int fake_vpss_release_channel_frame(void *opaque, int group,
                                           int channel, const void *frame)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 2 && frame != NULL);
    ++context->histogram_releases;
    return 0;
}

static int fake_avs_working_set(void *opaque, uint64_t bytes)
{
    struct fake_context *context = opaque;
    assert(bytes == 0x200018000ULL);
    return step(context);
}

static int fake_avs_create_group(void *opaque, int group,
                                 const void *attribute)
{
    struct fake_context *context = opaque;
    assert(group == 0 && attribute != NULL);
    assert(get_u32(attribute, 4) == 2);
    assert(get_u32(attribute, 220) == 36000);
    assert(get_u32(attribute, 224) == 18000);
    if (step(context) != 0) return -1;
    ++context->avs_group_balance;
    return 0;
}

static int fake_avs_destroy_group(void *opaque, int group)
{
    struct fake_context *context = opaque;
    assert(group == 0);
    --context->avs_group_balance;
    return 0;
}

static int fake_avs_group(void *opaque, int group)
{
    struct fake_context *context = opaque;
    assert(group == 0);
    return step(context);
}

static int fake_avs_stop_group(void *opaque, int group)
{
    (void)opaque;
    assert(group == 0);
    return 0;
}

static int fake_avs_set_channel_attr(void *opaque, int group, int channel,
                                     const void *attribute)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 0 && attribute != NULL);
    assert(get_u32(attribute, 0) == 7680);
    assert(get_u32(attribute, 4) == 3840);
    assert(get_u32(attribute, 8) == 0);
    assert(get_u32(attribute, 16) == 2);
    assert(get_u32(attribute, 28) == 8);
    return step(context);
}

static int fake_avs_enable_channel(void *opaque, int group, int channel)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 0);
    if (step(context) != 0) return -1;
    ++context->avs_channel_balance;
    return 0;
}

static int fake_avs_disable_channel(void *opaque, int group, int channel)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 0);
    --context->avs_channel_balance;
    return 0;
}

static int fake_avs_get_channel_frame(void *opaque, int group, int channel,
                                      void *frame, int timeout_ms)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 0 && frame != NULL);
    assert((uintptr_t)frame % 16U == 0U);
    if (context->stack_count != 0U) {
        unsigned char *source;
        if (timeout_ms == 0)
            return -1;
        assert(timeout_ms == 3000);
        if (context->stack_frame_index >= context->stack_count)
            return -1;
        source = context->stack_frames[context->stack_frame_index++];
        put_pointer(frame, 0U, source);
        put_u32(frame, 8U, 2U);
        put_u32(frame, 12U, 2U);
        put_u32(frame, 16U, 2U);
        put_u32(frame, 20U, 2U);
        put_u32(frame, 28U, 0U);
        put_pointer(frame, 48U, source);
        return 0;
    }
    assert(timeout_ms == 3000);
    return step(context);
}

static int fake_avs_release_channel_frame(void *opaque, int group,
                                          int channel, const void *frame)
{
    struct fake_context *context = opaque;
    assert(group == 0 && channel == 0 && frame != NULL);
    if (context->stack_count != 0U)
        return 0;
    return step(context);
}

static int fake_venc_create_channel(void *opaque, int channel,
                                    const void *attribute)
{
    struct fake_context *context = opaque;
    assert((channel == 0 || channel == 4) && attribute != NULL);
    if (channel == 4) {
        assert(get_u32(attribute, 0) == 15U);
        assert(get_u32(attribute, 12) == 44236800U);
        assert(get_u32(attribute, 24) == 7680U);
        assert(get_u32(attribute, 28) == 3840U);
        assert(get_u32(attribute, 32) == 7680U);
        assert(get_u32(attribute, 36) == 3840U);
        assert(get_u32(attribute, 40) == 3U);
        if (step(context) != 0)
            return -1;
        ++context->venc_channel_balance;
        return 0;
    }
    assert(get_u32(attribute, 0) ==
           (uint32_t)(context->expected_codec != 0
                          ? context->expected_codec : 8));
    assert(get_u32(attribute, 12) == 44236800U);
    assert(get_u32(attribute, 16) ==
           (context->expected_codec == 12 ? 0U : 100U));
    assert(get_u32(attribute, 24) == 7680U);
    assert(get_u32(attribute, 28) == 3840U);
    assert(get_u32(attribute, 40) == 6U);
    assert(get_u32(attribute, 72) ==
           (uint32_t)(context->expected_rc_mode != 0
                          ? context->expected_rc_mode : 1));
    assert(get_u32(attribute, 76) == 30U);
    assert(get_u32(attribute, 80) == 30U);
    assert(get_u32(attribute, 96) ==
           (uint32_t)(context->expected_bitrate != 0
                          ? context->expected_bitrate : 100000));
    if (context->expected_rc_mode == 10) {
        assert(get_u32(attribute, 100) == 60000U);
        assert(get_u32(attribute, 104) == 60000U);
        assert(get_u32(attribute, 108) == 3U);
    } else {
        assert(get_u32(attribute, 100) == 3U);
        assert(get_u32(attribute, 104) == 0U);
        assert(get_u32(attribute, 108) == 0U);
    }
    assert(get_u32(attribute, 112) == 1U);
    if (step(context) != 0)
        return -1;
    ++context->venc_channel_balance;
    return 0;
}

static int fake_venc_destroy_channel(void *opaque, int channel)
{
    struct fake_context *context = opaque;
    assert(channel == 0 || channel == 4);
    --context->venc_channel_balance;
    return 0;
}

static int fake_venc_set_rc_param(void *opaque, int channel,
                                  const void *parameter)
{
    static const uint32_t expected[] = {
        23U, 2U, 48U, 15U, 45U, 20U, 2U, 1U,
    };
    struct fake_context *context = opaque;
    assert((channel == 0 || channel == 4) && parameter != NULL);
    assert(memcmp(parameter, expected, sizeof(expected)) == 0);
    return step(context);
}

static int fake_venc_start_receive(void *opaque, int channel,
                                   const void *parameter)
{
    struct fake_context *context = opaque;
    int32_t count;
    assert((channel == 0 || channel == 4) && parameter != NULL);
    memcpy(&count, parameter, sizeof(count));
    assert(count == (channel == 0 ? -1 : 1));
    if (step(context) != 0)
        return -1;
    if (channel == 0)
        ++context->venc_receive_balance;
    return 0;
}

static int fake_venc_stop_receive(void *opaque, int channel)
{
    struct fake_context *context = opaque;
    assert(channel == 0 || channel == 4);
    if (channel == 0)
        --context->venc_receive_balance;
    return 0;
}

static int fake_venc_get_stream(void *opaque, int channel, void *stream,
                                int timeout_ms)
{
    struct fake_context *context = opaque;
    unsigned char *pack;
    uint32_t count = 1U;
    uint32_t size = 4096U;
    assert((channel == 0 || channel == 4) && stream != NULL);
    assert((uintptr_t)stream % 16U == 0U);
    assert(timeout_ms == (channel == 0 ? 0 : 3000));
    if (step(context) != 0)
        return -1;
    /* Rockit fills both stream-info unions, not only the first 24 bytes. */
    memset((unsigned char *)stream + 24U, 0,
           NGCD_RK_VENC_STREAM_SIZE - 24U);
    memcpy(&pack, stream, sizeof(pack));
    assert(pack != NULL);
    assert((uintptr_t)pack % 16U == 0U);
    if (channel == 4) {
        void *handle = context;
        size = 4U;
        memcpy(pack, &handle, sizeof(handle));
    } else if (context->record_data != NULL) {
        const void *handle = context->record_data;
        size = (uint32_t)context->record_size;
        assert(context->record_size > 0U &&
               context->record_size <= UINT32_MAX);
        memcpy(pack, &handle, sizeof(handle));
        memcpy(pack + 16U, &context->record_pts,
               sizeof(context->record_pts));
    }
    memcpy((unsigned char *)stream + 16U, &count, sizeof(count));
    memcpy(pack + 8U, &size, sizeof(size));
    return 0;
}

static int fake_venc_release_stream(void *opaque, int channel,
                                    const void *stream)
{
    struct fake_context *context = opaque;
    assert((channel == 0 || channel == 4) && stream != NULL);
    return step(context);
}

static int fake_venc_request_idr(void *opaque, int channel, bool instant)
{
    struct fake_context *context = opaque;
    assert(channel == 0 && instant);
    return step(context);
}

static int fake_venc_get_jpeg_param(void *opaque, int channel,
                                    void *parameter)
{
    struct fake_context *context = opaque;
    assert(channel == 4 && parameter != NULL);
    if (step(context) != 0)
        return -1;
    memset(parameter, 0, NGCD_RK_VENC_JPEG_PARAM_SIZE);
    ((unsigned char *)parameter)[8] = 0x5aU;
    return 0;
}

static int fake_venc_set_jpeg_param(void *opaque, int channel,
                                    const void *parameter)
{
    struct fake_context *context = opaque;
    const unsigned char *bytes = parameter;
    assert(channel == 4 && parameter != NULL);
    assert(get_u32(parameter, 0) == 95U && bytes[8] == 0x5aU);
    return step(context);
}

static int fake_venc_send_frame(void *opaque, int channel,
                                const void *frame, int timeout_ms)
{
    struct fake_context *context = opaque;
    assert(channel == 4 && frame != NULL && timeout_ms == 3000);
    return step(context);
}

static int fake_ai_set_pub_attr(void *opaque, int device,
                                const void *attribute)
{
    static const uint32_t expected[] = {
        2U, 48000U, 1U, 48000U, 1U, 1U, 4U, 32U, 1024U, 2U,
    };
    struct fake_context *context = opaque;
    assert(device == 0 && attribute != NULL);
    assert(memcmp(attribute, expected, sizeof(expected)) == 0);
    assert(strcmp((const char *)attribute + 40U,
                  "default:CARD=rockchipwm8904") == 0);
    return step(context);
}

static int fake_ai_enable(void *opaque, int device)
{
    struct fake_context *context = opaque;
    assert(device == 0);
    if (step(context) != 0)
        return -1;
    ++context->audio_device_balance;
    return 0;
}

static int fake_ai_disable(void *opaque, int device)
{
    struct fake_context *context = opaque;
    assert(device == 0);
    --context->audio_device_balance;
    return 0;
}

static int fake_ai_enable_channel(void *opaque, int device, int channel)
{
    struct fake_context *context = opaque;
    assert(device == 0 && channel == 0);
    if (step(context) != 0)
        return -1;
    ++context->audio_channel_balance;
    return 0;
}

static int fake_ai_disable_channel(void *opaque, int device, int channel)
{
    struct fake_context *context = opaque;
    assert(device == 0 && channel == 0);
    --context->audio_channel_balance;
    return 0;
}

static int fake_ai_set_channel_param(void *opaque, int device, int channel,
                                     const void *parameter)
{
    struct fake_context *context = opaque;
    const uint32_t *values = parameter;
    assert(device == 0 && channel == 0 && parameter != NULL);
    assert(values[0] == 8U && values[1] == 0U);
    return step(context);
}

static int fake_ai_enable_resample(void *opaque, int device, int channel,
                                   int sample_rate)
{
    struct fake_context *context = opaque;
    assert(device == 0 && channel == 0 && sample_rate == 48000);
    if (step(context) != 0)
        return -1;
    ++context->audio_resample_balance;
    return 0;
}

static int fake_ai_disable_resample(void *opaque, int device, int channel)
{
    struct fake_context *context = opaque;
    assert(device == 0 && channel == 0);
    --context->audio_resample_balance;
    return 0;
}

static int fake_ai_get_frame(void *opaque, int device, int channel,
                             void *frame, void *aec_frame, int timeout_ms)
{
    struct fake_context *context = opaque;
    void *handle;
    uint32_t value;
    assert(device == 0 && channel == 0 && frame != NULL &&
           aec_frame == NULL && timeout_ms == 0);
    if (context->audio_data == NULL || context->audio_frames == 0U)
        return -1;
    --context->audio_frames;
    assert(context->audio_size > 0U && context->audio_size % 4U == 0U &&
           context->audio_size <= UINT32_MAX);
    memset(frame, 0, NGCD_RK_AUDIO_FRAME_SIZE);
    handle = (void *)context->audio_data;
    memcpy(frame, &handle, sizeof(handle));
    value = 1U;
    memcpy((unsigned char *)frame + 8U, &value, sizeof(value));
    memcpy((unsigned char *)frame + 12U, &value, sizeof(value));
    memcpy((unsigned char *)frame + 16U, &context->audio_pts,
           sizeof(context->audio_pts));
    value = (uint32_t)context->audio_size;
    memcpy((unsigned char *)frame + 28U, &value, sizeof(value));
    return 0;
}

static int fake_ai_release_frame(void *opaque, int device, int channel,
                                 const void *frame, const void *aec_frame)
{
    (void)opaque;
    assert(device == 0 && channel == 0 && frame != NULL &&
           aec_frame == NULL);
    return 0;
}

static int fake_mmz_alloc(void *opaque, void **handle, size_t bytes)
{
    struct fake_context *context = opaque;
    assert(handle != NULL && bytes == 6U && context->stack_output == NULL);
    context->stack_output = calloc(1U, bytes);
    if (context->stack_output == NULL)
        return -1;
    context->stack_output_size = bytes;
    *handle = context->stack_output;
    return 0;
}

static int fake_mmz_free(void *opaque, void *handle)
{
    struct fake_context *context = opaque;
    assert(handle != NULL && handle == context->stack_output);
    free(handle);
    context->stack_output = NULL;
    context->stack_output_size = 0U;
    return 0;
}

static void *fake_mb_handle_to_address(void *opaque, void *handle)
{
    static unsigned char jpeg[] = {0xffU, 0xd8U, 0xffU, 0xd9U};
    return opaque == handle ? jpeg : handle;
}

static size_t fake_mb_get_size(void *opaque, void *handle)
{
    struct fake_context *context = opaque;
    unsigned int index;
    if (handle == context->audio_data)
        return context->audio_size;
    if (handle == context->record_data)
        return context->record_size;
    if (handle == context->stack_output)
        return context->stack_output_size;
    for (index = 0U; index < context->stack_count; ++index)
        if (handle == context->stack_frames[index])
            return sizeof(context->stack_frames[index]);
    return handle == opaque ? 4U : 0U;
}

static int file_contains(const char *path, const char needle[4])
{
    unsigned char window[4];
    FILE *file = fopen(path, "rb");
    int byte;
    size_t used = 0U;
    if (file == NULL)
        return 0;
    while ((byte = fgetc(file)) != EOF) {
        if (used < sizeof(window))
            window[used++] = (unsigned char)byte;
        else {
            memmove(window, window + 1U, sizeof(window) - 1U);
            window[sizeof(window) - 1U] = (unsigned char)byte;
        }
        if (used == sizeof(window) && memcmp(window, needle, 4U) == 0) {
            assert(fclose(file) == 0);
            return 1;
        }
    }
    assert(fclose(file) == 0);
    return 0;
}

static int fake_venc_get_h264_vui(void *opaque, int channel, void *vui)
{
    struct fake_context *context = opaque;
    assert(channel == 0 && vui != NULL);
    if (step(context) != 0)
        return -1;
    memset(vui, 0, NGCD_RK_VENC_VUI_SIZE);
    ((unsigned char *)vui)[0] = 0x5aU;
    return 0;
}

static int fake_venc_set_h264_vui(void *opaque, int channel,
                                  const void *vui)
{
    struct fake_context *context = opaque;
    const unsigned char *bytes = vui;
    assert(channel == 0 && vui != NULL);
    assert(bytes[0] == 0x5aU && bytes[22] == 1U);
    return step(context);
}

static int fake_venc_get_h265_vui(void *opaque, int channel, void *vui)
{
    struct fake_context *context = opaque;
    assert(channel == 0 && vui != NULL);
    if (step(context) != 0)
        return -1;
    memset(vui, 0, NGCD_RK_VENC_VUI_SIZE);
    ((unsigned char *)vui)[0] = 0xa5U;
    return 0;
}

static int fake_venc_set_h265_vui(void *opaque, int channel,
                                  const void *vui)
{
    struct fake_context *context = opaque;
    const unsigned char *bytes = vui;
    assert(channel == 0 && vui != NULL);
    assert(bytes[0] == 0xa5U && bytes[26] == 1U);
    return step(context);
}

static int fake_vo_bind_layer(void *opaque, int layer, int device, int mode)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && device == 3 && mode == 2);
    if (step(context) != 0) return -1;
    ++context->vo_layer_bind_balance;
    return 0;
}

static int fake_vo_unbind_layer(void *opaque, int layer, int device)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && device == 3);
    --context->vo_layer_bind_balance;
    return 0;
}

static int fake_vo_layer_int(void *opaque, int layer, int value)
{
    struct fake_context *context = opaque;
    assert(layer == 3);
    (void)value;
    return step(context);
}

static int fake_vo_set_layer_attr(void *opaque, int layer,
                                  const void *attribute)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && attribute != NULL);
    assert(get_u32(attribute, 8) == 480);
    assert(get_u32(attribute, 12) == 800);
    assert(get_u32(attribute, 24) == 25);
    assert(get_u32(attribute, 28) == 0x10001);
    return step(context);
}

static int fake_vo_enable_layer(void *opaque, int layer)
{
    struct fake_context *context = opaque;
    assert(layer == 3);
    if (step(context) != 0) return -1;
    ++context->vo_layer_balance;
    return 0;
}

static int fake_vo_disable_layer(void *opaque, int layer)
{
    struct fake_context *context = opaque;
    assert(layer == 3);
    --context->vo_layer_balance;
    return 0;
}

static int fake_vo_set_channel_attr(void *opaque, int layer, int channel,
                                    const void *attribute)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && channel >= 0 && channel < 2 && attribute != NULL);
    assert(get_u32(attribute, 0) == 3);
    assert(get_u32(attribute, 4) == 20);
    assert(get_u32(attribute, 8) == (uint32_t)(channel * 400));
    assert(get_u32(attribute, 12) == 440);
    assert(get_u32(attribute, 16) == 400);
    assert(get_u32(attribute, 44) == 1);
    return step(context);
}

static int fake_vo_enable_channel(void *opaque, int layer, int channel)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && channel >= 0 && channel < 2);
    if (step(context) != 0) return -1;
    ++context->vo_channel_balance;
    return 0;
}

static int fake_vo_disable_channel(void *opaque, int layer, int channel)
{
    struct fake_context *context = opaque;
    assert(layer == 3 && channel >= 0 && channel < 2);
    --context->vo_channel_balance;
    return 0;
}

static const struct ngcd_rk_api FAKE_API = {
    .system_init = fake_system_init,
    .system_exit = fake_system_exit,
    .bind = fake_bind,
    .unbind = fake_unbind,
    .sensor_start = fake_sensor_start,
    .sensor_stop = fake_sensor_stop,
    .sensor_synchronize = fake_sensor_synchronize,
    .aiq_capture_raw = fake_aiq_capture_raw,
    .prepare_directory = fake_prepare_directory,
    .wait_output = fake_wait_output,
    .vi_get_dev_attr = fake_vi_get_dev_attr,
    .vi_set_dev_attr = fake_vi_set_dev_attr,
    .vi_get_dev_enabled = fake_vi_get_dev_enabled,
    .vi_enable_dev = fake_vi_enable_dev,
    .vi_disable_dev = fake_vi_disable_dev,
    .vi_bind_pipe = fake_vi_bind_pipe,
    .vi_set_channel_attr = fake_vi_set_channel_attr,
    .vi_enable_channel = fake_vi_enable_channel,
    .vi_disable_channel = fake_vi_disable_channel,
    .vpss_create_group = fake_vpss_create_group,
    .vpss_destroy_group = fake_vpss_destroy_group,
    .vpss_set_device = fake_vpss_group_int,
    .vpss_enable_backup = fake_vpss_group,
    .vpss_start_group = fake_vpss_group,
    .vpss_stop_group = fake_vpss_stop_group,
    .vpss_set_channel_attr = fake_vpss_set_channel_attr,
    .vpss_enable_channel = fake_vpss_enable_channel,
    .vpss_disable_channel = fake_vpss_disable_channel,
    .vpss_get_channel_frame = fake_vpss_get_channel_frame,
    .vpss_release_channel_frame = fake_vpss_release_channel_frame,
    .avs_set_working_set = fake_avs_working_set,
    .avs_create_group = fake_avs_create_group,
    .avs_destroy_group = fake_avs_destroy_group,
    .avs_start_group = fake_avs_group,
    .avs_stop_group = fake_avs_stop_group,
    .avs_set_channel_attr = fake_avs_set_channel_attr,
    .avs_enable_channel = fake_avs_enable_channel,
    .avs_disable_channel = fake_avs_disable_channel,
    .avs_get_channel_frame = fake_avs_get_channel_frame,
    .avs_release_channel_frame = fake_avs_release_channel_frame,
    .venc_create_channel = fake_venc_create_channel,
    .venc_destroy_channel = fake_venc_destroy_channel,
    .venc_set_rc_param = fake_venc_set_rc_param,
    .venc_start_receive = fake_venc_start_receive,
    .venc_stop_receive = fake_venc_stop_receive,
    .venc_get_stream = fake_venc_get_stream,
    .venc_release_stream = fake_venc_release_stream,
    .venc_request_idr = fake_venc_request_idr,
    .venc_get_h264_vui = fake_venc_get_h264_vui,
    .venc_set_h264_vui = fake_venc_set_h264_vui,
    .venc_get_h265_vui = fake_venc_get_h265_vui,
    .venc_set_h265_vui = fake_venc_set_h265_vui,
    .venc_get_jpeg_param = fake_venc_get_jpeg_param,
    .venc_set_jpeg_param = fake_venc_set_jpeg_param,
    .venc_send_frame = fake_venc_send_frame,
    .ai_set_pub_attr = fake_ai_set_pub_attr,
    .ai_enable = fake_ai_enable,
    .ai_disable = fake_ai_disable,
    .ai_enable_channel = fake_ai_enable_channel,
    .ai_disable_channel = fake_ai_disable_channel,
    .ai_set_channel_param = fake_ai_set_channel_param,
    .ai_enable_resample = fake_ai_enable_resample,
    .ai_disable_resample = fake_ai_disable_resample,
    .ai_get_frame = fake_ai_get_frame,
    .ai_release_frame = fake_ai_release_frame,
    .mmz_alloc = fake_mmz_alloc,
    .mmz_free = fake_mmz_free,
    .mb_handle_to_address = fake_mb_handle_to_address,
    .mb_get_size = fake_mb_get_size,
    .vo_bind_layer = fake_vo_bind_layer,
    .vo_unbind_layer = fake_vo_unbind_layer,
    .vo_set_layer_buffer_length = fake_vo_layer_int,
    .vo_set_layer_attr = fake_vo_set_layer_attr,
    .vo_set_layer_splice_mode = fake_vo_layer_int,
    .vo_enable_layer = fake_vo_enable_layer,
    .vo_disable_layer = fake_vo_disable_layer,
    .vo_set_channel_attr = fake_vo_set_channel_attr,
    .vo_enable_channel = fake_vo_enable_channel,
    .vo_disable_channel = fake_vo_disable_channel,
};

static struct ngcd_profile profile(void)
{
    struct ngcd_profile value;
    memset(&value, 0, sizeof(value));
    memcpy(value.camera_mode, "SBS_STITCH", sizeof("SBS_STITCH"));
    memcpy(value.stitch_mode, "VR180", sizeof("VR180"));
    value.sensor_count = 1;
    value.sensor[0].width = 4048;
    value.sensor[0].height = 3040;
    value.sensor[0].fps = 30;
    value.capture_count = 1;
    value.capture[0].width = 3520;
    value.capture[0].height = 2880;
    value.capture[0].fps = 30;
    value.stitch.width = 7680;
    value.stitch.height = 3840;
    value.stitch_fov_x = 36000;
    value.stitch_fov_y = 18000;
    value.encoder_count = 1U;
    memcpy(value.encoder[0].codec, "H264", sizeof("H264"));
    memcpy(value.encoder[0].rate_control, "CBR", sizeof("CBR"));
    memcpy(value.encoder[0].profile, "HIGH", sizeof("HIGH"));
    value.encoder[0].width = 7680;
    value.encoder[0].height = 3840;
    value.encoder[0].fps = 30;
    value.encoder[0].bitrate = 100000;
    value.encoder[0].gop = 30;
    return value;
}

static void assert_clean(const struct fake_context *context,
                         const struct ngcd_rk_graph *graph)
{
    assert(context->system_balance == 0);
    assert(context->sensor_balance == 0);
    assert(context->vi_device_balance == 0);
    assert(context->vi_channel_balance == 0);
    assert(context->vpss_group_balance == 0);
    assert(context->vpss_channel_balance == 0);
    assert(context->avs_group_balance == 0);
    assert(context->avs_channel_balance == 0);
    assert(context->venc_channel_balance == 0);
    assert(context->venc_receive_balance == 0);
    assert(context->audio_device_balance == 0);
    assert(context->audio_channel_balance == 0);
    assert(context->audio_resample_balance == 0);
    assert(context->bind_balance == 0);
    assert(context->vo_layer_bind_balance == 0);
    assert(context->vo_layer_balance == 0);
    assert(context->vo_channel_balance == 0);
    assert(!graph->system_started);
    assert(graph->sensor_mask == 0);
    assert(graph->vi_device_mask == 0);
    assert(graph->vpss_group_mask == 0);
}

int main(void)
{
    struct ngcd_profile value = profile();
    int failure;
    assert(NGCD_RK_VENC_STREAM_SIZE == 0x190U);
    {
        static const unsigned int counts[] = {2U, 4U, 8U, 16U, 24U};
        static const unsigned char expected[][3] = {
            {106U, 173U, 83U},
            {144U, 189U, 67U},
            {197U, 201U, 55U},
            {235U, 211U, 45U},
            {235U, 211U, 45U},
        };
        size_t test_index;
        for (test_index = 0U;
             test_index < sizeof(counts) / sizeof(counts[0]);
             ++test_index) {
            struct fake_context context;
            struct ngcd_rk_graph graph;
            unsigned int frame;
            memset(&context, 0, sizeof(context));
            memset(&graph, 0, sizeof(graph));
            context.stack_count = counts[test_index];
            for (frame = 0U; frame < context.stack_count; ++frame) {
                memset(context.stack_frames[frame], 80, 4U);
                context.stack_frames[frame][4] = 160U;
                context.stack_frames[frame][5] = 96U;
            }
            graph.api = &FAKE_API;
            graph.api_context = &context;
            graph.snapshot_width = 2;
            graph.snapshot_height = 2;
            assert(ngcd_rk_graph_stack_snapshot(
                       &graph, (int)context.stack_count) == 0);
            assert(context.stack_output != NULL);
            assert(((unsigned char *)context.stack_output)[0] ==
                   expected[test_index][0]);
            assert(((unsigned char *)context.stack_output)[4] ==
                   expected[test_index][1]);
            assert(((unsigned char *)context.stack_output)[5] ==
                   expected[test_index][2]);
            ngcd_rk_graph_stop(&graph);
            assert(context.stack_output == NULL);
        }
    }
    {
        struct ngcd_rk_venc_chn_attr attribute;
        struct ngcd_rk_venc_rc_param rate_control;
        struct ngcd_encoder_state encoder = value.encoder[0];
        encoder.gop = 45;
        assert(ngcd_rk_encoder_attributes(&encoder, &attribute,
                                          &rate_control) == 0);
        assert(get_u32(attribute.bytes, 72) == 1U);
        assert(get_u32(attribute.bytes, 76) == 45U);
        assert(get_u32(attribute.bytes, 80) == 30U);
        assert(get_u32(attribute.bytes, 84) == 1U);
        assert(get_u32(attribute.bytes, 88) == 30U);
        assert(get_u32(attribute.bytes, 92) == 1U);
        assert(get_u32(attribute.bytes, 96) == 100000U);
        assert(get_u32(attribute.bytes, 100) == 3U);
        assert(get_u32(attribute.bytes, 112) == 1U);
        encoder.gop = value.encoder[0].gop;
        memcpy(encoder.codec, "H265", sizeof("H265"));
        memcpy(encoder.rate_control, "AVBR", sizeof("AVBR"));
        memcpy(encoder.profile, "MAIN", sizeof("MAIN"));
        assert(ngcd_rk_encoder_attributes(&encoder, &attribute,
                                          &rate_control) == 0);
        assert(get_u32(attribute.bytes, 0) == 12U);
        assert(get_u32(attribute.bytes, 16) == 0U);
        assert(get_u32(attribute.bytes, 72) == 10U);
        encoder.color_range = 2;
        assert(ngcd_rk_encoder_attributes(&encoder, &attribute,
                                          &rate_control) != 0);
    }
    for (failure = 1; failure <= 70; ++failure) {
        struct fake_context context;
        struct ngcd_rk_graph graph;
        int result;
        memset(&context, 0, sizeof(context));
        context.fail_at = failure;
        result = ngcd_rk_graph_start(&graph, &FAKE_API, &context, &value);
        if (result == 0) {
            (void)ngcd_rk_graph_tick(&graph);
            (void)ngcd_rk_graph_tick(&graph);
            ngcd_rk_graph_stop(&graph);
        }
        assert_clean(&context, &graph);
    }
    value.preview = true;
    for (failure = 1; failure <= 100; ++failure) {
        struct fake_context context;
        struct ngcd_rk_graph graph;
        struct ngcd_rk_display display;
        int result;
        memset(&context, 0, sizeof(context));
        memset(&display, 0, sizeof(display));
        context.fail_at = failure;
        display.device_started = true;
        result = ngcd_rk_graph_start_in_system(
            &graph, &FAKE_API, &context, &value, &display);
        if (result == 0) {
            (void)ngcd_rk_graph_tick(&graph);
            (void)ngcd_rk_graph_tick(&graph);
            ngcd_rk_graph_stop(&graph);
        }
        assert_clean(&context, &graph);
    }
    {
        struct fake_context context;
        struct ngcd_rk_graph graph;
        struct ngcd_rk_display display;
        uint32_t bins[NGCD_HISTOGRAM_BINS];
        uint64_t total = 0U;
        size_t index;
        memset(&context, 0, sizeof(context));
        memset(&display, 0, sizeof(display));
        display.device_started = true;
        assert(ngcd_rk_graph_start_in_system(
                   &graph, &FAKE_API, &context, &value, &display) == 0);
        assert(ngcd_rk_graph_histogram(&graph, bins) == 0);
        for (index = 0U; index < NGCD_HISTOGRAM_BINS; ++index)
            total += bins[index];
        assert(total > 1000U);
        assert(context.histogram_frames == 1U);
        assert(context.histogram_releases == 1U);
        context.fail_at = context.calls + 1;
        assert(ngcd_rk_graph_histogram(&graph, bins) != 0);
        assert(context.histogram_releases == 1U);
        context.fail_at = 0;
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    {
        struct fake_context context;
        struct ngcd_rk_graph graph;
        struct ngcd_encoder_state encoder = value.encoder[0];
        value.preview = false;
        memset(&context, 0, sizeof(context));
        assert(ngcd_rk_graph_start(&graph, &FAKE_API, &context, &value) == 0);
        assert(context.sensor_start_count == 2);
        assert(context.sensor_start_order[0] == 1);
        assert(context.sensor_sync_mode[0] == NGCD_RK_SENSOR_EXTERNAL_MASTER);
        assert(context.sensor_start_order[1] == 0);
        assert(context.sensor_sync_mode[1] == NGCD_RK_SENSOR_INTERNAL_MASTER);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(!graph.validation_pending && !graph.validation_complete);
        memcpy(encoder.codec, "H265", sizeof("H265"));
        memcpy(encoder.rate_control, "AVBR", sizeof("AVBR"));
        memcpy(encoder.profile, "MAIN", sizeof("MAIN"));
        encoder.bitrate = 60000;
        context.expected_bitrate = encoder.bitrate;
        context.expected_codec = 12;
        context.expected_rc_mode = 10;
        assert(ngcd_rk_graph_validate_encoder(&graph, &encoder) == 0);
        assert(graph.validation_pending && !graph.validation_complete);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(graph.validation_complete && !graph.validation_failed);
        context.expected_bitrate = 0;
        context.expected_codec = 0;
        context.expected_rc_mode = 0;
        assert(ngcd_rk_graph_activate_encoder(
                   &graph, &value.encoder[0]) == 0);
        assert(context.venc_channel_balance == 1);
        assert(context.venc_receive_balance == 1);
        assert(graph.record_bound);
        {
            static const char path[] = "/tmp/calf-ngcd-snapshot-test.jpg";
            unsigned char jpeg[264];
            struct ngcd_rk_exif_metadata metadata = {
                "2026:08:10 12:34:56", 400U, 1U, 125U
            };
            FILE *file;
            (void)remove(path);
            assert(ngcd_rk_graph_snapshot(&graph, path, &metadata) == 0);
            file = fopen(path, "rb");
            assert(file != NULL);
            assert(fread(jpeg, 1U, sizeof(jpeg), file) == sizeof(jpeg));
            assert(fclose(file) == 0);
            assert(jpeg[0] == 0xffU && jpeg[1] == 0xd8U &&
                   jpeg[2] == 0xffU && jpeg[3] == 0xe1U);
            assert(memcmp(jpeg + 6U, "Exif\0\0", 6U) == 0);
            assert(memcmp(jpeg + 115U, "2026:08:10 12:34:56", 19U) == 0);
            assert(get_u32(jpeg, 138U) == 0x0005829aU);
            assert(get_u32(jpeg, 150U) == 0x00038827U);
            assert(jpeg[154] == 1U && jpeg[155] == 0U);
            assert(jpeg[158] == 0x90U && jpeg[159] == 0x01U);
            assert(jpeg[214] == 1U && jpeg[218] == 125U);
            assert(jpeg[262] == 0xffU && jpeg[263] == 0xd9U);
            assert(remove(path) == 0);
        }
        {
            char output_directory[2][128];
            assert(ngcd_rk_graph_capture_raw(
                       &graph, 3, "/tmp", output_directory) == 0);
            assert(context.raw_capture_calls == 2U);
            assert(context.raw_capture_mask == 3U);
            assert(output_directory[0][0] == '\0');
            assert(output_directory[1][0] == '\0');
        }
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    for (failure = 1; failure <= 9; ++failure) {
        static const char path[] = "/tmp/calf-ngcd-snapshot-failure.jpg";
        struct ngcd_rk_exif_metadata metadata = {
            "2026:08:10 12:34:56", 0U, 0U, 0U
        };
        struct fake_context context;
        struct ngcd_rk_graph graph;
        FILE *file;
        memset(&context, 0, sizeof(context));
        (void)remove(path);
        assert(ngcd_rk_graph_start(&graph, &FAKE_API, &context, &value) == 0);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        context.fail_at = context.calls + failure;
        assert(ngcd_rk_graph_snapshot(&graph, path, &metadata) != 0);
        file = fopen(path, "rb");
        assert(file == NULL);
        context.fail_at = 0;
        assert(ngcd_rk_graph_snapshot(&graph, path, &metadata) == 0);
        assert(remove(path) == 0);
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    {
        static const char path[] = "/tmp/calf-ngcd-recording-test.mp4";
        static const unsigned char access_unit[] = {
            0x00, 0x00, 0x00, 0x01,
            0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xb7,
            0xfe, 0x5c, 0x05, 0x05, 0x05, 0x02,
            0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
            0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0,
        };
        static const unsigned char inter_frame[] = {
            0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11,
        };
        static const unsigned char audio[4096] = {0};
        struct fake_context context;
        struct ngcd_rk_graph graph;
        memset(&context, 0, sizeof(context));
        (void)unlink(path);
        assert(ngcd_rk_graph_start(&graph, &FAKE_API, &context, &value) == 0);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(ngcd_rk_graph_record_start(&graph, &value.encoder[0], path) == 0);
        assert(ngcd_rk_graph_record_camm_gyro(
                   &graph, UINT64_C(1234000000), 0.1f, 0.2f, 0.3f) == 0);
        context.record_data = inter_frame;
        context.record_size = sizeof(inter_frame);
        context.record_pts = UINT64_C(1200000);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        {
            uint64_t duration;
            assert(ngcd_rk_graph_record_duration(&graph, &duration) == 0);
            assert(duration == 0U);
        }
        context.record_data = access_unit;
        context.record_size = sizeof(access_unit);
        context.record_pts = UINT64_C(1234567);
        context.audio_data = audio;
        context.audio_size = sizeof(audio);
        context.audio_pts = UINT64_C(1234000);
        context.audio_frames = 1U;
        assert(ngcd_rk_graph_tick(&graph) == 0);
        context.audio_data = NULL;
        assert(ngcd_rk_graph_record_camm_gyro(
                   &graph, UINT64_C(1235000000), 0.1f, 0.2f, 0.3f) == 0);
        {
            uint64_t bytes;
            uint64_t duration;
            assert(ngcd_rk_graph_record_size(&graph, &bytes) == 0);
            assert(bytes > sizeof(access_unit));
            assert(ngcd_rk_graph_record_duration(&graph, &duration) == 0);
            assert(duration == UINT64_C(1000000) /
                                   (uint64_t)value.encoder[0].fps);
        }
        assert(ngcd_rk_graph_record_stop(&graph) == 0);
        assert(context.venc_channel_balance == 1);
        assert(context.venc_receive_balance == 1);
        assert(graph.record_bound);
        context.record_data = NULL;
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(ngcd_rk_graph_validate_encoder(
                   &graph, &value.encoder[0]) == 0);
        assert(context.venc_channel_balance == 0);
        assert(context.venc_receive_balance == 0);
        context.record_data = access_unit;
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(graph.validation_complete && !graph.validation_failed);
        context.record_data = NULL;
        assert(file_contains(path, "avcC"));
        assert(file_contains(path, "camm"));
        assert(file_contains(path, "mp4a"));
        assert(file_contains(path, "moov"));
        assert(unlink(path) == 0);
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    {
        static const char path[] = "/tmp/calf-ngcd-recording-h265.mp4";
        static const unsigned char access_unit[] = {
            0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01,
            0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
            0x60, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x99, 0xa0,
            0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0, 0xf1,
            0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x09, 0x40,
        };
        struct ngcd_profile h265 = value;
        struct fake_context context;
        struct ngcd_rk_graph graph;
        memcpy(h265.encoder[0].codec, "H265", sizeof("H265"));
        memcpy(h265.encoder[0].profile, "MAIN", sizeof("MAIN"));
        memset(&context, 0, sizeof(context));
        context.expected_codec = 12;
        context.expected_rc_mode = 8;
        (void)unlink(path);
        assert(ngcd_rk_graph_start(&graph, &FAKE_API, &context, &h265) == 0);
        context.record_data = access_unit;
        context.record_size = sizeof(access_unit);
        context.record_pts = UINT64_C(2000000);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(ngcd_rk_graph_record_start(
                   &graph, &h265.encoder[0], path) == 0);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        assert(ngcd_rk_graph_record_stop(&graph) == 0);
        assert(file_contains(path, "hvc1"));
        assert(file_contains(path, "hvcC"));
        assert(unlink(path) == 0);
        context.record_data = NULL;
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    for (failure = 1; failure <= 11; ++failure) {
        static const char path[] = "/tmp/calf-ngcd-recording-failure.mp4";
        struct fake_context context;
        struct ngcd_rk_graph graph;
        memset(&context, 0, sizeof(context));
        (void)unlink(path);
        assert(ngcd_rk_graph_start(&graph, &FAKE_API, &context, &value) == 0);
        assert(ngcd_rk_graph_tick(&graph) == 0);
        context.fail_at = context.calls + failure;
        assert(ngcd_rk_graph_record_start(&graph, &value.encoder[0], path) != 0);
        assert(!graph.recording && graph.record_writer == NULL);
        assert(access(path, F_OK) != 0);
        context.fail_at = 0;
        ngcd_rk_graph_stop(&graph);
        assert_clean(&context, &graph);
    }
    puts("ngcd Rockchip graph tests passed");
    return 0;
}
