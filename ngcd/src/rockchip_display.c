#include "ngcd_rk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RK_VO_DEVICE_LCD = 3,
    RK_VO_VIDEO_LAYER = 3,
    RK_VO_HISTOGRAM_WBC = 0,
    RK_VO_WBC_SOURCE_LAYER = 1,
    RK_VO_PIXEL_FORMAT_NV12 = 0,
    RK_MODULE_VPSS = 6,
    RK_MODULE_WBC = 16,
    RK_HISTOGRAM_VPSS_GROUP = 4,
    RK_HISTOGRAM_VPSS_CHANNEL = 0,
    RK_VO_GRAPHICS_LAYER = 7,
    RK_VO_GRAPHICS_CHANNEL = 3,
    RK_VO_BIND_GRAPHICS = 1,
    RK_VO_PIXEL_FORMAT_ARGB8888 = 25,
    RK_VO_FRAME_FORMAT_ARGB8888 = 0x10012,
};

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void put_u16(unsigned char *buffer, size_t offset, uint16_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
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

static void report_display_error(const char *operation, int result)
{
    fprintf(stderr, "ngcd: %s failed (Rockit 0x%08x)\n", operation,
            (unsigned int)result);
}

int ngcd_rk_display_histogram_suspend(struct ngcd_rk_display *display)
{
    uint32_t source[2] = {0U, RK_VO_DEVICE_LCD};
    if (display == NULL || display->api == NULL)
        return -1;
    if (!display->histogram_wbc_started ||
        !display->histogram_video_source)
        return 0;
    if (display->api->vo_set_wbc_source == NULL ||
        display->api->vo_set_wbc_source(display->api_context,
                                        RK_VO_HISTOGRAM_WBC,
                                        source) != 0)
        return -1;
    display->histogram_video_source = false;
    return 0;
}

static void stop_histogram(struct ngcd_rk_display *display)
{
    const struct ngcd_rk_api *api = display->api;
    struct ngcd_rk_channel source = {
        RK_MODULE_WBC, RK_VO_HISTOGRAM_WBC, 0
    };
    struct ngcd_rk_channel destination = {
        RK_MODULE_VPSS, RK_HISTOGRAM_VPSS_GROUP,
        RK_HISTOGRAM_VPSS_CHANNEL
    };
    (void)ngcd_rk_display_histogram_suspend(display);
    if (display->histogram_wbc_vpss_bound && api->unbind != NULL)
        (void)api->unbind(display->api_context, &source, &destination);
    if (display->histogram_vpss_channel_started &&
        api->vpss_disable_channel != NULL)
        (void)api->vpss_disable_channel(display->api_context,
                                        RK_HISTOGRAM_VPSS_GROUP,
                                        RK_HISTOGRAM_VPSS_CHANNEL);
    if (display->histogram_vpss_group_started &&
        api->vpss_stop_group != NULL)
        (void)api->vpss_stop_group(display->api_context,
                                   RK_HISTOGRAM_VPSS_GROUP);
    if (display->histogram_vpss_group_created &&
        api->vpss_destroy_group != NULL)
        (void)api->vpss_destroy_group(display->api_context,
                                      RK_HISTOGRAM_VPSS_GROUP);
    if (display->histogram_wbc_started && api->vo_disable_wbc != NULL)
        (void)api->vo_disable_wbc(display->api_context,
                                  RK_VO_HISTOGRAM_WBC);
    display->histogram_wbc_vpss_bound = false;
    display->histogram_vpss_channel_started = false;
    display->histogram_vpss_group_started = false;
    display->histogram_vpss_group_created = false;
    display->histogram_wbc_started = false;
    display->histogram_video_source = false;
}

void ngcd_rk_display_auxiliary_stop(struct ngcd_rk_display *display)
{
    if (display != NULL && display->api != NULL)
        stop_histogram(display);
}

static int validate_display_api(const struct ngcd_rk_api *api)
{
    return api != NULL && api->vo_set_pub_attr != NULL &&
           api->vo_enable != NULL && api->vo_disable != NULL &&
           api->vo_bind_layer != NULL && api->vo_unbind_layer != NULL &&
           api->vo_set_layer_attr != NULL && api->vo_enable_layer != NULL &&
           api->vo_disable_layer != NULL && api->vo_set_channel_attr != NULL &&
           api->vo_enable_channel != NULL &&
           api->vo_disable_channel != NULL && api->vo_send_frame != NULL &&
           api->mmz_alloc != NULL && api->mmz_free != NULL &&
           api->mb_handle_to_id != NULL &&
           api->mb_handle_to_address != NULL && api->mb_get_size != NULL &&
           api->vo_create_graphics_buffer != NULL &&
           api->vo_destroy_graphics_buffer != NULL;
}

static void make_layer_attribute(unsigned char *attribute, int format,
                                 uint32_t flags)
{
    memset(attribute, 0, NGCD_RK_VO_LAYER_ATTR_SIZE);
    put_u32(attribute, 8, 480);
    put_u32(attribute, 12, 800);
    put_u32(attribute, 16, 480);
    put_u32(attribute, 20, 800);
    put_u32(attribute, 24, (uint32_t)format);
    put_u32(attribute, 28, flags);
}

static void make_channel_attribute(unsigned char *attribute)
{
    memset(attribute, 0, NGCD_RK_VO_CHN_ATTR_SIZE);
    put_u32(attribute, 0, RK_VO_GRAPHICS_LAYER);
    put_u32(attribute, 4, 0);
    put_u32(attribute, 8, 0);
    put_u32(attribute, 12, 480);
    put_u32(attribute, 16, 800);
    put_u32(attribute, 44, 1);
}

void ngcd_rk_display_stop(struct ngcd_rk_display *display)
{
    const struct ngcd_rk_api *api;
    if (display == NULL || display->api == NULL)
        return;
    api = display->api;
    stop_histogram(display);
    if (display->graphics_channel_started)
        (void)api->vo_disable_channel(display->api_context,
                                      RK_VO_GRAPHICS_LAYER,
                                      RK_VO_GRAPHICS_CHANNEL);
    if (display->graphics_layer_started)
        (void)api->vo_disable_layer(display->api_context,
                                    RK_VO_GRAPHICS_LAYER);
    if (display->graphics_layer_bound)
        (void)api->vo_unbind_layer(display->api_context,
                                   RK_VO_GRAPHICS_LAYER,
                                   RK_VO_DEVICE_LCD);
    if (display->pixel_handle != NULL)
        (void)api->vo_destroy_graphics_buffer(display->api_context,
                                              display->pixel_handle);
    if (display->control_handle != NULL)
        (void)api->mmz_free(display->api_context, display->control_handle);
    if (display->device_started)
        (void)api->vo_disable(display->api_context, RK_VO_DEVICE_LCD);
    free(display->screenshot_bmp);
    memset(display, 0, sizeof(*display));
    display->control_id = -1;
}

int ngcd_rk_display_start(struct ngcd_rk_display *display,
                          const struct ngcd_rk_api *api, void *api_context)
{
    unsigned char public_attribute[NGCD_RK_VO_PUB_ATTR_SIZE];
    unsigned char layer_attribute[NGCD_RK_VO_LAYER_ATTR_SIZE];
    unsigned char channel_attribute[NGCD_RK_VO_CHN_ATTR_SIZE];
    int pixel_id;
    int result;
    size_t pixel_size;
    void *pixels;
    if (display == NULL || !validate_display_api(api))
        return -1;
    memset(display, 0, sizeof(*display));
    display->api = api;
    display->api_context = api_context;
    display->control_id = -1;

    memset(public_attribute, 0, sizeof(public_attribute));
    put_u64(public_attribute, 0, UINT64_C(0x20000000000));
    put_u32(public_attribute, 8, 46);
    result = api->vo_set_pub_attr(api_context, RK_VO_DEVICE_LCD,
                                  public_attribute);
    if (result != 0) {
        report_display_error("VO_SetPubAttr", result);
        goto fail;
    }
    result = api->vo_enable(api_context, RK_VO_DEVICE_LCD);
    if (result != 0) {
        report_display_error("VO_Enable", result);
        goto fail;
    }
    display->device_started = true;

    result = api->vo_bind_layer(api_context, RK_VO_GRAPHICS_LAYER,
                                RK_VO_DEVICE_LCD, RK_VO_BIND_GRAPHICS);
    if (result != 0) {
        report_display_error("VO_BindLayer(graphics)", result);
        goto fail;
    }
    display->graphics_layer_bound = true;
    make_layer_attribute(layer_attribute, RK_VO_PIXEL_FORMAT_ARGB8888,
                         RK_VO_FRAME_FORMAT_ARGB8888);
    result = api->vo_set_layer_attr(api_context, RK_VO_GRAPHICS_LAYER,
                                    layer_attribute);
    if (result != 0) {
        report_display_error("VO_SetLayerAttr(graphics)", result);
        goto fail;
    }
    result = api->vo_enable_layer(api_context, RK_VO_GRAPHICS_LAYER);
    if (result != 0) {
        report_display_error("VO_EnableLayer(graphics)", result);
        goto fail;
    }
    display->graphics_layer_started = true;

    make_channel_attribute(channel_attribute);
    result = api->vo_set_channel_attr(api_context, RK_VO_GRAPHICS_LAYER,
                                      RK_VO_GRAPHICS_CHANNEL,
                                      channel_attribute);
    if (result != 0) {
        report_display_error("VO_SetChnAttr(graphics)", result);
        goto fail;
    }
    result = api->vo_enable_channel(api_context, RK_VO_GRAPHICS_LAYER,
                                    RK_VO_GRAPHICS_CHANNEL);
    if (result != 0) {
        report_display_error("VO_EnableChn(graphics)", result);
        goto fail;
    }
    display->graphics_channel_started = true;

    result = api->mmz_alloc(api_context, &display->control_handle, 1024);
    if (result != 0 || display->control_handle == NULL) {
        report_display_error("SYS_MmzAlloc(graphics control)", result);
        goto fail;
    }
    display->control_id =
        api->mb_handle_to_id(api_context, display->control_handle);
    display->control = api->mb_handle_to_address(api_context,
                                                 display->control_handle);
    if (display->control_id < 0 || display->control == NULL) {
        fprintf(stderr, "ngcd: graphics control buffer mapping failed\n");
        goto fail;
    }

    result = api->vo_create_graphics_buffer(api_context, 480, 800,
                                            RK_VO_FRAME_FORMAT_ARGB8888,
                                            &display->pixel_handle);
    /* Rockit returns the allocated byte size on success, not zero. */
    if (result < 0 || display->pixel_handle == NULL) {
        report_display_error("VO_CreateGraphicsFrameBuffer", result);
        goto fail;
    }
    pixel_id = api->mb_handle_to_id(api_context, display->pixel_handle);
    pixel_size = api->mb_get_size(api_context, display->pixel_handle);
    pixels = api->mb_handle_to_address(api_context, display->pixel_handle);
    if (pixel_id < 0 || pixel_size < 800U * 480U * 4U || pixels == NULL ||
        pixel_size > UINT32_MAX) {
        fprintf(stderr,
                "ngcd: graphics pixel buffer mapping failed "
                "(id=%d size=%lu address=%p)\n",
                pixel_id, (unsigned long)pixel_size, pixels);
        goto fail;
    }
    memset(pixels, 0, pixel_size);
    display->control[0] = 0;
    display->control[1] = 0;
    display->control[2] = 480;
    display->control[3] = 800;
    display->control[4] = (uint32_t)pixel_id;
    display->control[5] = (uint32_t)pixel_size;
    display->last_generation = 0;
    return 0;

fail:
    ngcd_rk_display_stop(display);
    return -1;
}

int ngcd_rk_display_tick(struct ngcd_rk_display *display)
{
    unsigned char frame[NGCD_RK_VIDEO_FRAME_SIZE];
    uint32_t generation;
    if (display == NULL || display->api == NULL || display->control == NULL ||
        display->pixel_handle == NULL)
        return -1;
    generation = display->control[1];
    if (generation == display->last_generation)
        return 0;
    memset(frame, 0, sizeof(frame));
    put_pointer(frame, 0, display->pixel_handle);
    put_u32(frame, 8, 800);
    put_u32(frame, 12, 480);
    put_u32(frame, 16, 800);
    put_u32(frame, 20, 480);
    put_u32(frame, 28, RK_VO_FRAME_FORMAT_ARGB8888);
    if (display->api->vo_send_frame(display->api_context,
                                    RK_VO_GRAPHICS_LAYER,
                                    RK_VO_GRAPHICS_CHANNEL, frame, 0) != 0)
        return -1;
    display->last_generation = generation;
    return 0;
}

int ngcd_rk_display_control_id(const struct ngcd_rk_display *display)
{
    return display != NULL ? display->control_id : -1;
}

static uint32_t get_u32(const unsigned char *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static void *get_pointer(const unsigned char *buffer, size_t offset)
{
    void *value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static int start_histogram(struct ngcd_rk_display *display)
{
    uint32_t source[2];
    unsigned char wbc_attribute[24];
    unsigned char group_attribute[NGCD_RK_VPSS_GRP_ATTR_SIZE];
    unsigned char channel_attribute[NGCD_RK_VPSS_CHN_ATTR_SIZE];
    struct ngcd_rk_channel bind_source = {
        RK_MODULE_WBC, RK_VO_HISTOGRAM_WBC, 0
    };
    struct ngcd_rk_channel bind_destination = {
        RK_MODULE_VPSS, RK_HISTOGRAM_VPSS_GROUP,
        RK_HISTOGRAM_VPSS_CHANNEL
    };
    const struct ngcd_rk_api *api = display->api;
    if (display->histogram_wbc_vpss_bound) {
        if (display->histogram_video_source)
            return 0;
        source[0] = RK_VO_WBC_SOURCE_LAYER;
        source[1] = RK_VO_VIDEO_LAYER;
        if (api->vo_set_wbc_source == NULL ||
            api->vo_set_wbc_source(display->api_context,
                                   RK_VO_HISTOGRAM_WBC, source) != 0)
            return -1;
        display->histogram_video_source = true;
        return 0;
    }
    if (api->vo_set_wbc_source == NULL || api->vo_set_wbc_attr == NULL ||
        api->vo_enable_wbc == NULL || api->vo_disable_wbc == NULL ||
        api->vpss_create_group == NULL || api->vpss_destroy_group == NULL ||
        api->vpss_set_device == NULL || api->vpss_enable_backup == NULL ||
        api->vpss_start_group == NULL || api->vpss_stop_group == NULL ||
        api->vpss_set_channel_attr == NULL ||
        api->vpss_enable_channel == NULL ||
        api->vpss_disable_channel == NULL ||
        api->vpss_get_channel_frame == NULL ||
        api->vpss_release_channel_frame == NULL || api->bind == NULL ||
        api->unbind == NULL)
        return -1;
    /* Match stock init_wbc(): WBC must be enabled on LCD device 3 before
     * changing the live source to video layer 3.  Rockit accepts subsequent
     * source changes across graph transitions only in this configuration. */
    source[0] = 0U;
    source[1] = RK_VO_DEVICE_LCD;
    memset(wbc_attribute, 0, sizeof(wbc_attribute));
    put_u32(wbc_attribute, 0U, 480U);
    put_u32(wbc_attribute, 4U, 800U);
    put_u32(wbc_attribute, 8U, RK_VO_PIXEL_FORMAT_NV12);
    put_u32(wbc_attribute, 12U, 25U);
    if (api->vo_set_wbc_source(display->api_context,
                               RK_VO_HISTOGRAM_WBC, source) != 0 ||
        api->vo_set_wbc_attr(display->api_context,
                             RK_VO_HISTOGRAM_WBC, wbc_attribute) != 0 ||
        api->vo_enable_wbc(display->api_context,
                           RK_VO_HISTOGRAM_WBC) != 0)
        return -1;
    display->histogram_wbc_started = true;
    display->histogram_video_source = false;

    memset(group_attribute, 0, sizeof(group_attribute));
    put_u32(group_attribute, 0U, 480U);
    put_u32(group_attribute, 4U, 800U);
    put_u32(group_attribute, 16U, 25U);
    put_u32(group_attribute, 20U, 25U);
    if (api->vpss_create_group(display->api_context,
                               RK_HISTOGRAM_VPSS_GROUP,
                               group_attribute) != 0)
        goto fail;
    display->histogram_vpss_group_created = true;
    if (api->vpss_set_device(display->api_context,
                             RK_HISTOGRAM_VPSS_GROUP, 1) != 0 ||
        api->vpss_enable_backup(display->api_context,
                                RK_HISTOGRAM_VPSS_GROUP) != 0 ||
        api->vpss_start_group(display->api_context,
                              RK_HISTOGRAM_VPSS_GROUP) != 0)
        goto fail;
    display->histogram_vpss_group_started = true;

    memset(channel_attribute, 0, sizeof(channel_attribute));
    put_u32(channel_attribute, 0U, 2U); /* pass-through */
    put_u32(channel_attribute, 4U, 480U);
    put_u32(channel_attribute, 8U, 800U);
    put_u32(channel_attribute, 28U, 25U);
    put_u32(channel_attribute, 32U, 25U);
    put_u32(channel_attribute, 44U, 3U);
    if (api->vpss_set_channel_attr(display->api_context,
                                   RK_HISTOGRAM_VPSS_GROUP,
                                   RK_HISTOGRAM_VPSS_CHANNEL,
                                   channel_attribute) != 0 ||
        api->vpss_enable_channel(display->api_context,
                                 RK_HISTOGRAM_VPSS_GROUP,
                                 RK_HISTOGRAM_VPSS_CHANNEL) != 0)
        goto fail;
    display->histogram_vpss_channel_started = true;
    if (api->bind(display->api_context, &bind_source,
                  &bind_destination) != 0)
        goto fail;
    display->histogram_wbc_vpss_bound = true;
    source[0] = RK_VO_WBC_SOURCE_LAYER;
    source[1] = RK_VO_VIDEO_LAYER;
    if (api->vo_set_wbc_source(display->api_context,
                               RK_VO_HISTOGRAM_WBC, source) != 0)
        goto fail;
    display->histogram_video_source = true;
    return 0;

fail:
    stop_histogram(display);
    return -1;
}

int ngcd_rk_display_histogram(
    struct ngcd_rk_display *display,
    uint32_t bins[NGCD_HISTOGRAM_BINS])
{
    unsigned char frame[NGCD_RK_VIDEO_FRAME_SIZE];
    const struct ngcd_rk_api *api;
    unsigned char *luma;
    void *handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t minimum_dimension;
    size_t required;
    size_t block_size;
    int center_x;
    int center_y;
    int radius;
    int step;
    int x;
    int y;
    int attempt;
    size_t index;
    int result;
    if (display == NULL || bins == NULL || display->api == NULL ||
        !display->device_started)
        return -1;
    api = display->api;
    if (api->mb_handle_to_address == NULL || api->mb_get_size == NULL ||
        start_histogram(display) != 0)
        return -1;
    result = -1;
    for (attempt = 0; attempt < 3 && result != 0; ++attempt) {
        memset(frame, 0, sizeof(frame));
        result = api->vpss_get_channel_frame(display->api_context,
                                             RK_HISTOGRAM_VPSS_GROUP,
                                             RK_HISTOGRAM_VPSS_CHANNEL,
                                             frame, 20);
    }
    if (result != 0)
        return -1;
    width = get_u32(frame, 8U);
    height = get_u32(frame, 12U);
    stride = get_u32(frame, 16U);
    handle = get_pointer(frame, 0U);
    luma = get_pointer(frame, 48U);
    if (luma == NULL && handle != NULL)
        luma = api->mb_handle_to_address(display->api_context, handle);
    block_size = handle != NULL
                     ? api->mb_get_size(display->api_context, handle) : 0U;
    if (width < 64U || width > 8192U || height < 64U || height > 8192U ||
        stride < width || stride > 8192U ||
        height > SIZE_MAX / stride) {
        result = -1;
        goto release;
    }
    required = (size_t)stride * height;
    if (luma == NULL || (block_size != 0U && block_size < required)) {
        result = -1;
        goto release;
    }
    for (index = 0U; index < NGCD_HISTOGRAM_BINS; ++index)
        bins[index] = 0U;
    center_x = (int)width / 2;
    center_y = (int)height / 2;
    if (width > height) {
        center_x = (int)width / 4;
        minimum_dimension = width / 2U < height ? width / 2U : height;
    } else if (height > width) {
        center_y = (int)height / 4;
        minimum_dimension = height / 2U < width ? height / 2U : width;
    } else {
        minimum_dimension = width;
    }
    radius = (int)(minimum_dimension * 7U / 20U);
    step = radius / 140;
    if (step < 1)
        step = 1;
    for (y = center_y - radius; y <= center_y + radius; y += step) {
        for (x = center_x - radius; x <= center_x + radius; x += step) {
            int delta_x = x - center_x;
            int delta_y = y - center_y;
            unsigned int value;
            if (x < 0 || y < 0 || x >= (int)width || y >= (int)height ||
                delta_x * delta_x + delta_y * delta_y > radius * radius)
                continue;
            value = luma[(size_t)y * stride + (size_t)x];
            ++bins[value >> 2U];
        }
    }
    result = 0;
release:
    if (api->vpss_release_channel_frame(display->api_context,
                                        RK_HISTOGRAM_VPSS_GROUP,
                                        RK_HISTOGRAM_VPSS_CHANNEL,
                                        frame) != 0)
        result = -1;
    return result;
}

static unsigned char clamp_rgb(int value)
{
    if (value < 0)
        return 0U;
    if (value > 255)
        return 255U;
    return (unsigned char)value;
}

static int encode_rotated_nv12_bmp(struct ngcd_rk_display *display,
                                   const unsigned char *source,
                                   size_t source_size, uint32_t width,
                                   uint32_t height, uint32_t stride,
                                   uint32_t virtual_height,
                                   const unsigned char **data, size_t *size)
{
    const size_t header_size = 54U;
    uint32_t output_width;
    uint32_t output_height;
    size_t row_size;
    size_t pixel_size;
    size_t file_size;
    size_t luma_size;
    size_t output_y;
    if (display == NULL || source == NULL || data == NULL || size == NULL ||
        width < 64U || width > 8192U || height < 64U || height > 8192U ||
        stride < width || stride > 8192U || virtual_height < height ||
        virtual_height > 8192U ||
        (size_t)stride > SIZE_MAX / (size_t)virtual_height)
        return -1;
    luma_size = (size_t)stride * (size_t)virtual_height;
    if (luma_size > SIZE_MAX - luma_size / 2U ||
        source_size < luma_size + luma_size / 2U)
        return -1;

    /* LCD device 3 is physically 480x800 although the camera is used as an
     * 800x480 landscape display. Rotate its composed write-back frame into
     * the operator's logical orientation. */
    output_width = height;
    output_height = width;
    row_size = ((size_t)output_width * 3U + 3U) & ~(size_t)3U;
    if ((size_t)output_height > SIZE_MAX / row_size)
        return -1;
    pixel_size = row_size * (size_t)output_height;
    if (pixel_size > SIZE_MAX - header_size)
        return -1;
    file_size = header_size + pixel_size;
    if (file_size > 2U * 1024U * 1024U)
        return -1;
    if (display->screenshot_bmp_capacity < file_size) {
        unsigned char *resized = realloc(display->screenshot_bmp, file_size);
        if (resized == NULL)
            return -1;
        display->screenshot_bmp = resized;
        display->screenshot_bmp_capacity = file_size;
    }
    memset(display->screenshot_bmp, 0, header_size);
    display->screenshot_bmp[0] = 'B';
    display->screenshot_bmp[1] = 'M';
    put_u32(display->screenshot_bmp, 2U, (uint32_t)file_size);
    put_u32(display->screenshot_bmp, 10U, (uint32_t)header_size);
    put_u32(display->screenshot_bmp, 14U, 40U);
    put_u32(display->screenshot_bmp, 18U, output_width);
    put_u32(display->screenshot_bmp, 22U, output_height);
    put_u16(display->screenshot_bmp, 26U, 1U);
    put_u16(display->screenshot_bmp, 28U, 24U);
    put_u32(display->screenshot_bmp, 34U, (uint32_t)pixel_size);

    for (output_y = 0U; output_y < output_height; ++output_y) {
        size_t logical_y = (size_t)output_height - 1U - output_y;
        unsigned char *output = display->screenshot_bmp + header_size +
                                output_y * row_size;
        size_t output_x;
        for (output_x = 0U; output_x < output_width; ++output_x) {
            size_t source_x = (size_t)width - 1U - logical_y;
            size_t source_y = output_x;
            size_t chroma = luma_size + (source_y / 2U) * stride +
                            (source_x & ~(size_t)1U);
            int y = (int)source[source_y * stride + source_x] - 16;
            int u = (int)source[chroma] - 128;
            int v = (int)source[chroma + 1U] - 128;
            int red;
            int green;
            int blue;
            if (y < 0)
                y = 0;
            red = (298 * y + 409 * v + 128) >> 8;
            green = (298 * y - 100 * u - 208 * v + 128) >> 8;
            blue = (298 * y + 516 * u + 128) >> 8;
            output[output_x * 3U] = clamp_rgb(blue);
            output[output_x * 3U + 1U] = clamp_rgb(green);
            output[output_x * 3U + 2U] = clamp_rgb(red);
        }
    }
    *data = display->screenshot_bmp;
    *size = file_size;
    return 0;
}

int ngcd_rk_display_screenshot_bmp(
    struct ngcd_rk_display *display,
    const unsigned char **data, size_t *size)
{
    unsigned char frame[NGCD_RK_VIDEO_FRAME_SIZE];
    const struct ngcd_rk_api *api;
    const unsigned char *source;
    void *handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t virtual_height;
    size_t block_size;
    int attempt;
    int pass;
    int result = -1;
    int release_result;
    if (display == NULL || data == NULL || size == NULL ||
        display->api == NULL || !display->device_started)
        return -1;
    *data = NULL;
    *size = 0U;
    api = display->api;
    if (api->mb_handle_to_address == NULL || api->mb_get_size == NULL ||
        start_histogram(display) != 0)
        return -1;

    /* Prime a newly enabled WBC/VPSS path with three video-layer frames
     * before changing its source. The physical compositor needs multiple
     * 25-fps cycles after a cold enable; otherwise its first LCD-device frame
     * can be blocked behind a full startup queue. */
    for (pass = 0; pass < 3; ++pass) {
        for (attempt = 0; attempt < 3; ++attempt) {
            memset(frame, 0, sizeof(frame));
            if (api->vpss_get_channel_frame(
                    display->api_context, RK_HISTOGRAM_VPSS_GROUP,
                    RK_HISTOGRAM_VPSS_CHANNEL, frame, 40) == 0)
                break;
        }
        if (attempt == 3 || api->vpss_release_channel_frame(
                display->api_context, RK_HISTOGRAM_VPSS_GROUP,
                RK_HISTOGRAM_VPSS_CHANNEL, frame) != 0)
            goto restore;
    }
    if (ngcd_rk_display_histogram_suspend(display) != 0)
        goto restore;

    /* Discard one frame after changing WBC from the video layer to the LCD
     * device. This prevents a queued video-only frame from becoming the
     * screenshot before the graphics plane has been composited. */
    for (pass = 0; pass < 2; ++pass) {
        for (attempt = 0; attempt < 3; ++attempt) {
            memset(frame, 0, sizeof(frame));
            if (api->vpss_get_channel_frame(
                    display->api_context, RK_HISTOGRAM_VPSS_GROUP,
                    RK_HISTOGRAM_VPSS_CHANNEL, frame, 40) == 0)
                break;
        }
        if (attempt == 3)
            goto restore;
        if (pass == 0) {
            if (api->vpss_release_channel_frame(
                    display->api_context, RK_HISTOGRAM_VPSS_GROUP,
                    RK_HISTOGRAM_VPSS_CHANNEL, frame) != 0)
                goto restore;
        }
    }

    width = get_u32(frame, 8U);
    height = get_u32(frame, 12U);
    stride = get_u32(frame, 16U);
    virtual_height = get_u32(frame, 20U);
    if (virtual_height == 0U)
        virtual_height = height;
    handle = get_pointer(frame, 0U);
    source = get_pointer(frame, 48U);
    if (source == NULL && handle != NULL)
        source = api->mb_handle_to_address(display->api_context, handle);
    block_size = handle != NULL
                     ? api->mb_get_size(display->api_context, handle) : 0U;
    result = encode_rotated_nv12_bmp(display, source, block_size, width,
                                     height, stride, virtual_height,
                                     data, size);
    release_result = api->vpss_release_channel_frame(
        display->api_context, RK_HISTOGRAM_VPSS_GROUP,
        RK_HISTOGRAM_VPSS_CHANNEL, frame);
    if (release_result != 0)
        result = -1;

restore:
    if (start_histogram(display) != 0)
        result = -1;
    if (result != 0) {
        *data = NULL;
        *size = 0U;
    }
    return result;
}
