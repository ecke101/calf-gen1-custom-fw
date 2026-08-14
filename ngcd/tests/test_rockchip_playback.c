#include "ngcd_mp4.h"
#include "ngcd_rk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fake_playback {
    unsigned char *memory;
    size_t memory_size;
    void *wrapped_memory;
    size_t wrapped_size;
    int create_decoder;
    int destroy_decoder;
    int start_decoder;
    int stop_decoder;
    int reset_decoder;
    int sent;
    int media_buffers_created;
    int media_buffers_released;
    int groups_created;
    int groups_destroyed;
    int channels_enabled;
    int channels_disabled;
    int binds;
    int unbinds;
    struct ngcd_rk_channel bind_source[3];
    struct ngcd_rk_channel bind_destination[3];
    uint32_t decoder_codec;
    uint32_t decoder_mode;
    uint32_t decoder_width;
    uint32_t decoder_height;
    uint32_t decoder_duplicate_width;
    uint32_t decoder_duplicate_height;
    uint32_t decoder_byte_size;
    uint32_t decoder_picture_buffers;
    uint32_t decoder_stream_buffers;
    uint32_t decoder_parameter_codec;
    uint32_t decoder_parameter_nonzero;
    int decoder_display_mode;
    uint32_t output_width[3];
    uint32_t output_height[3];
    uint32_t output_mode[3];
    uint32_t group_width;
    uint32_t group_height;
    uint32_t group_compression;
    uint32_t status_received;
    uint32_t status_decoded;
    uint32_t status_pending_stream;
    uint32_t status_pending_pictures;
    uint32_t status_errors;
    int output_available;
    int output_presented;
    int output_released;
    int vo_splice_mode;
    int vo_channel;
    uint32_t vo_x;
    uint32_t vo_y;
    uint32_t vo_width;
    uint32_t vo_height;
};

static uint32_t read_u32(const void *data, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)data + offset, sizeof(value));
    return value;
}

static void write_u32(void *data, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)data + offset, &value, sizeof(value));
}

static int okay_one(void *context, int value)
{
    (void)context;
    (void)value;
    return 0;
}

static int okay_two(void *context, int first, int second)
{
    (void)context;
    (void)first;
    (void)second;
    return 0;
}

static int okay_three(void *context, int first, int second, int third)
{
    (void)context;
    (void)first;
    (void)second;
    (void)third;
    return 0;
}

static int okay_attribute_two(void *context, int first,
                              const void *attribute)
{
    (void)context;
    (void)first;
    (void)attribute;
    return 0;
}

static int fake_bind(void *context, const struct ngcd_rk_channel *source,
                     const struct ngcd_rk_channel *destination)
{
    struct fake_playback *fake = context;
    assert(source != NULL && destination != NULL);
    assert(fake->binds < 3);
    fake->bind_source[fake->binds] = *source;
    fake->bind_destination[fake->binds] = *destination;
    ++fake->binds;
    return 0;
}

static int fake_unbind(void *context, const struct ngcd_rk_channel *source,
                       const struct ngcd_rk_channel *destination)
{
    struct fake_playback *fake = context;
    assert(source != NULL && destination != NULL);
    ++fake->unbinds;
    return 0;
}

static int fake_mmz_alloc(void *context, void **handle, size_t bytes)
{
    struct fake_playback *fake = context;
    fake->memory = malloc(bytes);
    assert(fake->memory != NULL);
    fake->memory_size = bytes;
    *handle = fake->memory;
    return 0;
}

static int fake_mmz_free(void *context, void *handle)
{
    struct fake_playback *fake = context;
    assert(handle == fake->memory);
    free(fake->memory);
    fake->memory = NULL;
    fake->memory_size = 0U;
    return 0;
}

static int fake_mb_create(void *context, void **handle, void *address,
                          size_t bytes)
{
    struct fake_playback *fake = context;
    assert(handle != NULL && address != NULL && bytes > 0U);
    assert(fake->wrapped_memory == NULL);
    fake->wrapped_memory = address;
    fake->wrapped_size = bytes;
    *handle = address;
    ++fake->media_buffers_created;
    return 0;
}

static int fake_mb_release(void *context, void *handle)
{
    struct fake_playback *fake = context;
    assert(handle == fake->wrapped_memory && fake->wrapped_size > 0U);
    fake->wrapped_memory = NULL;
    fake->wrapped_size = 0U;
    ++fake->media_buffers_released;
    return 0;
}

static void *fake_address(void *context, void *handle)
{
    struct fake_playback *fake = context;
    return handle == fake->memory ? handle : NULL;
}

static size_t fake_size(void *context, void *handle)
{
    struct fake_playback *fake = context;
    return handle == fake->memory ? fake->memory_size : 0U;
}

static int fake_vdec_create(void *context, int channel,
                            const void *attribute)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    ++fake->create_decoder;
    fake->decoder_mode = read_u32(attribute, 0U);
    fake->decoder_codec = read_u32(attribute, 4U);
    fake->decoder_width = read_u32(attribute, 8U);
    fake->decoder_height = read_u32(attribute, 12U);
    fake->decoder_duplicate_width = read_u32(attribute, 16U);
    fake->decoder_duplicate_height = read_u32(attribute, 20U);
    fake->decoder_byte_size = read_u32(attribute, 24U);
    fake->decoder_picture_buffers = read_u32(attribute, 32U);
    fake->decoder_stream_buffers = read_u32(attribute, 36U);
    return 0;
}

static int fake_vdec_destroy(void *context, int channel)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    ++fake->destroy_decoder;
    return 0;
}

static int fake_vdec_start(void *context, int channel)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    ++fake->start_decoder;
    return 0;
}

static int fake_vdec_stop(void *context, int channel)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    ++fake->stop_decoder;
    return 0;
}

static int fake_vdec_reset(void *context, int channel)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    ++fake->reset_decoder;
    return 0;
}

static int fake_vdec_send(void *context, int channel, const void *stream,
                          int timeout_ms)
{
    struct fake_playback *fake = context;
    void *handle;
    assert(channel == 0 && timeout_ms == 100);
    memcpy(&handle, stream, sizeof(handle));
    assert(handle == (fake->memory != NULL
                          ? (void *)fake->memory : fake->wrapped_memory));
    assert(read_u32(stream, 8U) > 0U);
    assert(read_u32(stream, 28U) == 0U);
    /* A single reusable input block is only safe in Rockit's copy mode. */
    assert(read_u32(stream, 32U) == 0U);
    ++fake->sent;
    return 0;
}

static int fake_vdec_parameter(void *context, int channel,
                               const void *parameter)
{
    struct fake_playback *fake = context;
    size_t offset;
    assert(channel == 0);
    fake->decoder_parameter_codec = read_u32(parameter, 0U);
    for (offset = 4U; offset < NGCD_RK_VDEC_CHN_PARAM_SIZE;
         offset += sizeof(uint32_t))
        fake->decoder_parameter_nonzero |= read_u32(parameter, offset);
    return 0;
}

static int fake_vdec_display_mode(void *context, int channel, int mode)
{
    struct fake_playback *fake = context;
    assert(channel == 0);
    fake->decoder_display_mode = mode;
    return 0;
}

static int fake_vdec_status(void *context, int channel, void *status)
{
    struct fake_playback *fake = context;
    assert(channel == 0 && status != NULL);
    write_u32(status, 8U, fake->status_pending_stream);
    write_u32(status, 12U, fake->status_pending_pictures);
    write_u32(status, 20U, fake->status_received);
    write_u32(status, 24U, fake->status_decoded);
    write_u32(status, 28U, fake->status_errors);
    return 0;
}

static int fake_vpss_create(void *context, int group, const void *attribute)
{
    struct fake_playback *fake = context;
    assert(group == 2 && fake->groups_created == 0);
    fake->group_width = read_u32(attribute, 0U);
    fake->group_height = read_u32(attribute, 4U);
    fake->group_compression = read_u32(attribute, 24U);
    ++fake->groups_created;
    return 0;
}

static int fake_vpss_destroy(void *context, int group)
{
    struct fake_playback *fake = context;
    assert(group == 2);
    ++fake->groups_destroyed;
    return 0;
}

static int fake_vpss_attribute(void *context, int group, int channel,
                               const void *attribute)
{
    struct fake_playback *fake = context;
    assert(group == 2 && channel == 1);
    fake->output_mode[group] = read_u32(attribute, 0U);
    fake->output_width[group] = read_u32(attribute, 4U);
    fake->output_height[group] = read_u32(attribute, 8U);
    return 0;
}

static int fake_vpss_enable(void *context, int group, int channel)
{
    struct fake_playback *fake = context;
    assert(group == 2 && channel == 1);
    ++fake->channels_enabled;
    return 0;
}

static int fake_vpss_disable(void *context, int group, int channel)
{
    struct fake_playback *fake = context;
    assert(group == 2 && channel == 1);
    ++fake->channels_disabled;
    return 0;
}

static int fake_vpss_get_frame(void *context, int group, int channel,
                               void *frame, int timeout_ms)
{
    struct fake_playback *fake = context;
    assert(group == 2 && channel == 1 && frame != NULL && timeout_ms == 0);
    return fake->output_available > 0 ? 0 : -1;
}

static int fake_vpss_release_frame(void *context, int group, int channel,
                                   const void *frame)
{
    struct fake_playback *fake = context;
    assert(group == 2 && channel == 1 && frame != NULL);
    assert(fake->output_available > 0);
    --fake->output_available;
    ++fake->output_released;
    return 0;
}

static int fake_vo_send(void *context, int layer, int channel,
                        const void *frame, int timeout_ms)
{
    struct fake_playback *fake = context;
    assert(layer == 3 && channel == 1 && frame != NULL && timeout_ms == 0);
    ++fake->output_presented;
    return 0;
}

static int fake_vo_splice(void *context, int layer, int mode)
{
    struct fake_playback *fake = context;
    assert(layer == 3 && mode == 0);
    fake->vo_splice_mode = mode;
    return 0;
}

static int fake_vo_channel_attr(void *context, int layer, int channel,
                                const void *attribute)
{
    struct fake_playback *fake = context;
    assert(layer == 3 && channel == 1 && attribute != NULL);
    fake->vo_channel = channel;
    fake->vo_x = read_u32(attribute, 4U);
    fake->vo_y = read_u32(attribute, 8U);
    fake->vo_width = read_u32(attribute, 12U);
    fake->vo_height = read_u32(attribute, 16U);
    return 0;
}

static int fake_vo_channel(void *context, int layer, int channel)
{
    struct fake_playback *fake = context;
    assert(layer == 3 && channel == 1);
    fake->vo_channel = channel;
    return 0;
}

static void fill_api(struct ngcd_rk_api *api)
{
    memset(api, 0, sizeof(*api));
    api->bind = fake_bind;
    api->unbind = fake_unbind;
    api->vdec_create_channel = fake_vdec_create;
    api->vdec_destroy_channel = fake_vdec_destroy;
    api->vdec_start_receive = fake_vdec_start;
    api->vdec_stop_receive = fake_vdec_stop;
    api->vdec_reset_channel = fake_vdec_reset;
    api->vdec_send_stream = fake_vdec_send;
    api->vdec_set_channel_param = fake_vdec_parameter;
    api->vdec_set_display_mode = fake_vdec_display_mode;
    api->vdec_query_status = fake_vdec_status;
    api->vpss_create_group = fake_vpss_create;
    api->vpss_destroy_group = fake_vpss_destroy;
    api->vpss_set_device = okay_two;
    api->vpss_enable_backup = okay_one;
    api->vpss_start_group = okay_one;
    api->vpss_stop_group = okay_one;
    api->vpss_set_channel_attr = fake_vpss_attribute;
    api->vpss_enable_channel = fake_vpss_enable;
    api->vpss_disable_channel = fake_vpss_disable;
    api->vpss_get_channel_frame = fake_vpss_get_frame;
    api->vpss_release_channel_frame = fake_vpss_release_frame;
    api->vo_bind_layer = okay_three;
    api->vo_unbind_layer = okay_two;
    api->vo_set_layer_buffer_length = okay_two;
    api->vo_set_layer_attr = okay_attribute_two;
    api->vo_set_layer_splice_mode = fake_vo_splice;
    api->vo_enable_layer = okay_one;
    api->vo_disable_layer = okay_one;
    api->vo_set_channel_attr = fake_vo_channel_attr;
    api->vo_enable_channel = fake_vo_channel;
    api->vo_disable_channel = fake_vo_channel;
    api->vo_send_frame = fake_vo_send;
    api->mmz_alloc = fake_mmz_alloc;
    api->mmz_free = fake_mmz_free;
    api->mb_create = fake_mb_create;
    api->mb_release = fake_mb_release;
    api->mb_handle_to_address = fake_address;
    api->mb_get_size = fake_size;
}

int main(void)
{
    static const char jpeg_path[] = "/tmp/ngcd-playback-test.jpg";
    static const char video_path[] = "/tmp/ngcd-playback-test.mp4";
    static const unsigned char jpeg[] = {
        0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08,
        0x0f, 0x00, 0x1e, 0x00, 0x03, 0x01, 0x11,
        0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xd9,
    };
    static const unsigned char first_frame[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xb7,
        0xfe, 0x5c, 0x05, 0x05, 0x05, 0x02,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xa0,
    };
    static const unsigned char next_frame[] = {
        0x00, 0x00, 0x01, 0x41, 0x9a, 0x22, 0x11,
    };
    struct ngcd_rk_api api;
    struct fake_playback fake;
    struct ngcd_rk_playback *playback = NULL;
    struct ngcd_mp4_writer *writer = NULL;
    FILE *file;
    memset(&fake, 0, sizeof(fake));
    fill_api(&api);
    file = fopen(jpeg_path, "wbx");
    assert(file != NULL && fwrite(jpeg, 1U, sizeof(jpeg), file) ==
                               sizeof(jpeg) && fclose(file) == 0);
    assert(ngcd_rk_playback_open(&playback, &api, &fake, NULL,
                                 jpeg_path) == 0);
    assert(ngcd_rk_playback_is_picture(playback));
    assert(ngcd_rk_playback_width(playback) == 7680U);
    assert(ngcd_rk_playback_height(playback) == 3840U);
    assert(ngcd_rk_playback_create_time(playback) > 0U);
    assert(fake.decoder_codec == 15U);
    assert(fake.decoder_mode == 1U);
    assert(fake.decoder_duplicate_width == 0U);
    assert(fake.decoder_duplicate_height == 0U);
    assert(fake.decoder_byte_size == 0U);
    assert(fake.decoder_picture_buffers == 16U);
    assert(fake.decoder_stream_buffers == 8U);
    assert(fake.decoder_parameter_codec == 15U);
    assert(fake.decoder_parameter_nonzero == 0U);
    assert(fake.decoder_display_mode == 0);
    assert(fake.vo_splice_mode == 0 && fake.vo_channel == 1);
    assert(fake.vo_x == 0U && fake.vo_y == 0U);
    assert(fake.vo_width == 480U && fake.vo_height == 800U);
    assert(fake.groups_created == 1 && fake.channels_enabled == 1);
    assert(fake.group_width == 800U && fake.group_height == 480U);
    assert(fake.group_compression == 1U);
    assert(fake.output_mode[2] == 2U);
    assert(fake.output_width[2] == 800U && fake.output_height[2] == 480U);
    assert(fake.binds == 2 && fake.sent == 1);
    assert(fake.bind_source[0].module == 6 &&
           fake.bind_source[0].device == 2 &&
           fake.bind_source[0].channel == 1);
    assert(fake.bind_destination[0].module == 9 &&
           fake.bind_destination[0].device == 3 &&
           fake.bind_destination[0].channel == 1);
    assert(fake.bind_source[1].module == 5 &&
           fake.bind_source[1].device == 0 &&
           fake.bind_source[1].channel == 0);
    assert(fake.bind_destination[1].module == 6 &&
           fake.bind_destination[1].device == 2 &&
           fake.bind_destination[1].channel == 1);
    fake.status_received = 7U;
    fake.status_decoded = 6U;
    fake.status_pending_stream = 2U;
    fake.status_pending_pictures = 1U;
    fake.status_errors = 3U;
    assert(ngcd_rk_playback_tick(playback, UINT64_C(1000000)) == 0);
    assert(ngcd_rk_playback_decoder_received(playback) == 7U);
    assert(ngcd_rk_playback_decoder_decoded(playback) == 6U);
    assert(ngcd_rk_playback_decoder_pending_stream(playback) == 2U);
    assert(ngcd_rk_playback_decoder_pending_pictures(playback) == 1U);
    assert(ngcd_rk_playback_decoder_errors(playback) == 3U);
    assert(ngcd_rk_playback_presented_frames(playback) == 0U);
    assert(ngcd_rk_playback_output_errors(playback) == 0U);
    assert(fake.output_presented == 0 && fake.output_released == 0);
    ngcd_rk_playback_close(playback);
    assert(fake.unbinds == 2 && fake.groups_destroyed == 1);
    assert(fake.destroy_decoder == 1 && fake.memory == NULL);
    assert(unlink(jpeg_path) == 0);

    memset(&fake, 0, sizeof(fake));
    fill_api(&api);
    assert(ngcd_mp4_open(&writer, video_path, 7680U, 3840U, 30U) == 0);
    assert(ngcd_mp4_write_h264(writer, first_frame,
                               sizeof(first_frame), 0U) == 0);
    assert(ngcd_mp4_write_h264(writer, next_frame,
                               sizeof(next_frame), 33333U) == 0);
    assert(ngcd_mp4_write_h264(writer, next_frame,
                               sizeof(next_frame), 66666U) == 0);
    assert(ngcd_mp4_write_h264(writer, next_frame,
                               sizeof(next_frame), 99999U) == 0);
    assert(ngcd_mp4_write_h264(writer, next_frame,
                               sizeof(next_frame), 133332U) == 0);
    assert(ngcd_mp4_close(writer) == 0);
    assert(ngcd_rk_playback_open(&playback, &api, &fake, NULL,
                                 video_path) == 0);
    assert(!ngcd_rk_playback_is_picture(playback));
    assert(ngcd_rk_playback_create_time(playback) > 0U);
    assert(ngcd_rk_playback_is_paused(playback));
    assert(ngcd_rk_playback_sample_index(playback) == 3U);
    assert(fake.sent == 4);
    assert(fake.media_buffers_created == 4);
    assert(fake.media_buffers_released == 4);
    assert(fake.wrapped_memory == NULL);
    ngcd_rk_playback_close(playback);
    assert(fake.unbinds == 2 && fake.groups_destroyed == 1);
    assert(fake.destroy_decoder == 1 && fake.memory == NULL);
    assert(unlink(video_path) == 0);
    puts("ngcd Rockchip playback tests passed");
    return 0;
}
