#include "ngcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int mock_start(struct ngcd_backend *backend)
{
    backend->state.camera_running = true;
    return 0;
}

static void mock_stop(struct ngcd_backend *backend)
{
    backend->state.recording = false;
    backend->state.playback = false;
    backend->state.camera_running = false;
}

static int mock_tick(struct ngcd_backend *backend)
{
    (void)backend;
    return 0;
}

static int mock_graphics_control_id(struct ngcd_backend *backend)
{
    (void)backend;
    return 1;
}

static int mock_histogram(struct ngcd_backend *backend,
                          uint32_t bins[NGCD_HISTOGRAM_BINS])
{
    size_t index;
    (void)backend;
    for (index = 0U; index < NGCD_HISTOGRAM_BINS; ++index)
        bins[index] = (uint32_t)(index + 1U);
    return 0;
}

static int mock_lcd_screenshot(struct ngcd_backend *backend,
                               const unsigned char **data, size_t *size)
{
    static const unsigned char bmp[58] = {
        'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
        0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 128, 0,
    };
    (void)backend;
    if (data == NULL || size == NULL)
        return -1;
    *data = bmp;
    *size = sizeof(bmp);
    return 0;
}

static int mock_camera_mode(struct ngcd_backend *backend, const char *mode,
                            bool start)
{
    if (!start) {
        if (backend->state.recording || backend->state.playback)
            return -1;
        backend->state.camera_running = false;
        return 0;
    }
    if (mode == NULL || *mode == '\0' || strlen(mode) >= NGCD_VALUE_MAX)
        return -1;
    if (backend->state.recording || backend->state.playback)
        return -1;
    memcpy(backend->state.camera_mode, mode, strlen(mode) + 1);
    backend->state.camera_running = true;
    return 0;
}

static int mock_set_image(struct ngcd_backend *backend, const char *type,
                          const char *value, bool fixed)
{
    struct ngcd_image_state *image = &backend->state.image;
    char *destination = NULL;
    long integer;
    char *end = NULL;
    if (fixed && strcmp(type, "exp") != 0 && strcmp(type, "iso") != 0)
        return -1;
    if (fixed)
        return value != NULL && *value != '\0' &&
               strlen(value) < NGCD_VALUE_MAX ? 0 : -1;

    if (strcmp(type, "exp") == 0)
        destination = image->exposure;
    else if (strcmp(type, "iso") == 0)
        destination = image->iso;
    else if (strcmp(type, "wb") == 0)
        destination = image->white_balance;
    else if (strcmp(type, "ev") == 0)
        destination = image->exposure_compensation;
    else if (strcmp(type, "antiflicker") == 0)
        destination = image->anti_flicker;
    else if (strcmp(type, "imgeffect") == 0)
        destination = image->effect;

    if (destination != NULL) {
        if (*value == '\0' || strlen(value) >= NGCD_VALUE_MAX)
            return -1;
        memcpy(destination, value, strlen(value) + 1);
        return 0;
    }

    integer = strtol(value, &end, 10);
    if (end == value || *end != '\0' || integer < -100 || integer > 100)
        return -1;
    if (strcmp(type, "brightness") == 0)
        image->brightness = (int)integer;
    else if (strcmp(type, "contrast") == 0)
        image->contrast = (int)integer;
    else if (strcmp(type, "saturation") == 0)
        image->saturation = (int)integer;
    else if (strcmp(type, "hue") == 0)
        image->hue = (int)integer;
    else if (strcmp(type, "sharpness") == 0)
        image->sharpness = (int)integer;
    else if (strcmp(type, "3dnr") == 0)
        image->noise_reduction = (int)integer;
    else
        return -1;
    return 0;
}

static int mock_night_preview(struct ngcd_backend *backend, int fps,
                              const char *exposure, const char *iso)
{
    (void)backend;
    if (fps != 4 && fps != 8 && fps != 15 && fps != 30)
        return -1;
    if (exposure == NULL || exposure[0] == '\0' ||
        iso == NULL || iso[0] == '\0')
        return -1;
    return 0;
}

static int mock_snapshot(struct ngcd_backend *backend, char *filename,
                         size_t filename_size)
{
    static unsigned int sequence;
    int count;
    if (!backend->state.camera_running || backend->state.playback)
        return -1;
    ++sequence;
    count = snprintf(filename, filename_size, "P1%06u.jpg", sequence % 1000000U);
    return count > 0 && (size_t)count < filename_size ? 0 : -1;
}

static int mock_recording(struct ngcd_backend *backend, const char *action,
                          int split_type, uint64_t size_limit,
                          uint64_t time_limit)
{
    (void)split_type;
    (void)size_limit;
    (void)time_limit;
    if (strcmp(action, "start") == 0) {
        if (!backend->state.camera_running || backend->state.playback)
            return -1;
        backend->state.recording = true;
    } else if (strcmp(action, "stop") == 0) {
        backend->state.recording = false;
    } else if (strcmp(action, "toggle") == 0) {
        if (!backend->state.camera_running || backend->state.playback)
            return -1;
        backend->state.recording = !backend->state.recording;
    } else {
        return -1;
    }
    return 0;
}

static int mock_playback(struct ngcd_backend *backend, const char *action,
                         const char *path, int first, int second)
{
    if (backend->state.recording)
        return -1;
    if (strcmp(action, "start") == 0) {
        backend->state.playback = true;
    } else if (strcmp(action, "stop") == 0 ||
               strcmp(action, "close") == 0) {
        backend->state.playback = false;
        backend->state.playback_paused = false;
    } else if (strcmp(action, "open") == 0) {
        if (path == NULL || path[0] != '/')
            return -1;
        backend->state.playback = true;
        backend->state.playback_paused = true;
        backend->state.playback_sample_index = 0;
        backend->state.playback_sample_count = 100;
        backend->state.playback_duration_us = UINT64_C(4000000);
        backend->state.playback_file_size = 1024U;
        memcpy(backend->state.playback_codec, "H264", sizeof("H264"));
        backend->state.playback_width = 3840;
        backend->state.playback_height = 1920;
    } else if (strcmp(action, "pause") == 0) {
        if (!backend->state.playback)
            return -1;
        backend->state.playback_paused = true;
    } else if (strcmp(action, "resume") == 0) {
        if (!backend->state.playback)
            return -1;
        backend->state.playback_paused = false;
    } else if (strcmp(action, "toggle") == 0) {
        if (!backend->state.playback)
            return -1;
        backend->state.playback_paused = !backend->state.playback_paused;
    } else if (strcmp(action, "seek") == 0) {
        if (!backend->state.playback || first < 0)
            return -1;
        backend->state.playback_sample_index =
            second > 0 ? backend->state.playback_sample_count * first / second
                       : first;
        if (backend->state.playback_sample_index >=
            backend->state.playback_sample_count)
            backend->state.playback_sample_index =
                backend->state.playback_sample_count - 1;
    } else {
        return -1;
    }
    return 0;
}

static int mock_stream(struct ngcd_backend *backend, const char *service,
                       const char *action, const char *url)
{
    bool *state;
    (void)url;
    if (strcmp(service, "live") == 0)
        state = &backend->state.live;
    else if (strcmp(service, "rtmp") == 0)
        state = &backend->state.rtmp;
    else if (strcmp(service, "rtsp") == 0)
        state = &backend->state.rtsp;
    else if (strcmp(service, "srt") == 0)
        state = &backend->state.srt;
    else if (strcmp(service, "openstream") == 0)
        state = &backend->state.open_stream;
    else
        return -1;

    if (strstr(action, "start") != NULL)
        *state = true;
    else if (strstr(action, "stop") != NULL)
        *state = false;
    else if (strstr(action, "toggle") != NULL)
        *state = !*state;
    else if (strcmp(action, "add_push_conn") != 0 &&
             strcmp(action, "remove_push_conn") != 0 &&
             strcmp(action, "get_conn_infos") != 0)
        return -1;
    return 0;
}

static int mock_uvc(struct ngcd_backend *backend, bool enable)
{
    if (enable && (backend->state.recording || backend->state.playback))
        return -1;
    backend->state.uvc = enable;
    return 0;
}

static int mock_read_imu(struct ngcd_backend *backend,
                         struct ngcd_imu_sample *sample)
{
    struct timespec now;
    (void)backend;
    memset(sample, 0, sizeof(*sample));
    sample->acceleration_x = 1000;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        sample->monotonic_ns = (uint64_t)now.tv_sec * 1000000000ULL +
                               (uint64_t)now.tv_nsec;
    return 0;
}

static int mock_calibrate_imu(struct ngcd_backend *backend, int type,
                              bool save, int count)
{
    (void)save;
    if (type != 0 || count < 5 || count > 10000)
        return -1;
    backend->state.imu_calibration_state = 3;
    return 0;
}

static int mock_imu_calibration_state(struct ngcd_backend *backend,
                                      int *state)
{
    if (state == NULL)
        return -1;
    *state = backend->state.imu_calibration_state;
    return 0;
}

static int mock_set_encoder(struct ngcd_backend *backend, int channel,
                            const struct ngcd_encoder_state *encoder,
                            uint32_t changed_fields)
{
    (void)changed_fields;
    if (channel < 0 || channel >= 3 || encoder->width < 0 ||
        encoder->height < 0 || encoder->fps < 0 || encoder->bitrate < 0 ||
        encoder->gop < 0)
        return -1;
    backend->state.encoder[channel] = *encoder;
    return 0;
}

static int mock_set_backlight(struct ngcd_backend *backend, int value)
{
    if (value < 0 || value > 255)
        return -1;
    backend->state.backlight = value;
    if (value > 0)
        backend->state.backlight_saved = value;
    return 0;
}

static int mock_set_audio(struct ngcd_backend *backend, int input,
                          bool automatic, int volume, bool set_volume)
{
    if (input < 0 || input >= 3 || volume < 0 || volume > 100)
        return -1;
    if (set_volume)
        backend->state.audio_volume[input] = volume;
    else {
        backend->state.audio_auto = automatic ? 1 : 0;
        backend->state.audio_input = input;
    }
    return 0;
}

static int mock_read_image(struct ngcd_backend *backend)
{
    (void)backend;
    return 0;
}

static int mock_set_speaker(struct ngcd_backend *backend, int volume)
{
    if (volume < 0 || volume > 140)
        return -1;
    backend->state.speaker_volume = volume;
    return 0;
}

static int mock_storage(struct ngcd_backend *backend, const char *action,
                        const char *argument, int first, int second,
                        int *result)
{
    (void)backend;
    (void)argument;
    if (strcmp(action, "get_stor_infos") == 0 ||
        strcmp(action, "get_stor_info_act") == 0) {
        *result = 0;
        return 0;
    }
    if (strcmp(action, "iotest_stor") == 0 && first > 0 && second > 0 &&
        first <= 4096 && (uint64_t)(unsigned)first *
                         (uint64_t)(unsigned)second <= 64U * 1024U) {
        *result = 85000;
        return 0;
    }
    return -1;
}

static int mock_storage_status(struct ngcd_backend *backend,
                               struct ngcd_storage_info *info)
{
    (void)backend;
    memset(info, 0, sizeof(*info));
    memcpy(info->location, "/mnt/mmcblk1p1", sizeof("/mnt/mmcblk1p1"));
    info->total_bytes = UINT64_C(256) * 1024U * 1024U * 1024U;
    info->free_bytes = UINT64_C(128) * 1024U * 1024U * 1024U;
    return 0;
}

static int mock_wifi_status(struct ngcd_backend *backend,
                            struct ngcd_wifi_info *info)
{
    (void)backend;
    memset(info, 0, sizeof(*info));
    memcpy(info->ip_address, "192.168.1.2", sizeof("192.168.1.2"));
    memcpy(info->mac_address, "02:00:00:00:00:01",
           sizeof("02:00:00:00:00:01"));
    memcpy(info->ssid, "CALF-MOCK", sizeof("CALF-MOCK"));
    info->quality = 4;
    info->level = -55;
    return 0;
}

static int mock_wifi_scan(struct ngcd_backend *backend,
                          struct ngcd_wifi_network *networks, size_t capacity,
                          size_t *count)
{
    (void)backend;
    if (capacity < 2)
        return -1;
    memset(networks, 0, sizeof(*networks) * 2U);
    memcpy(networks[0].ssid, "CALF-MOCK", sizeof("CALF-MOCK"));
    networks[0].quality = 4;
    networks[0].level = -55;
    memcpy(networks[1].ssid, "Lab \"Guest\"", sizeof("Lab \"Guest\""));
    networks[1].quality = 2;
    networks[1].level = -78;
    *count = 2;
    return 0;
}

static int mock_power_status(struct ngcd_backend *backend,
                             struct ngcd_power_info *info)
{
    (void)backend;
    info->battery_percent = 75;
    info->usb_supply = 1;
    info->system_temperature = 42;
    info->core_temperature = 48;
    return 0;
}

static int mock_system_action(struct ngcd_backend *backend, const char *action)
{
    (void)backend;
    return strcmp(action, "sysinfo") == 0 || strcmp(action, "poweroff") == 0
               ? 0 : -1;
}

static const struct ngcd_backend_ops MOCK_OPS = {
    .start = mock_start,
    .stop = mock_stop,
    .tick = mock_tick,
    .graphics_control_id = mock_graphics_control_id,
    .histogram = mock_histogram,
    .lcd_screenshot = mock_lcd_screenshot,
    .camera_mode = mock_camera_mode,
    .set_image = mock_set_image,
    .night_preview = mock_night_preview,
    .read_image = mock_read_image,
    .snapshot = mock_snapshot,
    .recording = mock_recording,
    .playback = mock_playback,
    .stream = mock_stream,
    .uvc = mock_uvc,
    .read_imu = mock_read_imu,
    .calibrate_imu = mock_calibrate_imu,
    .imu_calibration_state = mock_imu_calibration_state,
    .set_encoder = mock_set_encoder,
    .set_backlight = mock_set_backlight,
    .set_audio = mock_set_audio,
    .set_speaker = mock_set_speaker,
    .storage = mock_storage,
    .storage_status = mock_storage_status,
    .wifi_status = mock_wifi_status,
    .wifi_scan = mock_wifi_scan,
    .power_status = mock_power_status,
    .system_action = mock_system_action,
};

const struct ngcd_backend_ops *ngcd_mock_backend_ops(void)
{
    return &MOCK_OPS;
}
