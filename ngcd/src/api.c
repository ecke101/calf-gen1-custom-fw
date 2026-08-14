#include "ngcd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENCODER_CODEC 0x01U
#define ENCODER_RATE_CONTROL 0x02U
#define ENCODER_PROFILE 0x04U
#define ENCODER_WIDTH 0x08U
#define ENCODER_HEIGHT 0x10U
#define ENCODER_FPS 0x20U
#define ENCODER_BITRATE 0x40U
#define ENCODER_GOP 0x80U
#define ENCODER_COLOR_RANGE 0x100U

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0)
        return 0U;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static uint64_t recording_duration_seconds(
    const struct ngcd_runtime_state *state)
{
    uint64_t now;
    if (!state->recording || state->recording_started_ns == 0U)
        return 0U;
    now = monotonic_nanoseconds();
    return now >= state->recording_started_ns
               ? (now - state->recording_started_ns) / UINT64_C(1000000000)
               : 0U;
}

static int response_format(struct ngcd_response *response, int status,
                           const char *format, ...)
{
    va_list arguments;
    int count;
    response->status = status;
    memcpy(response->content_type, "application/json",
           sizeof("application/json"));
    va_start(arguments, format);
    count = vsnprintf(response->body, sizeof(response->body), format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= sizeof(response->body)) {
        response->status = 500;
        memcpy(response->body, "{\"code\":-1,\"message\":\"response too large\"}",
               sizeof("{\"code\":-1,\"message\":\"response too large\"}"));
        response->body_length = strlen(response->body);
        return -1;
    }
    response->body_length = (size_t)count;
    return 0;
}

static int response_append(struct ngcd_response *response,
                           const char *format, ...)
{
    va_list arguments;
    int count;
    if (response->body_length >= sizeof(response->body))
        return -1;
    va_start(arguments, format);
    count = vsnprintf(response->body + response->body_length,
                      sizeof(response->body) - response->body_length,
                      format, arguments);
    va_end(arguments);
    if (count < 0 ||
        (size_t)count >= sizeof(response->body) - response->body_length)
        return -1;
    response->body_length += (size_t)count;
    return 0;
}

static int error_response(struct ngcd_response *response, int status,
                          const char *message)
{
    char escaped[512];
    if (ngcd_json_escape(escaped, sizeof(escaped), message) < 0)
        memcpy(escaped, "request failed", sizeof("request failed"));
    return response_format(response, status,
                           "{\"code\":-1,\"message\":\"%s\"}", escaped);
}

static int success(struct ngcd_response *response)
{
    return response_format(response, 200, "{\"code\":0}");
}

static int exposure_histogram(struct ngcd_app *app,
                              struct ngcd_response *response)
{
    uint32_t bins[NGCD_HISTOGRAM_BINS];
    size_t index;
    if (app->backend.ops->histogram == NULL ||
        app->backend.ops->histogram(&app->backend, bins) != 0)
        return error_response(response, 200, "histogram is unavailable");
    if (response_format(response, 200,
                        "{\"code\":0,\"body\":{\"hist\":[") != 0)
        return -1;
    for (index = 0U; index < NGCD_HISTOGRAM_BINS; ++index)
        if (response_append(response, "%s%u", index == 0U ? "" : ",",
                            bins[index]) != 0)
            return error_response(response, 500,
                                  "histogram response is too large");
    if (response_append(response, "]}}") != 0)
        return error_response(response, 500,
                              "histogram response is too large");
    return 0;
}

static int lcd_screenshot(struct ngcd_app *app,
                          struct ngcd_response *response)
{
    const unsigned char *data = NULL;
    size_t size = 0U;
    if (app->backend.ops->lcd_screenshot == NULL ||
        app->backend.ops->lcd_screenshot(&app->backend, &data, &size) != 0 ||
        data == NULL || size < 54U || size > 2U * 1024U * 1024U)
        return error_response(response, 200, "LCD screenshot is unavailable");
    response->status = 200;
    memcpy(response->content_type, "image/bmp", sizeof("image/bmp"));
    response->body_data = data;
    response->body_length = size;
    return 0;
}

static int require_method(const struct ngcd_request *request,
                          struct ngcd_response *response,
                          enum ngcd_method method)
{
    if (request->method == method)
        return 0;
    error_response(response, 405, "method not allowed");
    return -1;
}

static int body_string(const struct ngcd_request *request, const char *key,
                       char *output, size_t size, bool required)
{
    int result;
    if (request->body == NULL || request->body_length == 0)
        return required ? -1 : 0;
    result = ngcd_json_get_string(request->body, request->body_length,
                                  key, output, size);
    if (result < 0 || (required && result == 0))
        return -1;
    return result;
}

static int body_int(const struct ngcd_request *request, const char *key,
                    int64_t *value, bool required)
{
    int result;
    if (request->body == NULL || request->body_length == 0)
        return required ? -1 : 0;
    result = ngcd_json_get_int64(request->body, request->body_length,
                                 key, value);
    if (result < 0 || (required && result == 0))
        return -1;
    return result;
}

static int query_int(const char *query, const char *key, int *result)
{
    const char *cursor = query;
    size_t key_length = strlen(key);
    while (*cursor != '\0') {
        char *end;
        long value;
        if (strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
            cursor += key_length + 1;
            value = strtol(cursor, &end, 10);
            if (end == cursor || (*end != '\0' && *end != '&') ||
                value < 0 || value > 1000000)
                return -1;
            *result = (int)value;
            return 1;
        }
        cursor = strchr(cursor, '&');
        if (cursor == NULL)
            break;
        ++cursor;
    }
    return 0;
}

static int get_product_info(struct ngcd_app *app,
                            struct ngcd_response *response)
{
    char fields[7][256];
    const char *values[] = {app->manufacturer, app->brand, app->product,
                            app->version, app->build_time, app->hardware,
                            app->serial_number};
    size_t index;
    for (index = 0; index < 7; ++index)
        if (ngcd_json_escape(fields[index], sizeof(fields[index]), values[index]) < 0)
            return error_response(response, 500, "invalid product information");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"manufacturer\":\"%s\",\"brand\":\"%s\","
        "\"product\":\"%s\",\"version\":\"%s\",\"buildtime\":\"%s\","
        "\"hardware\":\"%s\",\"serialnumber\":\"%s\"}}",
        fields[0], fields[1], fields[2], fields[3], fields[4], fields[5],
        fields[6]);
}

static int get_image(struct ngcd_app *app, struct ngcd_response *response)
{
    struct ngcd_image_state *image = &app->backend.state.image;
    char exp[128], iso[128], wb[128], ev[128], anti[128], effect[128];
    if (app->backend.ops->read_image(&app->backend) != 0)
        return error_response(response, 200, "read image parameters failed");
    if (ngcd_json_escape(exp, sizeof(exp), image->exposure) < 0 ||
        ngcd_json_escape(iso, sizeof(iso), image->iso) < 0 ||
        ngcd_json_escape(wb, sizeof(wb), image->white_balance) < 0 ||
        ngcd_json_escape(ev, sizeof(ev), image->exposure_compensation) < 0 ||
        ngcd_json_escape(anti, sizeof(anti), image->anti_flicker) < 0 ||
        ngcd_json_escape(effect, sizeof(effect), image->effect) < 0)
        return error_response(response, 500, "invalid image state");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"exp\":\"%s\",\"iso\":\"%s\","
        "\"wb\":\"%s\",\"ev\":\"%s\",\"brightness\":\"%d\","
        "\"contrast\":\"%d\",\"saturation\":\"%d\",\"hue\":\"%d\","
        "\"sharpness\":\"%d\",\"3dnr\":\"%d\","
        "\"antiflicker\":\"%s\",\"imgeffect\":\"%s\"}}",
        exp, iso, wb, ev, image->brightness, image->contrast,
        image->saturation, image->hue, image->sharpness,
        image->noise_reduction, anti, effect);
}

static int set_image(struct ngcd_app *app, const struct ngcd_request *request,
                     struct ngcd_response *response)
{
    char type[NGCD_VALUE_MAX];
    char value[NGCD_VALUE_MAX];
    bool fixed = false;
    int fixed_result;
    int result;
    if (body_string(request, "type", type, sizeof(type), true) < 0 ||
        body_string(request, "value", value, sizeof(value), true) < 0)
        return error_response(response, 400, "invalid image parameters");
    fixed_result = ngcd_json_get_bool(request->body, request->body_length,
                                      "fixed", &fixed);
    if (fixed_result < 0)
        return error_response(response, 400, "invalid fixed value");
    result = app->backend.ops->set_image(&app->backend, type, value, fixed);
    if (result != 0) {
        char message[64];
        int count = snprintf(message, sizeof(message),
                             "set image parameters failed (%d)", result);
        if (count <= 0 || (size_t)count >= sizeof(message))
            return error_response(response, 200,
                                  "set image parameters failed");
        return error_response(response, 200, message);
    }
    return success(response);
}

static int set_night_preview(struct ngcd_app *app,
                             const struct ngcd_request *request,
                             struct ngcd_response *response)
{
    char exposure[NGCD_VALUE_MAX];
    char iso[NGCD_VALUE_MAX];
    int64_t fps;
    int result;
    if (body_int(request, "fps", &fps, true) < 0 ||
        body_string(request, "exposure", exposure, sizeof(exposure),
                    true) < 0 ||
        body_string(request, "iso", iso, sizeof(iso), true) < 0 ||
        (fps != 4 && fps != 8 && fps != 15 && fps != 30))
        return error_response(response, 400,
                              "invalid Night preview parameters");
    if (app->backend.ops->night_preview == NULL)
        return error_response(response, 501,
                              "Night preview is not implemented");
    result = app->backend.ops->night_preview(
        &app->backend, (int)fps, exposure, iso);
    if (result != 0) {
        char message[80];
        int count = snprintf(message, sizeof(message),
                             "Night preview transaction failed (%d)",
                             result);
        if (count <= 0 || (size_t)count >= sizeof(message))
            return error_response(response, 200,
                                  "Night preview transaction failed");
        return error_response(response, 200, message);
    }
    return success(response);
}

static int get_recording(struct ngcd_app *app,
                         struct ngcd_response *response)
{
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"running\":%d,\"duration\":%llu}}",
        app->backend.state.recording ? 1 : 0,
        (unsigned long long)recording_duration_seconds(
            &app->backend.state));
}

static int set_recording(struct ngcd_app *app,
                         const struct ngcd_request *request,
                         struct ngcd_response *response)
{
    char action[64];
    int64_t split = 2, size = 0, time = 0;
    bool was_recording = app->backend.state.recording;
    if (body_string(request, "action", action, sizeof(action), true) < 0 ||
        body_int(request, "file_split_type", &split, false) < 0 ||
        body_int(request, "size_limit", &size, false) < 0 ||
        body_int(request, "time_limit", &time, false) < 0 ||
        split < 0 || split > 2 || size < 0 || time < 0)
        return error_response(response, 400, "invalid recording request");
    if (app->backend.ops->recording(&app->backend, action, (int)split,
                                    (uint64_t)size, (uint64_t)time) != 0)
        return error_response(response, 200, "recording command failed");
    if (!was_recording && app->backend.state.recording)
        app->backend.state.recording_started_ns = monotonic_nanoseconds();
    else if (!app->backend.state.recording)
        app->backend.state.recording_started_ns = 0U;
    return success(response);
}

static int camera_mode(struct ngcd_app *app,
                       const struct ngcd_request *request,
                       struct ngcd_response *response)
{
    char action[32];
    char mode[NGCD_VALUE_MAX];
    bool start;
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid camera mode request");
    start = strcmp(action, "start") == 0;
    if (!start && strcmp(action, "stop") != 0)
        return error_response(response, 400, "invalid camera mode action");
    mode[0] = '\0';
    if (start && body_string(request, "mode", mode, sizeof(mode), true) < 0)
        return error_response(response, 400, "camera mode is required");
    if (app->backend.ops->camera_mode(&app->backend, mode, start) != 0)
        return error_response(response, 200, "camera mode command failed");
    return success(response);
}

static int snapshot(struct ngcd_app *app, struct ngcd_response *response)
{
    char filename[NGCD_PATH_MAX];
    char escaped[NGCD_PATH_MAX * 2U];
    if (app->backend.ops->snapshot(&app->backend, filename,
                                   sizeof(filename)) != 0)
        return error_response(response, 200, "snapshot failed");
    if (ngcd_json_escape(escaped, sizeof(escaped), filename) < 0)
        return error_response(response, 500, "invalid snapshot filename");
    return response_format(response, 200,
                           "{\"code\":0,\"body\":{\"filename\":\"%s\"}}",
                           escaped);
}

static int get_playback(struct ngcd_app *app,
                        struct ngcd_response *response)
{
    struct ngcd_runtime_state *state = &app->backend.state;
    if (!state->playback)
        return response_format(response, 200,
                               "{\"code\":0,\"body\":{\"running\":0}}");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"running\":1,\"file_size\":%llu,"
        "\"create_time\":%llu,\"is_picture\":%d,\"vcodec\":\"%s\","
        "\"width\":%d,\"height\":%d,\"duration\":%llu.%03llu,"
        "\"sample_index\":%d,\"sample_count\":%d,\"is_pause\":%d,"
        "\"decoder_received\":%u,\"decoder_decoded\":%u,"
        "\"decoder_pending_stream\":%u,"
        "\"decoder_pending_pictures\":%u,\"decoder_errors\":%u,"
        "\"presented_frames\":%u,\"output_errors\":%u}}",
        (unsigned long long)state->playback_file_size,
        (unsigned long long)state->playback_create_time,
        state->playback_picture ? 1 : 0, state->playback_codec,
        state->playback_width, state->playback_height,
        (unsigned long long)(state->playback_duration_us / UINT64_C(1000000)),
        (unsigned long long)((state->playback_duration_us / 1000U) % 1000U),
        state->playback_sample_index, state->playback_sample_count,
        state->playback_paused ? 1 : 0,
        state->playback_decoder_received,
        state->playback_decoder_decoded,
        state->playback_decoder_pending_stream,
        state->playback_decoder_pending_pictures,
        state->playback_decoder_errors,
        state->playback_presented_frames,
        state->playback_output_errors);
}

static int set_playback(struct ngcd_app *app,
                        const struct ngcd_request *request,
                        struct ngcd_response *response)
{
    char action[32], path[NGCD_PATH_MAX];
    int64_t index = 0, numerator = 0, denominator = 0;
    int has_index;
    int has_numerator;
    int has_denominator;
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid playback request");
    path[0] = '\0';
    has_index = body_int(request, "index", &index, false);
    has_numerator = body_int(request, "qnum", &numerator, false);
    has_denominator = body_int(request, "qden", &denominator, false);
    if (body_string(request, "filepath", path, sizeof(path), false) < 0 ||
        has_index < 0 || has_numerator < 0 || has_denominator < 0 ||
        index < 0 || index > 0x7fffffffLL || numerator < 0 ||
        numerator > 0x7fffffffLL || denominator < 0 ||
        denominator > 0x7fffffffLL ||
        (strcmp(action, "seek") == 0 && !has_index &&
         (!has_numerator || !has_denominator || denominator == 0)))
        return error_response(response, 400, "invalid playback parameters");
    if (app->backend.ops->playback(&app->backend, action,
                                   path[0] != '\0' ? path : NULL,
                                   has_index ? (int)index : (int)numerator,
                                   has_index ? 0 : (int)denominator) != 0)
        return error_response(response, 200, "playback command failed");
    return success(response);
}

static bool *stream_state(struct ngcd_runtime_state *state, const char *name)
{
    if (strcmp(name, "live") == 0)
        return &state->live;
    if (strcmp(name, "rtmp") == 0)
        return &state->rtmp;
    if (strcmp(name, "rtsp") == 0)
        return &state->rtsp;
    if (strcmp(name, "srt") == 0)
        return &state->srt;
    if (strcmp(name, "openstream") == 0)
        return &state->open_stream;
    return NULL;
}

static int get_stream(struct ngcd_app *app, struct ngcd_response *response,
                      const char *name)
{
    bool *running = stream_state(&app->backend.state, name);
    if (running == NULL)
        return error_response(response, 404, "unknown stream service");
    if (strcmp(name, "openstream") == 0)
        return response_format(response, 200,
                               "{\"code\":0,\"body\":{\"duration\":%d}}",
                               *running ? 0 : -1);
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"duration\":%d,\"video_bps\":%d}}",
        *running ? 1 : 0, *running ? 1 : 0);
}

static int set_stream(struct ngcd_app *app,
                      const struct ngcd_request *request,
                      struct ngcd_response *response, const char *name)
{
    char action[64], url[NGCD_URL_MAX];
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid stream request");
    url[0] = '\0';
    if (body_string(request, "url", url, sizeof(url), false) < 0)
        return error_response(response, 400, "invalid stream URL");
    if (app->backend.ops->stream(&app->backend, name, action,
                                 url[0] != '\0' ? url : NULL) != 0)
        return error_response(response, 200, "stream command failed");
    if (strcmp(action, "get_conn_infos") == 0)
        return response_format(response, 200,
                               "{\"code\":0,\"body\":{\"conn_infos\":[]}}");
    return success(response);
}

static int get_imu(struct ngcd_app *app, struct ngcd_response *response)
{
    struct ngcd_imu_sample sample;
    if (app->backend.ops->read_imu(&app->backend, &sample) != 0)
        return error_response(response, 200, "failed to get imu sample");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"gyro_x\":%d,\"gyro_y\":%d,"
        "\"gyro_z\":%d,\"acc_x\":%d,\"acc_y\":%d,\"acc_z\":%d,"
        "\"monotonic_ns\":%llu}}",
        sample.gyro_x, sample.gyro_y, sample.gyro_z,
        sample.acceleration_x, sample.acceleration_y, sample.acceleration_z,
        (unsigned long long)sample.monotonic_ns);
}

static int set_imu(struct ngcd_app *app, const struct ngcd_request *request,
                   struct ngcd_response *response)
{
    char action[32];
    int64_t type = 0;
    int64_t count = 20;
    bool save = false;
    int parsed;
    if (body_string(request, "action", action, sizeof(action), true) < 0 ||
        strcmp(action, "start_calib") != 0 ||
        body_int(request, "type", &type, false) < 0 ||
        body_int(request, "count", &count, false) < 0 || type != 0 ||
        count < 5 || count > 10000)
        return error_response(response, 400, "invalid IMU request");
    parsed = request->body != NULL
                 ? ngcd_json_get_bool(request->body, request->body_length,
                                      "save", &save)
                 : 0;
    if (parsed < 0) {
        int64_t numeric_save = 0;
        parsed = ngcd_json_get_int64(request->body, request->body_length,
                                     "save", &numeric_save);
        if (parsed < 0 || numeric_save < 0 || numeric_save > 1)
            return error_response(response, 400, "invalid IMU save value");
        save = numeric_save != 0;
    }
    if (app->backend.ops->calibrate_imu(&app->backend, (int)type, save,
                                        (int)count) != 0)
        return error_response(response, 200, "failed to start IMU calibration");
    return success(response);
}

static int get_imu_calibration_state(struct ngcd_app *app,
                                     struct ngcd_response *response)
{
    int state;
    if (app->backend.ops->imu_calibration_state(&app->backend, &state) != 0)
        return error_response(response, 200,
                              "failed to get IMU calibration state");
    return response_format(response, 200,
                           "{\"code\":0,\"body\":{\"calib_state\":%d}}",
                           state);
}

static int get_encoder(struct ngcd_app *app,
                       const struct ngcd_request *request,
                       struct ngcd_response *response)
{
    int channel;
    struct ngcd_encoder_state *encoder;
    if (query_int(request->query, "channel", &channel) != 1 || channel >= 3)
        return error_response(response, 400, "invalid encoder channel");
    encoder = &app->backend.state.encoder[channel];
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"channel\":%d,\"vcodec\":\"%s\","
        "\"rcmode\":\"%s\",\"profile\":\"%s\",\"width\":%d,"
        "\"height\":%d,\"fps\":%d,\"bitrate\":%d,\"gop\":%d,"
        "\"color_range\":%d}}",
        channel, encoder->codec, encoder->rate_control, encoder->profile,
        encoder->width, encoder->height, encoder->fps, encoder->bitrate,
        encoder->gop, encoder->color_range);
}

static int copy_optional_string(const struct ngcd_request *request,
                                const char *key, char *destination, size_t size,
                                uint32_t field, uint32_t *changed)
{
    char value[64];
    int result = body_string(request, key, value, sizeof(value), false);
    if (result <= 0)
        return result;
    if (strlen(value) >= size)
        return -1;
    memcpy(destination, value, strlen(value) + 1);
    *changed |= field;
    return 1;
}

static int copy_optional_int(const struct ngcd_request *request,
                             const char *key, int *destination,
                             uint32_t field, uint32_t *changed)
{
    int64_t value;
    int result = body_int(request, key, &value, false);
    if (result <= 0)
        return result;
    if (value < 0 || value > 1000000)
        return -1;
    *destination = (int)value;
    *changed |= field;
    return 1;
}

static int set_encoder(struct ngcd_app *app,
                       const struct ngcd_request *request,
                       struct ngcd_response *response)
{
    int64_t channel_value;
    int channel;
    uint32_t changed = 0;
    struct ngcd_encoder_state encoder;
    if (body_int(request, "channel", &channel_value, true) < 0 ||
        channel_value < 0 || channel_value >= 3)
        return error_response(response, 400, "invalid encoder channel");
    channel = (int)channel_value;
    encoder = app->backend.state.encoder[channel];
    if (copy_optional_string(request, "vcodec", encoder.codec,
                             sizeof(encoder.codec), ENCODER_CODEC, &changed) < 0 ||
        copy_optional_string(request, "rcmode", encoder.rate_control,
                             sizeof(encoder.rate_control), ENCODER_RATE_CONTROL,
                             &changed) < 0 ||
        copy_optional_string(request, "profile", encoder.profile,
                             sizeof(encoder.profile), ENCODER_PROFILE, &changed) < 0 ||
        copy_optional_int(request, "width", &encoder.width,
                          ENCODER_WIDTH, &changed) < 0 ||
        copy_optional_int(request, "height", &encoder.height,
                          ENCODER_HEIGHT, &changed) < 0 ||
        copy_optional_int(request, "fps", &encoder.fps,
                          ENCODER_FPS, &changed) < 0 ||
        copy_optional_int(request, "bitrate", &encoder.bitrate,
                          ENCODER_BITRATE, &changed) < 0 ||
        copy_optional_int(request, "gop", &encoder.gop,
                          ENCODER_GOP, &changed) < 0 ||
        copy_optional_int(request, "color_range", &encoder.color_range,
                          ENCODER_COLOR_RANGE, &changed) < 0 || changed == 0)
        return error_response(response, 400, "invalid encoder parameters");
    if (app->backend.ops->set_encoder(&app->backend, channel, &encoder,
                                      changed) != 0)
        return error_response(response, 200, "set venc config failed");
    return success(response);
}

static int uvc(struct ngcd_app *app, const struct ngcd_request *request,
               struct ngcd_response *response)
{
    char action[32];
    if (request->method == NGCD_METHOD_GET)
        return response_format(
            response, 200,
            "{\"code\":0,\"body\":{\"status\":\"%s\"}}",
            app->backend.state.uvc ? "enabled" : "disabled");
    if (body_string(request, "action", action, sizeof(action), true) < 0 ||
        (strcmp(action, "enable") != 0 && strcmp(action, "disable") != 0))
        return error_response(response, 400, "invalid uvc request");
    if (app->backend.ops->uvc(&app->backend,
                              strcmp(action, "enable") == 0) != 0)
        return error_response(response, 200, "uvc command failed");
    return success(response);
}

static int audio_info(struct ngcd_app *app, struct ngcd_response *response)
{
    struct ngcd_runtime_state *state = &app->backend.state;
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"autoinput\":%d,\"inputtype\":%d,"
        "\"inputvol0\":%d,\"inputvol1\":%d,\"inputvol2\":%d}}",
        state->audio_auto, state->audio_input, state->audio_volume[0],
        state->audio_volume[1], state->audio_volume[2]);
}

static int audio_control(struct ngcd_app *app,
                         const struct ngcd_request *request,
                         struct ngcd_response *response)
{
    struct ngcd_runtime_state *state = &app->backend.state;
    char action[32];
    int64_t input = 0, automatic = 0, volume = 0;
    bool set_volume;
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid audio request");
    set_volume = strcmp(action, "volume") == 0;
    if (!set_volume && strcmp(action, "input") != 0)
        return error_response(response, 400, "invalid audio action");
    if (body_int(request, "input", &input, !set_volume ? false : true) < 0 ||
        body_int(request, "auto", &automatic, false) < 0 ||
        body_int(request, "value", &volume, set_volume) < 0 ||
        input < 0 || input >= 3 || automatic < 0 || automatic > 1 ||
        volume < 0 || volume > 100)
        return error_response(response, 400, "invalid audio parameters");
    /* A graph/profile transition reapplies the UI's saved audio state.  A
     * backend which does not own the audio hardware can still truthfully
     * accept an idempotent request, while real changes remain unsupported. */
    if ((set_volume && state->audio_volume[input] == (int)volume) ||
        (!set_volume && state->audio_auto == (automatic != 0) &&
         (automatic != 0 || state->audio_input == (int)input)))
        return success(response);
    if (app->backend.ops->set_audio(&app->backend, (int)input,
                                    automatic != 0, (int)volume,
                                    set_volume) != 0)
        return error_response(response, 200, "audio command failed");
    return success(response);
}

static int speaker(struct ngcd_app *app, const struct ngcd_request *request,
                   struct ngcd_response *response)
{
    char action[32];
    int64_t volume;
    if (body_string(request, "action", action, sizeof(action), true) < 0 ||
        strcmp(action, "volume") != 0 ||
        body_int(request, "value", &volume, true) < 0 ||
        volume < 0 || volume > 140)
        return error_response(response, 400, "invalid speaker request");
    if (app->backend.ops->set_speaker(&app->backend, (int)volume) != 0)
        return error_response(response, 200, "speaker command failed");
    return success(response);
}

static int backlight(struct ngcd_app *app,
                     const struct ngcd_request *request,
                     struct ngcd_response *response)
{
    char action[32];
    int64_t brightness = 0;
    int value;
    if (request->method == NGCD_METHOD_GET)
        return response_format(response, 200,
                               "{\"code\":0,\"body\":{\"brightness\":%d}}",
                               app->backend.state.backlight);
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid backlight request");
    if (strcmp(action, "set_brightness") == 0) {
        if (body_int(request, "brightness", &brightness, true) < 0 ||
            brightness < 0 || brightness > 255)
            return error_response(response, 400, "invalid brightness");
        value = (int)brightness;
    } else if (strcmp(action, "turn_on") == 0) {
        value = app->backend.state.backlight_saved > 0
                    ? app->backend.state.backlight_saved : 151;
    } else if (strcmp(action, "turn_off") == 0) {
        value = 0;
    } else {
        return error_response(response, 400, "invalid backlight action");
    }
    if (app->backend.ops->set_backlight(&app->backend, value) != 0)
        return error_response(response, 200, "backlight command failed");
    return success(response);
}

static int system_status(struct ngcd_app *app,
                         const struct ngcd_request *request,
                         struct ngcd_response *response)
{
    int64_t mask;
    struct ngcd_runtime_state *state = &app->backend.state;
    struct ngcd_wifi_info wifi;
    struct ngcd_power_info power = {-1, 0, 0, 0};
    struct ngcd_imu_sample motion;
    struct ngcd_storage_info storage;
    char ip_address[128], mac_address[64], ssid[NGCD_WIFI_SSID_MAX * 2U];
    char storage_location[NGCD_PATH_MAX];
    uint64_t storage_total_mb = 0;
    uint64_t storage_free_mb = 0;
    if (body_int(request, "ssids", &mask, true) < 0 || mask < 0)
        return error_response(response, 400, "invalid system status mask");
    memset(&wifi, 0, sizeof(wifi));
    if (app->backend.ops->wifi_status(&app->backend, &wifi) != 0)
        memset(&wifi, 0, sizeof(wifi));
    (void)app->backend.ops->power_status(&app->backend, &power);
    memset(&storage, 0, sizeof(storage));
    if (app->backend.ops->storage_status(&app->backend, &storage) == 0) {
        storage_total_mb = storage.total_bytes / (1024U * 1024U);
        storage_free_mb = storage.free_bytes / (1024U * 1024U);
    }
    memset(&motion, 0, sizeof(motion));
    (void)app->backend.ops->read_imu(&app->backend, &motion);
    if (ngcd_json_escape(ip_address, sizeof(ip_address), wifi.ip_address) < 0 ||
        ngcd_json_escape(mac_address, sizeof(mac_address), wifi.mac_address) < 0 ||
        ngcd_json_escape(ssid, sizeof(ssid), wifi.ssid) < 0 ||
        ngcd_json_escape(storage_location, sizeof(storage_location),
                         storage.location) < 0)
        return error_response(response, 500, "invalid system status");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"ws\":{\"ipaddr\":\"%s\","
        "\"mac\":\"%s\",\"essid\":\"%s\",\"qual\":%d,\"level\":%d},"
        "\"rs\":{\"is_running\":%d,\"duration\":%llu},"
        "\"msi\":{\"stor_loc\":\"%s\",\"total_mb\":%llu,"
        "\"free_mb\":%llu},"
        "\"is_usb_supply\":%d,\"batt_cap\":%d,\"ain_src\":%d,"
        "\"sys_temp\":%d,\"core_temp\":%d,"
        "\"ls\":{\"duration\":%d,\"video_bps\":%d},"
        "\"eth0\":{\"ipaddr\":\"0.0.0.0\"},\"gyro_x\":%d,\"gyro_y\":%d}}",
        ip_address, mac_address, ssid, wifi.quality, wifi.level,
        state->recording ? 1 : 0,
        (unsigned long long)recording_duration_seconds(state),
        storage_location,
        (unsigned long long)storage_total_mb,
        (unsigned long long)storage_free_mb,
        power.usb_supply, power.battery_percent,
        state->audio_input, power.system_temperature, power.core_temperature,
        state->live ? 1 : 0, state->live ? 1 : 0,
        motion.gyro_x, motion.gyro_y);
}

static int wifi_status(struct ngcd_app *app, struct ngcd_response *response)
{
    struct ngcd_wifi_info info;
    char ip_address[128], mac_address[64], ssid[NGCD_WIFI_SSID_MAX * 2U];
    if (app->backend.ops->wifi_status(&app->backend, &info) != 0)
        return error_response(response, 200, "failed to get wifi status");
    if (ngcd_json_escape(ip_address, sizeof(ip_address), info.ip_address) < 0 ||
        ngcd_json_escape(mac_address, sizeof(mac_address), info.mac_address) < 0 ||
        ngcd_json_escape(ssid, sizeof(ssid), info.ssid) < 0)
        return error_response(response, 500, "invalid wifi status");
    return response_format(
        response, 200,
        "{\"code\":0,\"body\":{\"ipaddr\":\"%s\",\"mac\":\"%s\","
        "\"essid\":\"%s\",\"qual\":%d,\"level\":%d}}",
        ip_address, mac_address, ssid, info.quality, info.level);
}

static int wifi_scan(struct ngcd_app *app, struct ngcd_response *response)
{
    struct ngcd_wifi_network networks[NGCD_WIFI_NETWORKS_MAX];
    size_t count;
    size_t index;
    if (app->backend.ops->wifi_scan(&app->backend, networks,
                                    NGCD_WIFI_NETWORKS_MAX, &count) != 0 ||
        count > NGCD_WIFI_NETWORKS_MAX)
        return error_response(response, 200, "failed to scan wifi networks");
    if (response_format(response, 200,
                        "{\"code\":0,\"body\":{\"list\":[") != 0)
        return -1;
    for (index = 0; index < count; ++index) {
        char ssid[NGCD_WIFI_SSID_MAX * 2U];
        if (ngcd_json_escape(ssid, sizeof(ssid), networks[index].ssid) < 0 ||
            response_append(response,
                            "%s{\"essid\":\"%s\",\"qual\":%d,\"level\":%d}",
                            index == 0 ? "" : ",", ssid,
                            networks[index].quality,
                            networks[index].level) != 0)
            return error_response(response, 500, "wifi scan response too large");
    }
    if (response_append(response, "]}}") != 0)
        return error_response(response, 500, "wifi scan response too large");
    return 0;
}

static int media_storage(struct ngcd_app *app,
                         const struct ngcd_request *request,
                         struct ngcd_response *response)
{
    char action[64], argument[NGCD_PATH_MAX];
    int64_t first = 0, second = 0;
    int result = 0;
    struct ngcd_storage_info info;
    char location[NGCD_PATH_MAX];
    uint64_t total_mb;
    uint64_t free_mb;
    if (body_string(request, "action", action, sizeof(action), true) < 0)
        return error_response(response, 400, "invalid storage request");
    argument[0] = '\0';
    if (body_string(request, "loc_policy", argument, sizeof(argument), false) < 0 ||
        body_int(request, "block_kb", &first, false) < 0 ||
        body_int(request, "count", &second, false) < 0 ||
        first < 0 || first > 0x7fffffffLL || second < 0 || second > 0x7fffffffLL)
        return error_response(response, 400, "invalid storage parameters");
    if (strcmp(action, "get_stor_infos") == 0 ||
        strcmp(action, "get_stor_info_act") == 0) {
        if (app->backend.ops->storage_status(&app->backend, &info) != 0 ||
            ngcd_json_escape(location, sizeof(location), info.location) < 0)
            return error_response(response, 200, "storage is unavailable");
        total_mb = info.total_bytes / (1024U * 1024U);
        free_mb = info.free_bytes / (1024U * 1024U);
        if (strcmp(action, "get_stor_infos") == 0)
            return response_format(
                response, 200,
                "{\"code\":0,\"body\":{\"stor_infos\":[{"
                "\"stor_loc\":\"%s\",\"total_mb\":%llu,"
                "\"free_mb\":%llu}]}}",
                location, (unsigned long long)total_mb,
                (unsigned long long)free_mb);
        return response_format(
            response, 200,
            "{\"code\":0,\"body\":{\"stor_loc\":\"%s\","
            "\"total_mb\":%llu,\"free_mb\":%llu}}",
            location, (unsigned long long)total_mb,
            (unsigned long long)free_mb);
    }
    if (app->backend.ops->storage(&app->backend, action, argument,
                                  (int)first, (int)second, &result) != 0)
        return error_response(response, 200, "storage command failed");
    if (strcmp(action, "iotest_stor") == 0)
        return response_format(response, 200,
                               "{\"code\":0,\"body\":{\"kbps\":%d}}", result);
    return success(response);
}

static int media_catalog(struct ngcd_app *app,
                         const struct ngcd_request *request,
                         struct ngcd_response *response)
{
    struct ngcd_storage_info storage;
    struct ngcd_media_entry entries[64];
    size_t count;
    size_t total;
    size_t index;
    int offset = 0;
    int limit = 64;
    int parsed;
    parsed = query_int(request->query, "offset", &offset);
    if (parsed < 0 || offset < 0)
        return error_response(response, 400, "invalid media offset");
    parsed = query_int(request->query, "limit", &limit);
    if (parsed < 0 || limit < 1 || limit > 64)
        return error_response(response, 400, "invalid media limit");
    if (app->backend.ops->storage_status(&app->backend, &storage) != 0 ||
        ngcd_storage_media_list(storage.location, (size_t)offset, entries,
                                (size_t)limit, &count, &total) != 0 ||
        response_format(response, 200,
                        "{\"code\":0,\"body\":{\"offset\":%d,"
                        "\"total\":%lu,\"items\":[",
                        offset, (unsigned long)total) != 0)
        return error_response(response, 200, "media catalog is unavailable");
    for (index = 0U; index < count; ++index) {
        char path[NGCD_PATH_MAX * 2U];
        char name[32];
        if (ngcd_json_escape(path, sizeof(path), entries[index].path) < 0 ||
            ngcd_json_escape(name, sizeof(name), entries[index].name) < 0 ||
            response_append(
                response,
                "%s{\"path\":\"%s\",\"name\":\"%s\","
                "\"video\":%d,\"size\":%llu,\"create_time\":%llu}",
                index == 0U ? "" : ",", path, name,
                entries[index].video ? 1 : 0,
                (unsigned long long)entries[index].size,
                (unsigned long long)entries[index].create_time) != 0)
            return error_response(response, 500,
                                  "media catalog response is too large");
    }
    if (response_append(response, "]}}") != 0)
        return error_response(response, 500,
                              "media catalog response is too large");
    return 0;
}

static int unsupported(struct ngcd_response *response)
{
    return error_response(response, 501, "route is not implemented by this backend");
}

int ngcd_dispatch(struct ngcd_app *app, const struct ngcd_request *request,
                  struct ngcd_response *response)
{
    const char *route = request->path;
    const char prefix[] = "/camera/v2/";
    memset(response, 0, sizeof(*response));

    if (request->method == NGCD_METHOD_OPTIONS)
        return response_format(response, 204, "");
    if (strncmp(route, prefix, sizeof(prefix) - 1) != 0)
        return error_response(response, 404, "route not found");
    route += sizeof(prefix) - 1;

    if (strcmp(route, "productinfo") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return get_product_info(app, response);
    }
    if (strcmp(route, "graphctrlmb") == 0) {
        int identifier;
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        identifier = app->backend.ops->graphics_control_id(&app->backend);
        return identifier >= 0
                   ? response_format(response, 200,
                                     "{\"code\":0,\"body\":{\"ctrlmb\":%d}}",
                                     identifier)
                   : error_response(response, 200,
                                    "graphics control buffer is unavailable");
    }
    if (strcmp(route, "imgparams") == 0) {
        if (request->method == NGCD_METHOD_GET) return get_image(app, response);
        if (request->method == NGCD_METHOD_POST) return set_image(app, request, response);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "nightpreview") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return set_night_preview(app, request, response);
    }
    if (strcmp(route, "recording") == 0) {
        if (request->method == NGCD_METHOD_GET) return get_recording(app, response);
        if (request->method == NGCD_METHOD_POST) return set_recording(app, request, response);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "cameramode") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return camera_mode(app, request, response);
    }
    if (strcmp(route, "snapshot") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return snapshot(app, response);
    }
    if (strcmp(route, "playback") == 0) {
        if (request->method == NGCD_METHOD_GET) return get_playback(app, response);
        if (request->method == NGCD_METHOD_POST) return set_playback(app, request, response);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "media") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return media_catalog(app, request, response);
    }
    if (strcmp(route, "live") == 0 || strcmp(route, "rtmp") == 0 ||
        strcmp(route, "rtsp") == 0 || strcmp(route, "srt") == 0 ||
        strcmp(route, "openstream") == 0) {
        if (request->method == NGCD_METHOD_GET) return get_stream(app, response, route);
        if (request->method == NGCD_METHOD_POST) return set_stream(app, request, response, route);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "uvc") == 0) {
        if (request->method != NGCD_METHOD_GET && request->method != NGCD_METHOD_POST)
            return error_response(response, 405, "method not allowed");
        return uvc(app, request, response);
    }
    if (strcmp(route, "imu_sample") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return get_imu(app, response);
    }
    if (strcmp(route, "imu") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return set_imu(app, request, response);
    }
    if (strcmp(route, "imu_calib_state") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return get_imu_calibration_state(app, response);
    }
    if (strcmp(route, "audioinfo") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return audio_info(app, response);
    }
    if (strcmp(route, "audioctrl") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return audio_control(app, request, response);
    }
    if (strcmp(route, "aplay") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return speaker(app, request, response);
    }
    if (strcmp(route, "backlight") == 0 || strcmp(route, "lcd/backlight") == 0) {
        if (request->method != NGCD_METHOD_GET && request->method != NGCD_METHOD_POST)
            return error_response(response, 405, "method not allowed");
        return backlight(app, request, response);
    }
    if (strcmp(route, "vencattr") == 0) {
        if (request->method == NGCD_METHOD_GET) return get_encoder(app, request, response);
        if (request->method == NGCD_METHOD_POST) return set_encoder(app, request, response);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "systemstatus") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return system_status(app, request, response);
    }
    if (strcmp(route, "wifi") == 0) {
        if (request->method == NGCD_METHOD_GET)
            return wifi_status(app, response);
        if (request->method == NGCD_METHOD_POST)
            return unsupported(response);
        return error_response(response, 405, "method not allowed");
    }
    if (strcmp(route, "scanwifi") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return wifi_scan(app, response);
    }
    if (strcmp(route, "mediastor") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return media_storage(app, request, response);
    }
    if (strcmp(route, "sysinfo") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        return app->backend.ops->system_action(&app->backend, "sysinfo") == 0
                   ? success(response) : error_response(response, 200, "sysinfo failed");
    }
    if (strcmp(route, "poweroff") == 0) {
        if (require_method(request, response, NGCD_METHOD_POST) != 0) return 0;
        if (app->backend.ops->system_action(&app->backend, "poweroff") != 0)
            return error_response(response, 200, "poweroff failed");
        /* Let the server send this response before main tears down the media
         * graph, syncs storage, and asks the kernel to remove power. */
        app->poweroff_requested = true;
        ngcd_request_shutdown();
        return success(response);
    }
    if (strcmp(route, "exphist") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return exposure_histogram(app, response);
    }
    if (strcmp(route, "lcdscreenshot") == 0) {
        if (require_method(request, response, NGCD_METHOD_GET) != 0) return 0;
        return lcd_screenshot(app, response);
    }

    if (strcmp(route, "defcalib") == 0 || strcmp(route, "dumpimage") == 0 ||
        strcmp(route, "dumpyuv") == 0 || strcmp(route, "ethaddr") == 0 ||
        strcmp(route, "hdmictrl") == 0 || strcmp(route, "net") == 0 ||
        strcmp(route, "osd") == 0 || strcmp(route, "pencattr") == 0 ||
        strcmp(route, "romparams") == 0 ||
        strcmp(route, "scanqrcode") == 0 ||
        strcmp(route, "sdcard") == 0 || strcmp(route, "sensoroffset") == 0 ||
        strcmp(route, "serialnumber") == 0 || strcmp(route, "soundcard") == 0 ||
        strcmp(route, "stereooffset") == 0 || strcmp(route, "upgrade") == 0 ||
        strcmp(route, "webservice") == 0)
        return unsupported(response);
    return error_response(response, 404, "route not found");
}
