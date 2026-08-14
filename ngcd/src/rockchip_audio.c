#include "ngcd_rk.h"

#include <stdio.h>
#include <string.h>

enum {
    RK_AO_DEVICE = 0,
    RK_AO_CHANNEL = 1,
    RK_AUDIO_SAMPLE_RATE = 48000,
    RK_AUDIO_CHANNELS = 2,
};

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value)
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

static int read_card_id(char *destination, size_t size)
{
    FILE *file;
    size_t length;
    if (destination == NULL || size < 2U)
        return -1;
    file = fopen("/proc/asound/card0/id", "r");
    if (file == NULL || fgets(destination, (int)size, file) == NULL) {
        if (file != NULL)
            (void)fclose(file);
        return -1;
    }
    if (fclose(file) != 0)
        return -1;
    length = strlen(destination);
    while (length > 0U &&
           (destination[length - 1U] == '\n' ||
            destination[length - 1U] == '\r'))
        destination[--length] = '\0';
    return length > 0U ? 0 : -1;
}

static int audio_output_api_valid(const struct ngcd_rk_api *api)
{
    return api != NULL && api->ao_clear_pub_attr != NULL &&
           api->ao_set_pub_attr != NULL && api->ao_enable != NULL &&
           api->ao_disable != NULL && api->ao_enable_channel != NULL &&
           api->ao_disable_channel != NULL &&
           api->ao_set_channel_param != NULL &&
           api->ao_enable_resample != NULL &&
           api->ao_disable_resample != NULL && api->ao_send_frame != NULL &&
           api->ao_wait_eos != NULL && api->ao_set_volume != NULL &&
           api->ao_get_volume != NULL && api->mb_create != NULL &&
           api->mb_release != NULL;
}

int ngcd_rk_audio_output_start(struct ngcd_rk_audio_output *output,
                               const struct ngcd_rk_api *api,
                               void *api_context)
{
    struct ngcd_rk_aio_attr attribute;
    uint32_t channel_parameter = 10U;
    char card_id[64];
    char card[88];
    int length;
    if (output == NULL || !audio_output_api_valid(api) ||
        api_context == NULL || output->device_started ||
        output->channel_started || output->resample_started ||
        read_card_id(card_id, sizeof(card_id)) != 0)
        return -1;
    length = snprintf(card, sizeof(card), "default:CARD=%s", card_id);
    if (length <= 0 || (size_t)length >= sizeof(card))
        return -1;
    output->api = api;
    output->api_context = api_context;
    memset(&attribute, 0, sizeof(attribute));
    /* Exact ViewPT 2.2.1 ao_module::init_dev layout. */
    put_u32(attribute.bytes, 0U, RK_AUDIO_CHANNELS);
    put_u32(attribute.bytes, 4U, RK_AUDIO_SAMPLE_RATE);
    put_u32(attribute.bytes, 8U, 1U); /* AUDIO_BIT_WIDTH_16 */
    put_u32(attribute.bytes, 12U, RK_AUDIO_SAMPLE_RATE);
    put_u32(attribute.bytes, 16U, 1U); /* AUDIO_SOUND_MODE_STEREO */
    put_u32(attribute.bytes, 28U, 8U);
    put_u32(attribute.bytes, 32U, 1024U);
    put_u32(attribute.bytes, 36U, RK_AUDIO_CHANNELS);
    memcpy(attribute.bytes + 40U, card, (size_t)length + 1U);
    if (api->ao_clear_pub_attr(api_context, RK_AO_DEVICE) != 0 ||
        api->ao_set_pub_attr(api_context, RK_AO_DEVICE, &attribute) != 0 ||
        api->ao_enable(api_context, RK_AO_DEVICE) != 0)
        goto fail;
    output->device_started = true;
    if (api->ao_set_channel_param(api_context, RK_AO_DEVICE,
                                  RK_AO_CHANNEL, &channel_parameter) != 0 ||
        api->ao_enable_channel(api_context, RK_AO_DEVICE,
                               RK_AO_CHANNEL) != 0)
        goto fail;
    output->channel_started = true;
    if (api->ao_enable_resample(api_context, RK_AO_DEVICE,
                                RK_AO_CHANNEL,
                                RK_AUDIO_SAMPLE_RATE) != 0)
        goto fail;
    output->resample_started = true;
    return 0;

fail:
    ngcd_rk_audio_output_stop(output);
    return -1;
}

void ngcd_rk_audio_output_stop(struct ngcd_rk_audio_output *output)
{
    if (output == NULL || output->api == NULL)
        return;
    if (output->resample_started)
        (void)output->api->ao_disable_resample(
            output->api_context, RK_AO_DEVICE, RK_AO_CHANNEL);
    if (output->channel_started)
        (void)output->api->ao_disable_channel(
            output->api_context, RK_AO_DEVICE, RK_AO_CHANNEL);
    if (output->device_started)
        (void)output->api->ao_disable(output->api_context, RK_AO_DEVICE);
    memset(output, 0, sizeof(*output));
}

int ngcd_rk_audio_output_set_volume(struct ngcd_rk_audio_output *output,
                                    int volume, int *readback)
{
    int actual = 0;
    if (output == NULL || output->api == NULL || !output->device_started ||
        volume < 0 || volume > 140 ||
        output->api->ao_set_volume(output->api_context, RK_AO_DEVICE,
                                   volume) != 0 ||
        output->api->ao_get_volume(output->api_context, RK_AO_DEVICE,
                                   &actual) != 0)
        return -1;
    if (readback != NULL)
        *readback = actual;
    return 0;
}

int ngcd_rk_audio_output_send_pcm(struct ngcd_rk_audio_output *output,
                                  const void *data, size_t bytes,
                                  uint64_t pts_us, int timeout_ms)
{
    struct ngcd_rk_audio_frame frame;
    void *handle = NULL;
    int result;
    int release_result;
    if (output == NULL || output->api == NULL || !output->resample_started ||
        data == NULL || bytes == 0U || bytes > UINT32_MAX ||
        (bytes & 3U) != 0U || timeout_ms < 0 ||
        output->api->mb_create(output->api_context, &handle,
                               (void *)data, bytes) != 0 || handle == NULL)
        return -1;
    memset(&frame, 0, sizeof(frame));
    put_pointer(frame.bytes, 0U, handle);
    put_u32(frame.bytes, 8U, 1U);
    put_u32(frame.bytes, 12U, 1U);
    put_u64(frame.bytes, 16U, pts_us);
    put_u32(frame.bytes, 28U, (uint32_t)bytes);
    result = output->api->ao_send_frame(
        output->api_context, RK_AO_DEVICE, RK_AO_CHANNEL, &frame,
        timeout_ms);
    release_result = output->api->mb_release(output->api_context, handle);
    return result != 0 ? result : release_result;
}
