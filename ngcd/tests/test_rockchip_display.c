#include "ngcd_rk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_display_context {
    int calls, fail_at;
    int device_balance, layer_bind_balance, layer_balance, channel_balance;
    int control_balance, pixel_balance, sent_frames;
    int histogram_frames, histogram_releases, histogram_enabled;
    int histogram_group_created, histogram_group_started;
    int histogram_channel_started, histogram_bound;
    int histogram_video_source;
    uint32_t control[256];
    uint32_t pixels[800 * 480];
    unsigned char luma[480 * 800 * 3 / 2];
};

static uint32_t get_u32(const void *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)buffer + offset, sizeof(value));
    return value;
}

static void *get_pointer(const void *buffer, size_t offset)
{
    void *value;
    memcpy(&value, (const unsigned char *)buffer + offset, sizeof(value));
    return value;
}

static int step(struct fake_display_context *context)
{
    ++context->calls;
    return context->calls == context->fail_at ? -1 : 0;
}

static int fake_set_pub_attr(void *opaque, int device, const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(device == 3 && attribute != NULL && get_u32(attribute, 8) == 46);
    return step(context);
}

static int fake_enable(void *opaque, int device)
{
    struct fake_display_context *context = opaque;
    assert(device == 3);
    if (step(context) != 0) return -1;
    ++context->device_balance;
    return 0;
}

static int fake_disable(void *opaque, int device)
{
    struct fake_display_context *context = opaque;
    assert(device == 3);
    --context->device_balance;
    return 0;
}

static int fake_bind_layer(void *opaque, int layer, int device, int mode)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && device == 3 && mode == 1);
    if (step(context) != 0) return -1;
    ++context->layer_bind_balance;
    return 0;
}

static int fake_unbind_layer(void *opaque, int layer, int device)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && device == 3);
    --context->layer_bind_balance;
    return 0;
}

static int fake_set_layer_attr(void *opaque, int layer, const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && attribute != NULL);
    assert(get_u32(attribute, 8) == 480 && get_u32(attribute, 12) == 800);
    assert(get_u32(attribute, 24) == 25 &&
           get_u32(attribute, 28) == 0x10012);
    return step(context);
}

static int fake_enable_layer(void *opaque, int layer)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7);
    if (step(context) != 0) return -1;
    ++context->layer_balance;
    return 0;
}

static int fake_disable_layer(void *opaque, int layer)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7);
    --context->layer_balance;
    return 0;
}

static int fake_set_channel_attr(void *opaque, int layer, int channel,
                                 const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && channel == 3 && attribute != NULL);
    assert(get_u32(attribute, 0) == 7 && get_u32(attribute, 12) == 480);
    assert(get_u32(attribute, 16) == 800 && get_u32(attribute, 44) == 1);
    return step(context);
}

static int fake_enable_channel(void *opaque, int layer, int channel)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && channel == 3);
    if (step(context) != 0) return -1;
    ++context->channel_balance;
    return 0;
}

static int fake_disable_channel(void *opaque, int layer, int channel)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && channel == 3);
    --context->channel_balance;
    return 0;
}

static int fake_send_frame(void *opaque, int layer, int channel,
                           const void *frame, int timeout_ms)
{
    struct fake_display_context *context = opaque;
    assert(layer == 7 && channel == 3 && timeout_ms == 0);
    assert(get_pointer(frame, 0) == context->pixels);
    assert(get_u32(frame, 8) == 800 && get_u32(frame, 12) == 480);
    assert(get_u32(frame, 28) == 0x10012);
    ++context->sent_frames;
    return 0;
}

static int fake_set_wbc_source(void *opaque, int wbc, const void *source)
{
    struct fake_display_context *context = opaque;
    assert(wbc == 0 && get_u32(source, 4) == 3);
    if (get_u32(source, 0) == 1) {
        assert(!context->histogram_video_source);
        context->histogram_video_source = 1;
    } else {
        assert(get_u32(source, 0) == 0);
        if (context->histogram_enabled)
            assert(context->histogram_video_source);
        else
            assert(!context->histogram_video_source);
        context->histogram_video_source = 0;
    }
    return 0;
}

static int fake_set_wbc_attr(void *opaque, int wbc, const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(wbc == 0 && get_u32(attribute, 0) == 480 &&
           get_u32(attribute, 4) == 800 && get_u32(attribute, 8) == 0 &&
           get_u32(attribute, 12) == 25 && get_u32(attribute, 20) == 0);
    (void)context;
    return 0;
}

static int fake_enable_wbc(void *opaque, int wbc)
{
    struct fake_display_context *context = opaque;
    assert(wbc == 0 && !context->histogram_video_source &&
           !context->histogram_enabled);
    context->histogram_enabled = 1;
    return 0;
}

static int fake_disable_wbc(void *opaque, int wbc)
{
    struct fake_display_context *context = opaque;
    assert(wbc == 0 && context->histogram_enabled &&
           !context->histogram_video_source);
    context->histogram_enabled = 0;
    return 0;
}

static int fake_vpss_create_group(void *opaque, int group,
                                  const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && get_u32(attribute, 0) == 480 &&
           get_u32(attribute, 4) == 800 && get_u32(attribute, 16) == 25 &&
           get_u32(attribute, 20) == 25 && get_u32(attribute, 24) == 0);
    assert(!context->histogram_group_created);
    context->histogram_group_created = 1;
    return 0;
}

static int fake_vpss_destroy_group(void *opaque, int group)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && context->histogram_group_created &&
           !context->histogram_group_started);
    context->histogram_group_created = 0;
    return 0;
}

static int fake_vpss_set_device(void *opaque, int group, int device)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && device == 1 && context->histogram_group_created);
    return 0;
}

static int fake_vpss_enable_backup(void *opaque, int group)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && context->histogram_group_created);
    return 0;
}

static int fake_vpss_start_group(void *opaque, int group)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && context->histogram_group_created &&
           !context->histogram_group_started);
    context->histogram_group_started = 1;
    return 0;
}

static int fake_vpss_stop_group(void *opaque, int group)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && context->histogram_group_started &&
           !context->histogram_channel_started);
    context->histogram_group_started = 0;
    return 0;
}

static int fake_vpss_set_channel_attr(void *opaque, int group, int channel,
                                      const void *attribute)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && channel == 0 && context->histogram_group_started);
    assert(get_u32(attribute, 0) == 2 && get_u32(attribute, 4) == 480 &&
           get_u32(attribute, 8) == 800 && get_u32(attribute, 24) == 0 &&
           get_u32(attribute, 28) == 25 && get_u32(attribute, 32) == 25 &&
           get_u32(attribute, 44) == 3);
    return 0;
}

static int fake_vpss_enable_channel(void *opaque, int group, int channel)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && channel == 0 && context->histogram_group_started &&
           !context->histogram_channel_started);
    context->histogram_channel_started = 1;
    return 0;
}

static int fake_vpss_disable_channel(void *opaque, int group, int channel)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && channel == 0 && context->histogram_channel_started &&
           !context->histogram_bound);
    context->histogram_channel_started = 0;
    return 0;
}

static int fake_bind(void *opaque, const struct ngcd_rk_channel *source,
                     const struct ngcd_rk_channel *destination)
{
    struct fake_display_context *context = opaque;
    assert(source->module == 16 && source->device == 0 &&
           source->channel == 0 && destination->module == 6 &&
           destination->device == 4 && destination->channel == 0);
    assert(context->histogram_enabled && context->histogram_channel_started &&
           !context->histogram_bound);
    context->histogram_bound = 1;
    return 0;
}

static int fake_unbind(void *opaque, const struct ngcd_rk_channel *source,
                       const struct ngcd_rk_channel *destination)
{
    struct fake_display_context *context = opaque;
    assert(source->module == 16 && source->device == 0 &&
           source->channel == 0 && destination->module == 6 &&
           destination->device == 4 && destination->channel == 0);
    assert(context->histogram_bound);
    context->histogram_bound = 0;
    return 0;
}

static int fake_vpss_get_frame(void *opaque, int group, int channel,
                               void *frame, int timeout_ms)
{
    struct fake_display_context *context = opaque;
    unsigned char *bytes = frame;
    void *pointer = context->luma;
    assert(group == 4 && channel == 0 && context->histogram_bound &&
           frame != NULL && (timeout_ms == 20 || timeout_ms == 40));
    memset(frame, 0, NGCD_RK_VIDEO_FRAME_SIZE);
    memcpy(bytes, &pointer, sizeof(pointer));
    memcpy(bytes + 8, &(uint32_t){480}, sizeof(uint32_t));
    memcpy(bytes + 12, &(uint32_t){800}, sizeof(uint32_t));
    memcpy(bytes + 16, &(uint32_t){480}, sizeof(uint32_t));
    memcpy(bytes + 20, &(uint32_t){800}, sizeof(uint32_t));
    memcpy(bytes + 48, &pointer, sizeof(pointer));
    ++context->histogram_frames;
    return 0;
}

static int fake_vpss_release_frame(void *opaque, int group, int channel,
                                   const void *frame)
{
    struct fake_display_context *context = opaque;
    assert(group == 4 && channel == 0 && frame != NULL);
    ++context->histogram_releases;
    return 0;
}

static int fake_mmz_alloc(void *opaque, void **handle, size_t bytes)
{
    struct fake_display_context *context = opaque;
    assert(handle != NULL && bytes == 1024);
    if (step(context) != 0) return -1;
    *handle = context->control;
    ++context->control_balance;
    return 0;
}

static int fake_mmz_free(void *opaque, void *handle)
{
    struct fake_display_context *context = opaque;
    assert(handle == context->control);
    --context->control_balance;
    return 0;
}

static int fake_handle_to_id(void *opaque, void *handle)
{
    struct fake_display_context *context = opaque;
    if (handle == context->control) return 11;
    if (handle == context->pixels) return 12;
    return -1;
}

static void *fake_handle_to_address(void *opaque, void *handle)
{
    struct fake_display_context *context = opaque;
    if (handle == context->control || handle == context->pixels ||
        handle == context->luma) return handle;
    return NULL;
}

static size_t fake_get_size(void *opaque, void *handle)
{
    struct fake_display_context *context = opaque;
    if (handle == context->pixels) return sizeof(context->pixels);
    if (handle == context->luma) return sizeof(context->luma);
    return 0;
}

static int fake_create_graphics(void *opaque, int width, int height,
                                int format, void **handle)
{
    struct fake_display_context *context = opaque;
    assert(width == 480 && height == 800 && format == 0x10012);
    if (step(context) != 0) return -1;
    *handle = context->pixels;
    ++context->pixel_balance;
    return 0;
}

static int fake_destroy_graphics(void *opaque, void *handle)
{
    struct fake_display_context *context = opaque;
    assert(handle == context->pixels);
    --context->pixel_balance;
    return 0;
}

static const struct ngcd_rk_api API = {
    .bind = fake_bind,
    .unbind = fake_unbind,
    .vpss_create_group = fake_vpss_create_group,
    .vpss_destroy_group = fake_vpss_destroy_group,
    .vpss_set_device = fake_vpss_set_device,
    .vpss_enable_backup = fake_vpss_enable_backup,
    .vpss_start_group = fake_vpss_start_group,
    .vpss_stop_group = fake_vpss_stop_group,
    .vpss_set_channel_attr = fake_vpss_set_channel_attr,
    .vpss_enable_channel = fake_vpss_enable_channel,
    .vpss_disable_channel = fake_vpss_disable_channel,
    .vpss_get_channel_frame = fake_vpss_get_frame,
    .vpss_release_channel_frame = fake_vpss_release_frame,
    .vo_set_pub_attr = fake_set_pub_attr,
    .vo_enable = fake_enable,
    .vo_disable = fake_disable,
    .vo_bind_layer = fake_bind_layer,
    .vo_unbind_layer = fake_unbind_layer,
    .vo_set_layer_attr = fake_set_layer_attr,
    .vo_enable_layer = fake_enable_layer,
    .vo_disable_layer = fake_disable_layer,
    .vo_set_channel_attr = fake_set_channel_attr,
    .vo_enable_channel = fake_enable_channel,
    .vo_disable_channel = fake_disable_channel,
    .vo_send_frame = fake_send_frame,
    .vo_set_wbc_source = fake_set_wbc_source,
    .vo_set_wbc_attr = fake_set_wbc_attr,
    .vo_enable_wbc = fake_enable_wbc,
    .vo_disable_wbc = fake_disable_wbc,
    .mmz_alloc = fake_mmz_alloc,
    .mmz_free = fake_mmz_free,
    .mb_handle_to_id = fake_handle_to_id,
    .mb_handle_to_address = fake_handle_to_address,
    .mb_get_size = fake_get_size,
    .vo_create_graphics_buffer = fake_create_graphics,
    .vo_destroy_graphics_buffer = fake_destroy_graphics,
};

static void assert_clean(const struct fake_display_context *context)
{
    assert(context->device_balance == 0);
    assert(context->layer_bind_balance == 0);
    assert(context->layer_balance == 0);
    assert(context->channel_balance == 0);
    assert(context->control_balance == 0);
    assert(context->pixel_balance == 0);
    assert(context->histogram_enabled == 0);
    assert(context->histogram_group_created == 0);
    assert(context->histogram_group_started == 0);
    assert(context->histogram_channel_started == 0);
    assert(context->histogram_bound == 0);
    assert(context->histogram_video_source == 0);
}

int main(void)
{
    int failure;
    for (failure = 1; failure <= 12; ++failure) {
        struct fake_display_context context;
        struct ngcd_rk_display display;
        int result;
        memset(&context, 0, sizeof(context));
        memset(context.luma, 128, sizeof(context.luma));
        context.fail_at = failure;
        result = ngcd_rk_display_start(&display, &API, &context);
        if (result == 0) {
            assert(ngcd_rk_display_control_id(&display) == 11);
            assert(context.control[2] == 480 && context.control[3] == 800);
            assert(context.control[4] == 12);
            assert(context.control[5] == sizeof(context.pixels));
            assert(ngcd_rk_display_tick(&display) == 0);
            assert(context.sent_frames == 0);
            ++context.control[1];
            assert(ngcd_rk_display_tick(&display) == 0);
            assert(context.sent_frames == 1);
            {
                const unsigned char *bmp = NULL;
                size_t bmp_size = 0U;
                uint32_t bins[NGCD_HISTOGRAM_BINS];
                int index;
                assert(ngcd_rk_display_screenshot_bmp(
                           &display, &bmp, &bmp_size) == 0);
                assert(bmp != NULL && bmp_size == 1152054U);
                assert(bmp[0] == 'B' && bmp[1] == 'M');
                assert(get_u32(bmp, 18U) == 800U);
                assert(get_u32(bmp, 22U) == 480U);
                assert(bmp[54] >= 129U && bmp[54] <= 131U);
                assert(context.histogram_enabled == 1);
                assert(context.histogram_group_created == 1);
                assert(context.histogram_bound == 1);
                assert(context.histogram_frames == 5);
                assert(context.histogram_releases == 5);
                assert(ngcd_rk_display_histogram(&display, bins) == 0);
                for (index = 0; index < (int)NGCD_HISTOGRAM_BINS; ++index)
                    assert((index == 32) == (bins[index] != 0U));
                assert(context.histogram_frames == 6);
                assert(context.histogram_releases == 6);
                assert(ngcd_rk_display_histogram_suspend(&display) == 0);
                assert(context.histogram_enabled == 1);
                assert(context.histogram_group_created == 1);
                assert(context.histogram_group_started == 1);
                assert(context.histogram_channel_started == 1);
                assert(context.histogram_bound == 1);
                assert(context.histogram_video_source == 0);
                assert(ngcd_rk_display_histogram(&display, bins) == 0);
                assert(context.histogram_video_source == 1);
                assert(context.histogram_frames == 7);
                assert(context.histogram_releases == 7);
                {
                    assert(ngcd_rk_display_screenshot_bmp(
                               &display, &bmp, &bmp_size) == 0);
                    assert(bmp != NULL && bmp_size == 1152054U);
                    assert(bmp[0] == 'B' && bmp[1] == 'M');
                    assert(get_u32(bmp, 18U) == 800U);
                    assert(get_u32(bmp, 22U) == 480U);
                    assert(bmp[54] >= 129U && bmp[54] <= 131U);
                    assert(context.histogram_video_source == 1);
                    assert(context.histogram_frames == 12);
                    assert(context.histogram_releases == 12);
                }
                ngcd_rk_display_auxiliary_stop(&display);
                assert(context.histogram_enabled == 0);
                assert(context.histogram_group_created == 0);
                assert(context.histogram_bound == 0);
            }
            ngcd_rk_display_stop(&display);
        }
        assert_clean(&context);
    }
    puts("ngcd Rockchip display tests passed");
    return 0;
}
