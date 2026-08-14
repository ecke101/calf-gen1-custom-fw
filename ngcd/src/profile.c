#include "ngcd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum profile_section {
    SECTION_ROOT,
    SECTION_SENSOR,
    SECTION_CAPTURE,
    SECTION_GDC,
    SECTION_OUTPUT,
    SECTION_STITCH,
    SECTION_ENCODER,
};

static void set_error(char *error, size_t size, size_t line,
                      const char *message)
{
    if (error == NULL || size == 0)
        return;
    if (line > 0)
        (void)snprintf(error, size, "line %zu: %s", line, message);
    else
        (void)snprintf(error, size, "%s", message);
}

static char *trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t')
        ++text;
    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = '\0';
    return text;
}

static int split_pair(char *text, char **key, char **value)
{
    char *colon = strchr(text, ':');
    if (colon == NULL)
        return -1;
    *colon = '\0';
    *key = trim(text);
    *value = trim(colon + 1);
    return **key != '\0' ? 0 : -1;
}

static int parse_integer(const char *text, int *result)
{
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < -0x7fffffffL - 1L || value > 0x7fffffffL)
        return -1;
    *result = (int)value;
    return 0;
}

static int parse_text(const char *text, char *output, size_t size)
{
    size_t length = strlen(text);
    if (length >= 2 && text[0] == '"' && text[length - 1] == '"') {
        ++text;
        length -= 2;
    }
    if (length == 0 || length >= size)
        return -1;
    memcpy(output, text, length);
    output[length] = '\0';
    return 0;
}

static int geometry_value(struct ngcd_video_geometry *geometry,
                          const char *key, const char *value)
{
    int *field;
    if (strcmp(key, "mode") == 0)
        field = &geometry->mode;
    else if (strcmp(key, "width") == 0)
        field = &geometry->width;
    else if (strcmp(key, "height") == 0)
        field = &geometry->height;
    else if (strcmp(key, "fps") == 0)
        field = &geometry->fps;
    else
        return 0;
    return parse_integer(value, field) == 0 ? 1 : -1;
}

static int encoder_value(struct ngcd_profile *profile, size_t index,
                         const char *key, const char *value)
{
    struct ngcd_encoder_state *encoder = &profile->encoder[index];
    int *field;
    if (strcmp(key, "vcodec") == 0)
        return parse_text(value, encoder->codec, sizeof(encoder->codec)) == 0
                   ? 1 : -1;
    if (strcmp(key, "rcmode") == 0)
        return parse_text(value, encoder->rate_control,
                          sizeof(encoder->rate_control)) == 0 ? 1 : -1;
    if (strcmp(key, "profile") == 0)
        return parse_text(value, encoder->profile,
                          sizeof(encoder->profile)) == 0 ? 1 : -1;
    if (strcmp(key, "mask") == 0) {
        int mask;
        if (parse_integer(value, &mask) != 0 || (mask != 0 && mask != 1))
            return -1;
        profile->encoder_mask[index] = mask != 0;
        return 1;
    }
    if (strcmp(key, "width") == 0)
        field = &encoder->width;
    else if (strcmp(key, "height") == 0)
        field = &encoder->height;
    else if (strcmp(key, "fps") == 0)
        field = &encoder->fps;
    else if (strcmp(key, "bitrate") == 0)
        field = &encoder->bitrate;
    else if (strcmp(key, "gop") == 0)
        field = &encoder->gop;
    else if (strcmp(key, "color_range") == 0)
        field = &encoder->color_range;
    else
        return 0;
    return parse_integer(value, field) == 0 ? 1 : -1;
}

static int validate_geometry(const struct ngcd_video_geometry *geometry,
                             bool fps_required)
{
    if (geometry->width <= 0 || geometry->width > 16384 ||
        geometry->height <= 0 || geometry->height > 16384)
        return -1;
    if (fps_required && (geometry->fps <= 0 || geometry->fps > 240))
        return -1;
    return 0;
}

static int validate_profile(const struct ngcd_profile *profile)
{
    size_t index;
    bool stitched = strcmp(profile->camera_mode, "SBS_STITCH") == 0 ||
                    strcmp(profile->camera_mode, "SBS_3D") == 0;
    if (profile->camera_mode[0] == '\0' || profile->sensor_count == 0 ||
        profile->capture_count == 0 || profile->encoder_count == 0 ||
        validate_geometry(&profile->sensor[0], true) != 0 ||
        validate_geometry(&profile->capture[0], true) != 0)
        return -1;
    for (index = 0; index < profile->sensor_count; ++index)
        if (validate_geometry(&profile->sensor[index], true) != 0)
            return -1;
    for (index = 0; index < profile->capture_count; ++index)
        if (validate_geometry(&profile->capture[index], true) != 0)
            return -1;
    if (profile->output_enabled &&
        validate_geometry(&profile->output, true) != 0)
        return -1;
    if (stitched &&
        (profile->stitch_mode[0] == '\0' ||
         validate_geometry(&profile->stitch, false) != 0))
        return -1;
    for (index = 0; index < profile->encoder_count; ++index) {
        const struct ngcd_encoder_state *encoder = &profile->encoder[index];
        if (encoder->codec[0] == '\0' || encoder->rate_control[0] == '\0' ||
            encoder->profile[0] == '\0' || encoder->width <= 0 ||
            encoder->width > 16384 || encoder->height <= 0 ||
            encoder->height > 16384 || encoder->fps <= 0 ||
            encoder->fps > 240 || encoder->bitrate <= 0 ||
            encoder->bitrate > 1000000 || encoder->gop <= 0 ||
            encoder->gop > 1000)
            return -1;
    }
    return 0;
}

int ngcd_profile_parse(const char *yaml, size_t length,
                       struct ngcd_profile *profile, char *error,
                       size_t error_size)
{
    enum profile_section section = SECTION_ROOT;
    size_t sensor_index = 0;
    size_t capture_index = 0;
    size_t encoder_index = 0;
    bool sensor_item = false;
    bool capture_item = false;
    bool encoder_item = false;
    size_t offset = 0;
    size_t line_number = 0;

    memset(profile, 0, sizeof(*profile));
    profile->preview = true;
    while (offset < length) {
        char line[1024];
        char *content;
        char *key;
        char *value;
        size_t line_length = 0;
        size_t indentation = 0;
        int handled = 0;
        bool list_item = false;

        ++line_number;
        while (offset + line_length < length &&
               yaml[offset + line_length] != '\n')
            ++line_length;
        if (line_length >= sizeof(line)) {
            set_error(error, error_size, line_number, "line is too long");
            return -1;
        }
        memcpy(line, yaml + offset, line_length);
        line[line_length] = '\0';
        offset += line_length + (offset + line_length < length ? 1U : 0U);
        while (line[indentation] == ' ')
            ++indentation;
        if (line[indentation] == '\t') {
            set_error(error, error_size, line_number, "tabs are not allowed");
            return -1;
        }
        content = trim(line + indentation);
        if (*content == '\0' || *content == '#')
            continue;
        if (content[0] == '-' && content[1] == ' ') {
            list_item = true;
            content = trim(content + 2);
        }
        if (split_pair(content, &key, &value) != 0) {
            set_error(error, error_size, line_number, "expected key and value");
            return -1;
        }
        if (strcmp(key, "config") == 0 && *value == '\0')
            continue;
        if (*value == '\0') {
            if (strcmp(key, "sensor") == 0)
                section = SECTION_SENSOR;
            else if (strcmp(key, "vcap") == 0)
                section = SECTION_CAPTURE;
            else if (strcmp(key, "gdc") == 0)
                section = SECTION_GDC;
            else if (strcmp(key, "vout") == 0)
                section = SECTION_OUTPUT;
            else if (strcmp(key, "stitch") == 0)
                section = SECTION_STITCH;
            else if (strcmp(key, "venc") == 0)
                section = SECTION_ENCODER;
            else {
                set_error(error, error_size, line_number, "unknown section");
                return -1;
            }
            continue;
        }
        if (indentation <= 4 && !list_item &&
            (strcmp(key, "camera-mode") == 0 ||
             strcmp(key, "isp-mode") == 0 || strcmp(key, "preview") == 0)) {
            section = SECTION_ROOT;
            if (strcmp(key, "camera-mode") == 0)
                handled = parse_text(value, profile->camera_mode,
                                     sizeof(profile->camera_mode)) == 0 ? 1 : -1;
            else if (strcmp(key, "isp-mode") == 0)
                handled = parse_integer(value, &profile->isp_mode) == 0 ? 1 : -1;
            else {
                int preview;
                handled = parse_integer(value, &preview) == 0 &&
                          (preview == 0 || preview == 1) ? 1 : -1;
                if (handled > 0)
                    profile->preview = preview != 0;
            }
        } else if (section == SECTION_SENSOR) {
            if (list_item) {
                if (sensor_item)
                    ++sensor_index;
                sensor_item = true;
            }
            if (sensor_index >= 2)
                handled = -1;
            else {
                handled = geometry_value(&profile->sensor[sensor_index], key, value);
                if (handled > 0 && profile->sensor_count < sensor_index + 1)
                    profile->sensor_count = sensor_index + 1;
            }
        } else if (section == SECTION_CAPTURE) {
            if (list_item) {
                if (capture_item)
                    ++capture_index;
                capture_item = true;
            }
            if (capture_index >= 2)
                handled = -1;
            else {
                handled = geometry_value(&profile->capture[capture_index], key, value);
                if (handled > 0 && profile->capture_count < capture_index + 1)
                    profile->capture_count = capture_index + 1;
            }
        } else if (section == SECTION_GDC) {
            if (strcmp(key, "enable") == 0) {
                int enabled = 0;
                handled = parse_integer(value, &enabled) == 0 &&
                          (enabled == 0 || enabled == 1) ? 1 : -1;
                profile->gdc_enabled = enabled != 0;
            } else if (strcmp(key, "mesh-path") == 0) {
                handled = parse_text(value, profile->gdc_mesh,
                                     sizeof(profile->gdc_mesh)) == 0 ? 1 : -1;
            } else
                handled = geometry_value(&profile->gdc, key, value);
        } else if (section == SECTION_OUTPUT) {
            if (strcmp(key, "enable") == 0) {
                int enabled = 0;
                handled = parse_integer(value, &enabled) == 0 &&
                          (enabled == 0 || enabled == 1) ? 1 : -1;
                profile->output_enabled = enabled != 0;
            } else
                handled = geometry_value(&profile->output, key, value);
        } else if (section == SECTION_STITCH) {
            if (strcmp(key, "mode") == 0)
                handled = parse_text(value, profile->stitch_mode,
                                     sizeof(profile->stitch_mode)) == 0 ? 1 : -1;
            else if (strcmp(key, "fovx") == 0)
                handled = parse_integer(value, &profile->stitch_fov_x) == 0 ? 1 : -1;
            else if (strcmp(key, "fovy") == 0)
                handled = parse_integer(value, &profile->stitch_fov_y) == 0 ? 1 : -1;
            else
                handled = geometry_value(&profile->stitch, key, value);
        } else if (section == SECTION_ENCODER) {
            if (list_item) {
                if (encoder_item)
                    ++encoder_index;
                encoder_item = true;
            }
            if (encoder_index >= NGCD_PROFILE_ENCODERS)
                handled = -1;
            else {
                handled = encoder_value(profile, encoder_index, key, value);
                if (handled > 0 && profile->encoder_count < encoder_index + 1)
                    profile->encoder_count = encoder_index + 1;
            }
        }
        if (handled <= 0) {
            set_error(error, error_size, line_number,
                      handled < 0 ? "invalid value" : "unknown key");
            return -1;
        }
    }
    if (validate_profile(profile) != 0) {
        set_error(error, error_size, 0, "incomplete or unsafe profile");
        return -1;
    }
    if (error != NULL && error_size > 0)
        error[0] = '\0';
    return 0;
}

int ngcd_profile_load(const char *path, struct ngcd_profile *profile,
                      char *error, size_t error_size)
{
    FILE *stream;
    char buffer[16384];
    size_t used;
    int extra;
    stream = fopen(path, "rb");
    if (stream == NULL) {
        set_error(error, error_size, 0, "cannot open profile");
        return -1;
    }
    used = fread(buffer, 1, sizeof(buffer), stream);
    extra = fgetc(stream);
    if (ferror(stream) || extra != EOF) {
        fclose(stream);
        set_error(error, error_size, 0, "profile is too large or unreadable");
        return -1;
    }
    if (fclose(stream) != 0) {
        set_error(error, error_size, 0, "cannot close profile");
        return -1;
    }
    return ngcd_profile_parse(buffer, used, profile, error, error_size);
}
