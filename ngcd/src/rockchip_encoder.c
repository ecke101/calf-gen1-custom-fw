#include "ngcd_rk.h"

#include <stdint.h>
#include <string.h>

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static int equal_text(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;
    do {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'a' && a <= 'z')
            a = (unsigned char)(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z')
            b = (unsigned char)(b - ('a' - 'A'));
        if (a != b)
            return 0;
    } while (a != '\0');
    return 1;
}

static int h264_profile(const char *profile, uint32_t *value)
{
    if (equal_text(profile, "BASE") || equal_text(profile, "BASELINE"))
        *value = 66U;
    else if (equal_text(profile, "MAIN"))
        *value = 77U;
    else if (equal_text(profile, "HIGH"))
        *value = 100U;
    else
        return -1;
    return 0;
}

static int rate_control_mode(const struct ngcd_encoder_state *encoder,
                             bool h264, uint32_t *value)
{
    uint32_t base;
    if (equal_text(encoder->rate_control, "CBR"))
        base = 1U;
    else if (equal_text(encoder->rate_control, "VBR"))
        base = 2U;
    else if (equal_text(encoder->rate_control, "AVBR"))
        base = 3U;
    else
        return -1;
    /* Rockit 2.2.1 orders H.264 modes 1..3 and H.265 modes 8..10. */
    *value = h264 ? base : base + 7U;
    return 0;
}

int ngcd_rk_encoder_attributes(
    const struct ngcd_encoder_state *encoder,
    struct ngcd_rk_venc_chn_attr *attribute,
    struct ngcd_rk_venc_rc_param *rate_control)
{
    uint64_t pixels;
    uint64_t buffer_size;
    uint32_t codec;
    uint32_t mode;
    bool h264;
    static const uint32_t rc_parameter[] = {
        23U, 2U, 48U, 15U, 45U, 20U, 2U, 1U,
    };

    if (encoder == NULL || attribute == NULL || rate_control == NULL ||
        encoder->width <= 0 || encoder->width > 16384 ||
        encoder->height <= 0 || encoder->height > 16384 ||
        encoder->fps <= 0 || encoder->fps > 240 ||
        encoder->bitrate <= 0 || encoder->bitrate > 1000000 ||
        encoder->gop <= 0 || encoder->gop > 1000 ||
        (encoder->color_range != 0 && encoder->color_range != 1))
        return -1;
    if (equal_text(encoder->codec, "H264")) {
        codec = 8U;
        h264 = true;
    } else if (equal_text(encoder->codec, "H265")) {
        codec = 12U;
        h264 = false;
    } else {
        return -1;
    }
    if (rate_control_mode(encoder, h264, &mode) != 0)
        return -1;
    pixels = (uint64_t)(unsigned int)encoder->width *
             (uint64_t)(unsigned int)encoder->height;
    buffer_size = pixels + pixels / 2U;
    if (buffer_size > UINT32_MAX)
        return -1;

    memset(attribute, 0, sizeof(*attribute));
    memset(rate_control, 0, sizeof(*rate_control));
    put_u32(attribute->bytes, 0U, codec);
    put_u32(attribute->bytes, 12U, (uint32_t)buffer_size);
    if (h264) {
        uint32_t profile;
        if (h264_profile(encoder->profile, &profile) != 0)
            return -1;
        put_u32(attribute->bytes, 16U, profile);
    }
    put_u32(attribute->bytes, 24U, (uint32_t)encoder->width);
    put_u32(attribute->bytes, 28U, (uint32_t)encoder->height);
    put_u32(attribute->bytes, 32U, (uint32_t)encoder->width);
    put_u32(attribute->bytes, 36U, (uint32_t)encoder->height);
    put_u32(attribute->bytes, 40U, 6U); /* RK_FMT_YUV420SP */
    put_u32(attribute->bytes, 72U, mode);
    /* This firmware's RC union stores GOP first and bitrate after both frame
     * rate fractions.  The encoder SEI exposes these accepted values, making
     * the otherwise easy-to-confuse offsets observable on real hardware. */
    put_u32(attribute->bytes, 76U, (uint32_t)encoder->gop);
    put_u32(attribute->bytes, 80U, (uint32_t)encoder->fps);
    put_u32(attribute->bytes, 84U, 1U);
    put_u32(attribute->bytes, 88U, (uint32_t)encoder->fps);
    put_u32(attribute->bytes, 92U, 1U);
    put_u32(attribute->bytes, 96U, (uint32_t)encoder->bitrate);
    if (equal_text(encoder->rate_control, "CBR")) {
        put_u32(attribute->bytes, 100U, 3U);
    } else {
        put_u32(attribute->bytes, 100U, (uint32_t)encoder->bitrate);
        put_u32(attribute->bytes, 104U, (uint32_t)encoder->bitrate);
        put_u32(attribute->bytes, 108U, 3U);
    }
    put_u32(attribute->bytes, 112U, 1U);
    memcpy(rate_control->bytes, rc_parameter, sizeof(rc_parameter));
    return 0;
}
