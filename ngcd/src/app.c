#include "ngcd.h"

#include <stdio.h>
#include <string.h>

#ifndef NGCD_BUILD_ID
#define NGCD_BUILD_ID "development"
#endif

#ifndef NGCD_BUILD_TIME
#define NGCD_BUILD_TIME __DATE__ " " __TIME__
#endif

static void copy_value(char *destination, size_t size, const char *value)
{
    size_t length = strlen(value);
    if (length >= size)
        length = size - 1;
    memcpy(destination, value, length);
    destination[length] = '\0';
}

void ngcd_app_init(struct ngcd_app *app)
{
    int channel;
    memset(app, 0, sizeof(*app));
    app->manufacturer = "VIEWPT";
    app->brand = "CALF";
    app->product = "VR180 Camera";
    app->version = "ngcd-c-0.1";
    app->build_time = NGCD_BUILD_TIME;
    app->hardware = "4.0";
    app->serial_number = "unknown";
    app->backend.ops = ngcd_mock_backend_ops();
    app->backend.state.backlight = 151;
    app->backend.state.backlight_saved = 151;
    app->backend.state.audio_auto = 1;
    app->backend.state.audio_volume[0] = 80;
    app->backend.state.audio_volume[1] = 80;
    app->backend.state.audio_volume[2] = 80;
    copy_value(app->backend.state.camera_mode,
               sizeof(app->backend.state.camera_mode), "VR180_PIC");
    copy_value(app->backend.state.image.exposure,
               sizeof(app->backend.state.image.exposure), "-1");
    copy_value(app->backend.state.image.iso,
               sizeof(app->backend.state.image.iso), "auto");
    copy_value(app->backend.state.image.white_balance,
               sizeof(app->backend.state.image.white_balance), "auto");
    copy_value(app->backend.state.image.exposure_compensation,
               sizeof(app->backend.state.image.exposure_compensation), "0");
    copy_value(app->backend.state.image.anti_flicker,
               sizeof(app->backend.state.image.anti_flicker), "auto");
    copy_value(app->backend.state.image.effect,
               sizeof(app->backend.state.image.effect), "none");
    app->backend.state.image.brightness = 10;
    app->backend.state.image.contrast = 10;
    app->backend.state.image.saturation = 10;
    app->backend.state.image.sharpness = 10;
    app->backend.state.image.noise_reduction = 5;
    for (channel = 0; channel < 3; ++channel) {
        struct ngcd_encoder_state *encoder = &app->backend.state.encoder[channel];
        copy_value(encoder->codec, sizeof(encoder->codec), "H264");
        copy_value(encoder->rate_control, sizeof(encoder->rate_control), "CBR");
        copy_value(encoder->profile, sizeof(encoder->profile), "HIGH");
        encoder->width = 7680;
        encoder->height = 3840;
        encoder->fps = 30;
        encoder->bitrate = channel == 0 ? 100000 : 30000;
        encoder->gop = channel == 0 ? 30 : 10;
    }
}

static int read_identity_value(const char *path, char *value, size_t capacity)
{
    FILE *file;
    size_t length;
    int failed;
    if (capacity < 2U)
        return -1;
    file = fopen(path, "rb");
    if (file == NULL)
        return -1;
    length = fread(value, 1U, capacity - 1U, file);
    failed = ferror(file);
    if (fclose(file) != 0)
        failed = 1;
    if (failed || length == 0U)
        return -1;
    value[length] = '\0';
    while (length > 0U && (value[length - 1U] == '\0' ||
                           value[length - 1U] == '\n' ||
                           value[length - 1U] == '\r' ||
                           value[length - 1U] == ' ' ||
                           value[length - 1U] == '\t'))
        value[--length] = '\0';
    return length > 0U ? 0 : -1;
}

void ngcd_app_load_product_identity(struct ngcd_app *app)
{
    char firmware_version[32];
    int count;
    if (read_identity_value("/etc/manufacturer", app->manufacturer_storage,
                            sizeof(app->manufacturer_storage)) == 0)
        app->manufacturer = app->manufacturer_storage;
    if (read_identity_value("/etc/brand", app->brand_storage,
                            sizeof(app->brand_storage)) == 0)
        app->brand = app->brand_storage;
    if (read_identity_value("/etc/product", app->product_storage,
                            sizeof(app->product_storage)) == 0)
        app->product = app->product_storage;
    if (read_identity_value("/etc/hardware", app->hardware_storage,
                            sizeof(app->hardware_storage)) == 0)
        app->hardware = app->hardware_storage;
    if (read_identity_value("/param/serial_number",
                            app->serial_number_storage,
                            sizeof(app->serial_number_storage)) == 0)
        app->serial_number = app->serial_number_storage;
    if (read_identity_value("/etc/version", firmware_version,
                            sizeof(firmware_version)) == 0)
        count = snprintf(app->version_storage, sizeof(app->version_storage),
                         "%s / ngcd-c-0.1+%s", firmware_version,
                         NGCD_BUILD_ID);
    else
        count = snprintf(app->version_storage, sizeof(app->version_storage),
                         "ngcd-c-0.1+%s", NGCD_BUILD_ID);
    if (count > 0 && (size_t)count < sizeof(app->version_storage))
        app->version = app->version_storage;
    copy_value(app->build_time_storage, sizeof(app->build_time_storage),
               NGCD_BUILD_TIME);
    app->build_time = app->build_time_storage;
}
