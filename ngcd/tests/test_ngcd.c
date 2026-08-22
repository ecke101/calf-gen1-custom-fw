#include "ngcd.h"
#include "ngcd_rk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct ngcd_response dispatch(struct ngcd_app *app,
                                     enum ngcd_method method,
                                     const char *path, const char *query,
                                     const char *body)
{
    struct ngcd_request request;
    struct ngcd_response response;
    memset(&request, 0, sizeof(request));
    request.method = method;
    assert(strlen(path) < sizeof(request.path));
    memcpy(request.path, path, strlen(path) + 1);
    if (query != NULL) {
        assert(strlen(query) < sizeof(request.query));
        memcpy(request.query, query, strlen(query) + 1);
    }
    request.body = body;
    request.body_length = body != NULL ? strlen(body) : 0;
    assert(ngcd_dispatch(app, &request, &response) == 0);
    if (response.body_data == NULL)
        assert(response.body_length == strlen(response.body));
    return response;
}

static int unavailable_storage_status(struct ngcd_backend *backend,
                                      struct ngcd_storage_info *info)
{
    (void)backend;
    memset(info, 0, sizeof(*info));
    return -1;
}

static void test_json(void)
{
    const char json[] =
        "{\"ignored\":{\"nested\":[1,2,{\"x\":true}]},"
        "\"name\":\"left\\nright \\u00e5\",\"number\":\"-42\","
        "\"enabled\":true}";
    char value[64];
    char escaped[64];
    int64_t number;
    bool enabled;
    assert(ngcd_json_get_string(json, strlen(json), "name", value,
                                sizeof(value)) == 1);
    assert(strcmp(value, "left\nright \xc3\xa5") == 0);
    assert(ngcd_json_get_int64(json, strlen(json), "number", &number) == 1);
    assert(number == -42);
    assert(ngcd_json_get_bool(json, strlen(json), "enabled", &enabled) == 1);
    assert(enabled);
    assert(ngcd_json_get_string(json, strlen(json), "absent", value,
                                sizeof(value)) == 0);
    assert(ngcd_json_get_string("{\"x\":\"bad\\q\"}", 13, "x", value,
                                sizeof(value)) < 0);
    assert(ngcd_json_escape(escaped, sizeof(escaped), "a\n\"b") == 6);
    assert(strcmp(escaped, "a\\n\\\"b") == 0);
}

static void test_wifi_parsers(void)
{
    static const char status[] =
        "bssid=b2:8b:a9:3c:c3:96\n"
        "ssid=wifi\n"
        "wpa_state=COMPLETED\n"
        "ip_address=192.168.1.67\n"
        "address=70:f7:54:e2:36:45\n";
    static const char disconnected[] =
        "wpa_state=DISCONNECTED\naddress=70:f7:54:e2:36:45\n";
    static const char scan[] =
        "bssid / frequency / signal level / flags / ssid\n"
        "00:00:00:00:00:01\t2437\t-78\t[WPA2][ESS]\tLab \"Guest\"\n"
        "00:00:00:00:00:02\t2437\t-55\t[WPA2][ESS]\twifi\n"
        "00:00:00:00:00:03\t2437\t-70\t[WPA2][ESS]\twifi\n"
        "00:00:00:00:00:04\t2437\t-90\t[WPA2][ESS]\t\n";
    struct ngcd_wifi_info info;
    struct ngcd_wifi_network networks[4];
    size_t count;
    assert(ngcd_wifi_parse_status(status, sizeof(status) - 1, &info) == 0);
    assert(strcmp(info.ssid, "wifi") == 0);
    assert(strcmp(info.ip_address, "192.168.1.67") == 0);
    assert(strcmp(info.mac_address, "70:f7:54:e2:36:45") == 0);
    assert(ngcd_wifi_parse_status(disconnected, sizeof(disconnected) - 1,
                                  &info) == 0);
    assert(info.ssid[0] == '\0' && info.ip_address[0] == '\0');
    assert(ngcd_wifi_parse_scan_results(scan, sizeof(scan) - 1, networks,
                                        4, &count) == 0);
    assert(count == 2);
    assert(strcmp(networks[0].ssid, "wifi") == 0);
    assert(networks[0].level == -55 && networks[0].quality == 4);
    assert(strcmp(networks[1].ssid, "Lab \"Guest\"") == 0);
    assert(networks[1].level == -78 && networks[1].quality == 2);
    assert(ngcd_wifi_parse_status("FAIL\n", 5, &info) != 0);
}

static void test_usb_ethernet_mapping(void)
{
    char loopback[16];
    assert(strcmp(ngcd_usb_ethernet_udc("USB1"), "fc000000.usb") == 0);
    assert(strcmp(ngcd_usb_ethernet_udc("usb2"), "fc400000.usb") == 0);
    assert(ngcd_usb_ethernet_udc("USB3") == NULL);
    assert(strcmp(ngcd_usb_ethernet_function("win"), "rndis.usb0") == 0);
    assert(strcmp(ngcd_usb_ethernet_function("MAC"), "ecm.usb0") == 0);
    assert(ngcd_usb_ethernet_function("linux") == NULL);
    assert(ngcd_network_interface_ipv4("lo", loopback,
                                       sizeof(loopback)) == 0);
    assert(strcmp(loopback, "127.0.0.1") == 0);
    assert(ngcd_network_interface_ipv4("not-an-interface", loopback,
                                       sizeof(loopback)) != 0);
}

static void test_power_parser(void)
{
    int value = -1;
    assert(ngcd_power_parse_value("100\n", 0, 100, &value) == 0);
    assert(value == 100);
    assert(ngcd_power_parse_value(" 54\t\n", 0, 100, &value) == 0);
    assert(value == 54);
    assert(ngcd_power_parse_value("101\n", 0, 100, &value) != 0);
    assert(ngcd_power_parse_value("1online\n", 0, 1, &value) != 0);
    assert(ngcd_power_parse_value("\n", 0, 100, &value) != 0);
}

static void test_stock_session_marker(void)
{
    char path[128];
    char value[64];
    FILE *file;
    int count = snprintf(path, sizeof(path),
                         "/tmp/calf-ngcd-marker-test-%ld", (long)getpid());
    assert(count > 0 && (size_t)count < sizeof(path));
    (void)unlink(path);
    assert(ngcd_write_stock_session_marker(path) == 0);
    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fgets(value, sizeof(value), file) != NULL);
    assert(strcmp(value, "ngcd-fallback\n") == 0);
    assert(fclose(file) == 0);
    assert(unlink(path) == 0);
}

static void test_storage_mount_parser(void)
{
    static const char mounts[] =
        "/dev/root / ext4 rw 0 0\n"
        "/dev/mmcblk0p9 /app ext4 rw 0 0\n"
        "/dev/mmcblk1p1 /mnt/mmcblk1p1 exfat rw 0 0\n";
    static const char wrong_prefix[] =
        "/dev/mmcblk1p1 /mnt/mmcblk1p10 exfat rw 0 0\n";
    static const char read_only_mount[] =
        "/dev/mmcblk1p1 /mnt/mmcblk1p1 exfat ro,nosuid,nodev 0 0\n";
    char location[32];
    bool read_only = true;
    assert(ngcd_storage_parse_mounts(mounts, sizeof(mounts) - 1,
                                     location, sizeof(location),
                                     &read_only) == 0);
    assert(strcmp(location, "/mnt/mmcblk1p1") == 0);
    assert(!read_only);
    assert(ngcd_storage_parse_mounts(wrong_prefix,
                                     sizeof(wrong_prefix) - 1,
                                     location, sizeof(location),
                                     &read_only) != 0);
    assert(location[0] == '\0');
    assert(!read_only);
    assert(ngcd_storage_parse_mounts(read_only_mount,
                                     sizeof(read_only_mount) - 1,
                                     location, sizeof(location),
                                     &read_only) == 0);
    assert(read_only);
}

static void test_storage_io_file(void)
{
    char directory[] = "/tmp/ngcd-storage-test-XXXXXX";
    char path[256];
    FILE *existing;
    int rate = -1;
    int length;
    assert(mkdtemp(directory) != NULL);
    length = snprintf(path, sizeof(path), "%s/probe.tmp", directory);
    assert(length > 0 && (size_t)length < sizeof(path));
    assert(ngcd_storage_io_test_file(path, 4, 4,
                                     UINT64_C(32) * 1024U * 1024U,
                                     &rate) == 0);
    assert(rate > 0);
    assert(access(path, F_OK) != 0);
    assert(ngcd_storage_io_test_file(path, 0, 4,
                                     UINT64_C(32) * 1024U * 1024U,
                                     &rate) != 0);
    assert(ngcd_storage_io_test_file(path, 4096, 17,
                                     UINT64_C(128) * 1024U * 1024U,
                                     &rate) != 0);
    assert(ngcd_storage_io_test_file(path, 4, 4,
                                     UINT64_C(16) * 1024U * 1024U,
                                     &rate) != 0);
    existing = fopen(path, "wb");
    assert(existing != NULL);
    assert(fwrite("keep", 4, 1, existing) == 1);
    assert(fclose(existing) == 0);
    assert(ngcd_storage_io_test_file(path, 4, 4,
                                     UINT64_C(32) * 1024U * 1024U,
                                     &rate) != 0);
    assert(access(path, F_OK) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_storage_media_paths(void)
{
    static const unsigned char jpeg[] = {
        0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08,
        0x00, 0x10, 0x00, 0x20, 0x03, 0x01, 0x11,
        0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xd9,
    };
    char root[] = "/tmp/ngcd-media-test-XXXXXX";
    char basename[32];
    char temporary[512];
    char final_path[512];
    char first_path[512];
    char existing_path[512];
    char photo_path[512];
    char raw_left_path[512];
    char raw_right_path[512];
    char high_path[512];
    char hidden_path[512];
    char media_directory[512];
    char dcim_directory[512];
    FILE *file;
    struct ngcd_media_entry entries[4];
    size_t media_count;
    size_t media_total;
    unsigned int sequence = 0U;
    int count;
    assert(mkdtemp(root) != NULL);
    assert(ngcd_storage_media_paths(
               root, "jpg", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) == 0);
    assert(sequence == 1000000U);
    assert(strcmp(basename, "V1000000.jpg") == 0);
    assert(strstr(final_path, "/DCIM/100_CALF/V1000000.jpg") != NULL);
    assert(strstr(temporary,
                  "/DCIM/100_CALF/.V1000000.jpg.ngcd-") != NULL);
    memcpy(first_path, final_path, strlen(final_path) + 1U);
    file = fopen(first_path, "wbx");
    assert(file != NULL && fwrite(jpeg, 1U, sizeof(jpeg), file) ==
                               sizeof(jpeg) &&
           fclose(file) == 0);
    count = snprintf(existing_path, sizeof(existing_path),
                     "%s/DCIM/100_CALF/V1000042.mp4", root);
    assert(count > 0 && (size_t)count < sizeof(existing_path));
    file = fopen(existing_path, "wbx");
    assert(file != NULL && fwrite("v", 1U, 1U, file) == 1U &&
           fclose(file) == 0);
    assert(ngcd_storage_media_list(root, 0U, entries, 4U,
                                   &media_count, &media_total) == 0);
    assert(media_count == 1U && media_total == 1U);
    assert(strcmp(entries[0].name, "V1000000.jpg") == 0);
    assert(!entries[0].video && entries[0].size == sizeof(jpeg));
    assert(entries[0].create_time > 0U);
    assert(ngcd_storage_media_list(root, 1U, entries, 1U,
                                   &media_count, &media_total) == 0);
    assert(media_count == 0U && media_total == 1U);
    sequence = 0U;
    assert(ngcd_storage_media_paths(
               root, "jpg", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) == 0);
    assert(sequence == 1000043U);
    assert(strcmp(basename, "V1000043.jpg") == 0);
    memcpy(photo_path, final_path, strlen(final_path) + 1U);
    file = fopen(photo_path, "wbx");
    assert(file != NULL && fwrite(jpeg, 1U, sizeof(jpeg), file) ==
                               sizeof(jpeg) && fclose(file) == 0);
    count = snprintf(raw_left_path, sizeof(raw_left_path),
                     "%s/DCIM/100_CALF/V1000043-L.dng", root);
    assert(count > 0 && (size_t)count < sizeof(raw_left_path));
    count = snprintf(raw_right_path, sizeof(raw_right_path),
                     "%s/DCIM/100_CALF/V1000043-R.dng", root);
    assert(count > 0 && (size_t)count < sizeof(raw_right_path));
    file = fopen(raw_left_path, "wbx");
    assert(file != NULL && fwrite("l", 1U, 1U, file) == 1U &&
           fclose(file) == 0);
    file = fopen(raw_right_path, "wbx");
    assert(file != NULL && fwrite("r", 1U, 1U, file) == 1U &&
           fclose(file) == 0);
    /* JPEG plus left/right RAW sidecars are one capture and deliberately
     * share 1000043; the next video must still advance to 1000044. */
    sequence = 1000999U;
    assert(ngcd_storage_media_paths(
               root, "mp4", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) == 0);
    assert(sequence == 1000044U);
    assert(strcmp(basename, "V1000044.mp4") == 0);
    count = snprintf(high_path, sizeof(high_path),
                     "%s/DCIM/100_CALF/V1000046.mp4", root);
    assert(count > 0 && (size_t)count < sizeof(high_path));
    file = fopen(high_path, "wbx");
    assert(file != NULL && fwrite("v", 1U, 1U, file) == 1U &&
           fclose(file) == 0);
    assert(unlink(photo_path) == 0);
    assert(unlink(raw_left_path) == 0);
    assert(unlink(raw_right_path) == 0);
    sequence = 0U;
    assert(ngcd_storage_media_paths(
               root, "jpg", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) == 0);
    assert(sequence == 1000047U);
    assert(strcmp(basename, "V1000047.jpg") == 0);
    count = snprintf(hidden_path, sizeof(hidden_path),
                     "%s/DCIM/100_CALF/.V1000048.mp4.ngcd-123.tmp", root);
    assert(count > 0 && (size_t)count < sizeof(hidden_path));
    file = fopen(hidden_path, "wbx");
    assert(file != NULL && fclose(file) == 0);
    assert(ngcd_storage_media_paths(
               root, "jpg", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) == 0);
    assert(sequence == 1000049U);
    assert(strcmp(basename, "V1000049.jpg") == 0);
    assert(ngcd_storage_media_paths(
               root, "mov", &sequence, basename, sizeof(basename),
               temporary, sizeof(temporary), final_path,
               sizeof(final_path)) != 0);
    assert(unlink(first_path) == 0);
    assert(unlink(existing_path) == 0);
    assert(unlink(high_path) == 0);
    assert(unlink(hidden_path) == 0);
    count = snprintf(media_directory, sizeof(media_directory),
                     "%s/DCIM/100_CALF", root);
    assert(count > 0 && (size_t)count < sizeof(media_directory));
    count = snprintf(dcim_directory, sizeof(dcim_directory), "%s/DCIM", root);
    assert(count > 0 && (size_t)count < sizeof(dcim_directory));
    assert(rmdir(media_directory) == 0);
    assert(rmdir(dcim_directory) == 0);
    assert(rmdir(root) == 0);
}

struct fake_acp_api {
    struct ngcd_rk_aiq_acp_attr sensor[2];
    struct ngcd_rk_aiq_exp_sw_attr exposure[2];
    struct ngcd_rk_aiq_lin_exp_attr linear_exposure[2];
    struct ngcd_rk_aiq_exp_query_info exposure_query[2];
    struct ngcd_rk_aiq_effect_attr effect[2];
    unsigned int sharpness[2];
    unsigned int anr[2];
    unsigned int spatial_nr[2];
    unsigned int temporal_nr[2];
    unsigned int white_balance_mode[2];
    unsigned int white_balance_ct[2];
    unsigned char flicker_enabled[2];
    unsigned int flicker_mode[2];
    unsigned int power_line_frequency[2];
    int flicker_auto_changes_frequency;
    int fail_get_sensor;
    int fail_set_sensor;
    int detail_set_calls;
    int fail_detail_call;
    float exposure_time_scale;
    unsigned int exposure_get_sync_mode[2];
    unsigned char exposure_get_done[2];
    unsigned int exposure_set_sync_mode[2];
    unsigned char exposure_set_done[2];
    struct ngcd_rk_aiq_exp_sw_attr exposure_pending[2];
    int exposure_pending_gets[2];
    int exposure_async_delay;
    bool exposure_pending_valid[2];
};

static int fake_acp_sensor(void *sensor_context)
{
    uintptr_t value = (uintptr_t)sensor_context;
    return value >= 1U && value <= 2U ? (int)value - 1 : -1;
}

static int fake_get_acp(void *context, void *sensor_context,
                        struct ngcd_rk_aiq_acp_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    *attribute = fake->sensor[sensor];
    return 0;
}

static int fake_set_acp(void *context, void *sensor_context,
                        const struct ngcd_rk_aiq_acp_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_set_sensor)
        return -1;
    fake->sensor[sensor] = *attribute;
    return 0;
}

static int fake_get_strength(void *context, void *sensor_context,
                             unsigned int *strength,
                             const unsigned int *values)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    *strength = values[sensor];
    return 0;
}

static int fake_set_strength(void *context, void *sensor_context,
                             unsigned int strength, unsigned int *values)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    ++fake->detail_set_calls;
    if (sensor < 0 || fake->detail_set_calls == fake->fail_detail_call)
        return -1;
    values[sensor] = strength;
    return 0;
}

#define FAKE_STRENGTH_CALLBACKS(name, field)                                  \
    static int fake_get_##name(void *context, void *sensor_context,          \
                               unsigned int *strength)                        \
    {                                                                         \
        struct fake_acp_api *fake = context;                                  \
        return fake_get_strength(context, sensor_context, strength,           \
                                 fake->field);                                \
    }                                                                         \
    static int fake_set_##name(void *context, void *sensor_context,          \
                               unsigned int strength)                         \
    {                                                                         \
        struct fake_acp_api *fake = context;                                  \
        return fake_set_strength(context, sensor_context, strength,           \
                                 fake->field);                                \
    }

FAKE_STRENGTH_CALLBACKS(sharpness, sharpness)
FAKE_STRENGTH_CALLBACKS(anr, anr)
FAKE_STRENGTH_CALLBACKS(spatial_nr, spatial_nr)
FAKE_STRENGTH_CALLBACKS(temporal_nr, temporal_nr)
FAKE_STRENGTH_CALLBACKS(white_balance_mode, white_balance_mode)
FAKE_STRENGTH_CALLBACKS(power_line_frequency, power_line_frequency)

static int fake_get_flicker_mode(void *context, void *sensor_context,
                                 unsigned int *mode)
{
    struct fake_acp_api *fake = context;
    return fake_get_strength(context, sensor_context, mode,
                             fake->flicker_mode);
}

static int fake_set_flicker_mode(void *context, void *sensor_context,
                                 unsigned int mode)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (fake_set_strength(context, sensor_context, mode,
                          fake->flicker_mode) != 0)
        return -1;
    if (mode == 1U && fake->flicker_auto_changes_frequency)
        fake->power_line_frequency[sensor] =
            fake->power_line_frequency[sensor] == 1U ? 2U : 1U;
    return 0;
}

static int fake_get_white_balance_ct(void *context, void *sensor_context,
                                     unsigned int *kelvin)
{
    struct fake_acp_api *fake = context;
    return fake_get_strength(context, sensor_context, kelvin,
                             fake->white_balance_ct);
}

static int fake_set_white_balance_ct(void *context, void *sensor_context,
                                     unsigned int kelvin)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (fake_set_strength(context, sensor_context, kelvin,
                          fake->white_balance_ct) != 0)
        return -1;
    fake->white_balance_mode[sensor] = 1U;
    return 0;
}

static int fake_get_flicker_enabled(void *context, void *sensor_context,
                                    unsigned char *enabled)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    *enabled = fake->flicker_enabled[sensor];
    return 0;
}

static int fake_set_flicker_enabled(void *context, void *sensor_context,
                                    unsigned char enabled)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    ++fake->detail_set_calls;
    if (sensor < 0 || fake->detail_set_calls == fake->fail_detail_call)
        return -1;
    fake->flicker_enabled[sensor] = enabled;
    return 0;
}

static int fake_get_effect(void *context, void *sensor_context,
                           struct ngcd_rk_aiq_effect_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    *attribute = fake->effect[sensor];
    return 0;
}

static int fake_set_effect(void *context, void *sensor_context,
                           const struct ngcd_rk_aiq_effect_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    ++fake->detail_set_calls;
    if (sensor < 0 || fake->detail_set_calls == fake->fail_detail_call)
        return -1;
    fake->effect[sensor] = *attribute;
    return 0;
}

static unsigned int fake_load_effect(const struct ngcd_rk_aiq_effect_attr *attr)
{
    unsigned int effect;
    memcpy(&effect, attr->bytes + 8U, sizeof(effect));
    return effect;
}

static int fake_get_exposure(void *context, void *sensor_context,
                             struct ngcd_rk_aiq_exp_sw_attr *attribute)
{
    struct fake_acp_api *fake = context;
    unsigned int sync_mode;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    memcpy(&sync_mode, attribute->bytes, sizeof(sync_mode));
    fake->exposure_get_sync_mode[sensor] = sync_mode;
    fake->exposure_get_done[sensor] = attribute->bytes[4U];
    if (sync_mode == 1U && fake->exposure_pending_valid[sensor]) {
        if (fake->exposure_pending_gets[sensor] == 0) {
            fake->exposure[sensor] = fake->exposure_pending[sensor];
            fake->exposure_pending_valid[sensor] = false;
        } else {
            --fake->exposure_pending_gets[sensor];
        }
    }
    memcpy(attribute, &fake->exposure[sensor], sizeof(*attribute));
    return 0;
}

static int fake_set_exposure(
    void *context, void *sensor_context,
    const struct ngcd_rk_aiq_exp_sw_attr *attribute)
{
    struct fake_acp_api *fake = context;
    struct ngcd_rk_aiq_exp_sw_attr *stored;
    unsigned int sync_mode;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_set_sensor)
        return -1;
    memcpy(&sync_mode, attribute->bytes, sizeof(sync_mode));
    fake->exposure_set_sync_mode[sensor] = sync_mode;
    fake->exposure_set_done[sensor] = attribute->bytes[4U];
    if (sync_mode == 2U && fake->exposure_async_delay > 0) {
        stored = &fake->exposure_pending[sensor];
        fake->exposure_pending_gets[sensor] = fake->exposure_async_delay;
        fake->exposure_pending_valid[sensor] = true;
    } else {
        stored = &fake->exposure[sensor];
        fake->exposure_pending_valid[sensor] = false;
    }
    memcpy(stored, attribute, sizeof(*attribute));
    if (fake->exposure_time_scale > 0.0f) {
        float time_min;
        float time_max;
        memcpy(&time_min, stored->bytes + 0x480U,
               sizeof(time_min));
        memcpy(&time_max, stored->bytes + 0x484U,
               sizeof(time_max));
        time_min *= fake->exposure_time_scale;
        time_max *= fake->exposure_time_scale;
        memcpy(stored->bytes + 0x480U, &time_min,
               sizeof(time_min));
        memcpy(stored->bytes + 0x484U, &time_max,
               sizeof(time_max));
    }
    return 0;
}

static int fake_query_exposure(
    void *context, void *sensor_context,
    struct ngcd_rk_aiq_exp_query_info *information)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    *information = fake->exposure_query[sensor];
    return 0;
}

static float fake_load_f32(const struct ngcd_rk_aiq_exp_sw_attr *attribute,
                           size_t offset)
{
    float value;
    memcpy(&value, attribute->bytes + offset, sizeof(value));
    return value;
}

static int fake_get_linear_exposure(
    void *context, void *sensor_context,
    struct ngcd_rk_aiq_lin_exp_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_get_sensor)
        return -1;
    memcpy(attribute, &fake->linear_exposure[sensor], sizeof(*attribute));
    return 0;
}

static int fake_set_linear_exposure(
    void *context, void *sensor_context,
    const struct ngcd_rk_aiq_lin_exp_attr *attribute)
{
    struct fake_acp_api *fake = context;
    int sensor = fake_acp_sensor(sensor_context);
    if (sensor < 0 || sensor == fake->fail_set_sensor)
        return -1;
    memcpy(&fake->linear_exposure[sensor], attribute, sizeof(*attribute));
    return 0;
}

static float fake_load_linear_f32(
    const struct ngcd_rk_aiq_lin_exp_attr *attribute, size_t offset)
{
    float value;
    memcpy(&value, attribute->bytes + offset, sizeof(value));
    return value;
}

static void test_rockchip_acp_controls(void)
{
    struct fake_acp_api fake;
    struct ngcd_rk_api api;
    struct ngcd_rk_graph graph;
    struct ngcd_rk_aiq_acp_attr original[2];
    struct ngcd_rk_aiq_exp_sw_attr original_exposure[2];
    struct ngcd_rk_aiq_lin_exp_attr original_linear_exposure[2];
    struct ngcd_rk_aiq_effect_attr original_effect[2];
    struct ngcd_rk_image_readback image_readback;
    unsigned char original_flicker_enabled[2];
    unsigned int original_flicker_mode[2];
    unsigned int original_power_line_frequency[2];
    enum ngcd_rk_flicker_control flicker_readback = NGCD_RK_FLICKER_OFF;
    unsigned int iso_readback = 0;
    float exposure_readback = 0.0f;
    int readback = -1;
    memset(&fake, 0, sizeof(fake));
    memset(&api, 0, sizeof(api));
    memset(&graph, 0, sizeof(graph));
    assert(sizeof(struct ngcd_rk_aiq_acp_attr) == 12U);
    assert(sizeof(struct ngcd_rk_aiq_exp_sw_attr) == 1264U);
    assert(sizeof(struct ngcd_rk_aiq_lin_exp_attr) == 312U);
    assert(sizeof(struct ngcd_rk_aiq_exp_query_info) == 2472U);
    assert(sizeof(struct ngcd_rk_aiq_effect_attr) == 12U);
    memset(&fake.sensor[0], 0x11, sizeof(fake.sensor[0]));
    memset(&fake.sensor[1], 0x22, sizeof(fake.sensor[1]));
    fake.fail_get_sensor = -1;
    fake.fail_set_sensor = -1;
    api.aiq_get_acp = fake_get_acp;
    api.aiq_set_acp = fake_set_acp;
    api.aiq_get_sharpness = fake_get_sharpness;
    api.aiq_set_sharpness = fake_set_sharpness;
    api.aiq_get_anr = fake_get_anr;
    api.aiq_set_anr = fake_set_anr;
    api.aiq_get_spatial_nr = fake_get_spatial_nr;
    api.aiq_set_spatial_nr = fake_set_spatial_nr;
    api.aiq_get_temporal_nr = fake_get_temporal_nr;
    api.aiq_set_temporal_nr = fake_set_temporal_nr;
    api.aiq_get_exposure = fake_get_exposure;
    api.aiq_set_exposure = fake_set_exposure;
    api.aiq_get_linear_exposure = fake_get_linear_exposure;
    api.aiq_set_linear_exposure = fake_set_linear_exposure;
    api.aiq_query_exposure = fake_query_exposure;
    api.aiq_get_white_balance_mode = fake_get_white_balance_mode;
    api.aiq_set_white_balance_mode = fake_set_white_balance_mode;
    api.aiq_get_white_balance_ct = fake_get_white_balance_ct;
    api.aiq_set_white_balance_ct = fake_set_white_balance_ct;
    api.aiq_get_flicker_enabled = fake_get_flicker_enabled;
    api.aiq_set_flicker_enabled = fake_set_flicker_enabled;
    api.aiq_get_flicker_mode = fake_get_flicker_mode;
    api.aiq_set_flicker_mode = fake_set_flicker_mode;
    api.aiq_get_power_line_frequency = fake_get_power_line_frequency;
    api.aiq_set_power_line_frequency = fake_set_power_line_frequency;
    api.aiq_get_effect = fake_get_effect;
    api.aiq_set_effect = fake_set_effect;
    graph.api = &api;
    graph.api_context = &fake;
    graph.sensor_handle[0] = (void *)(uintptr_t)1U;
    graph.sensor_handle[1] = (void *)(uintptr_t)2U;
    graph.sensor_count = 2;
    graph.sensor_mask = 3U;

    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_BRIGHTNESS,
                                 7, &readback) == 0);
    assert(readback == 7);
    assert(fake.sensor[0].bytes[8] == 88U);
    assert(fake.sensor[1].bytes[8] == 88U);
    assert(fake.sensor[0].bytes[7] == 0x11U);
    assert(fake.sensor[1].bytes[7] == 0x22U);
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_CONTRAST,
                                 20, &readback) == 0);
    assert(fake.sensor[0].bytes[9] == 250U);
    assert(fake.sensor[1].bytes[9] == 250U);

    original[0] = fake.sensor[0];
    original[1] = fake.sensor[1];
    fake.fail_set_sensor = 1;
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_SATURATION,
                                 4, &readback) != 0);
    assert(memcmp(&fake.sensor[0], &original[0], sizeof(original[0])) == 0);
    assert(memcmp(&fake.sensor[1], &original[1], sizeof(original[1])) == 0);
    fake.fail_set_sensor = -1;
    fake.fail_get_sensor = 1;
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_HUE,
                                 4, &readback) != 0);
    assert(memcmp(&fake.sensor[0], &original[0], sizeof(original[0])) == 0);
    assert(memcmp(&fake.sensor[1], &original[1], sizeof(original[1])) == 0);
    fake.fail_get_sensor = -1;
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_HUE,
                                 21, &readback) != 0);
    graph.sensor_handle[0] = NULL;
    graph.sensor_count = 1;
    graph.sensor_mask = 2U;
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_HUE,
                                 6, &readback) == 0);
    assert(fake.sensor[1].bytes[11] == 75U);

    graph.sensor_handle[0] = (void *)(uintptr_t)1U;
    graph.sensor_count = 2;
    graph.sensor_mask = 3U;
    fake.sharpness[0] = 20U;
    fake.sharpness[1] = 30U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    assert(ngcd_rk_image_set_sharpness(&graph, 12, &readback) == 0);
    assert(readback == 12);
    assert(fake.sharpness[0] == 60U && fake.sharpness[1] == 60U);
    fake.sharpness[0] = 20U;
    fake.sharpness[1] = 30U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 2;
    assert(ngcd_rk_image_set_sharpness(&graph, 8, &readback) != 0);
    assert(fake.sharpness[0] == 20U && fake.sharpness[1] == 30U);

    fake.anr[0] = 10U;
    fake.spatial_nr[0] = 11U;
    fake.temporal_nr[0] = 12U;
    fake.anr[1] = 20U;
    fake.spatial_nr[1] = 21U;
    fake.temporal_nr[1] = 22U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    assert(ngcd_rk_image_set_noise_reduction(&graph, 5, &readback) == 0);
    assert(readback == 5);
    assert(fake.anr[0] == 25U && fake.spatial_nr[0] == 25U &&
           fake.temporal_nr[0] == 25U);
    assert(fake.anr[1] == 25U && fake.spatial_nr[1] == 25U &&
           fake.temporal_nr[1] == 25U);
    fake.anr[0] = 10U;
    fake.spatial_nr[0] = 11U;
    fake.temporal_nr[0] = 12U;
    fake.anr[1] = 20U;
    fake.spatial_nr[1] = 21U;
    fake.temporal_nr[1] = 22U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 5;
    assert(ngcd_rk_image_set_noise_reduction(&graph, 9, &readback) != 0);
    assert(fake.anr[0] == 10U && fake.spatial_nr[0] == 11U &&
           fake.temporal_nr[0] == 12U);
    assert(fake.anr[1] == 20U && fake.spatial_nr[1] == 21U &&
           fake.temporal_nr[1] == 22U);

    fake.fail_set_sensor = -1;
    memset(fake.exposure, 0x5a, sizeof(fake.exposure));
    {
        float time_min = 0.125f;
        float time_max = 0.25f;
        memcpy(fake.exposure[0].bytes + 0x480U, &time_min,
               sizeof(time_min));
        memcpy(fake.exposure[0].bytes + 0x484U, &time_max,
               sizeof(time_max));
        memcpy(fake.exposure[1].bytes + 0x480U, &time_min,
               sizeof(time_min));
        memcpy(fake.exposure[1].bytes + 0x484U, &time_max,
               sizeof(time_max));
    }
    assert(ngcd_rk_image_set_iso(&graph, 800U, &iso_readback) == 0);
    assert(iso_readback == 800U);
    assert(fake_load_f32(&fake.exposure[0], 0x488U) == 8.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x48cU) == 8.0f);
    assert(fake.exposure[0].bytes[0x118U] == 1U);
    assert(fake_load_f32(&fake.exposure[0], 0x11cU) == 30.0f);
    assert(fake_load_f32(&fake.exposure[1], 0x488U) == 8.0f);
    assert(fake_load_f32(&fake.exposure[1], 0x48cU) == 8.0f);
    assert(fake.exposure[1].bytes[0x118U] == 1U);
    assert(fake_load_f32(&fake.exposure[1], 0x11cU) == 30.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 0.125f);
    assert(fake_load_f32(&fake.exposure[0], 0x484U) == 0.25f);
    assert(fake.exposure_get_sync_mode[0] == 1U);
    assert(fake.exposure_get_sync_mode[1] == 1U);
    assert(fake.exposure_get_done[0] == 0U);
    assert(fake.exposure_get_done[1] == 0U);
    assert(fake.exposure_set_sync_mode[0] == 2U);
    assert(fake.exposure_set_sync_mode[1] == 2U);
    assert(fake.exposure_set_done[0] == 0U);
    assert(fake.exposure_set_done[1] == 0U);
    fake.exposure_async_delay = 2;
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.002f, false, 400U,
               &exposure_readback, &iso_readback) == 0);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 0.002f);
    assert(fake_load_f32(&fake.exposure[1], 0x480U) == 0.002f);
    fake.exposure_async_delay = 0;
    assert(ngcd_rk_image_set_iso(&graph, 101U, &iso_readback) != 0);
    original_exposure[0] = fake.exposure[0];
    original_exposure[1] = fake.exposure[1];
    fake.fail_set_sensor = 1;
    assert(ngcd_rk_image_set_iso(&graph, 1600U, &iso_readback) != 0);
    assert(memcmp(&fake.exposure[0], &original_exposure[0],
                  sizeof(original_exposure[0])) == 0);
    assert(memcmp(&fake.exposure[1], &original_exposure[1],
                  sizeof(original_exposure[1])) == 0);
    fake.fail_set_sensor = -1;
    assert(ngcd_rk_image_set_iso(&graph, 0U, &iso_readback) == 0);
    assert(fake_load_f32(&fake.exposure[0], 0x488U) == 1.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x48cU) == 128.0f);
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.001f, false, 800U,
               &exposure_readback, &iso_readback) == 0);
    assert(exposure_readback == 0.001f);
    assert(iso_readback == 800U);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 0.001f);
    assert(fake_load_f32(&fake.exposure[0], 0x484U) == 0.001f);
    assert(fake_load_f32(&fake.exposure[0], 0x488U) == 8.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x48cU) == 8.0f);
    assert(fake_load_f32(&fake.exposure[1], 0x480U) == 0.001f);
    assert(fake_load_f32(&fake.exposure[1], 0x484U) == 0.001f);
    assert(fake_load_f32(&fake.exposure[1], 0x488U) == 8.0f);
    assert(fake_load_f32(&fake.exposure[1], 0x48cU) == 8.0f);
    fake.exposure_time_scale = 0.995f;
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.5f, false, 400U,
               &exposure_readback, &iso_readback) == 0);
    assert(exposure_readback == 0.5f && iso_readback == 400U);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) < 0.5f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) > 0.49f);
    fake.exposure_time_scale = 0.96f;
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.25f, false, 100U,
               &exposure_readback, &iso_readback) == 0);
    assert(exposure_readback == 0.25f && iso_readback == 100U);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) < 0.25f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) > 0.239f);
    original_exposure[0] = fake.exposure[0];
    original_exposure[1] = fake.exposure[1];
    fake.exposure_time_scale = 0.85f;
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.25f, false, 100U,
               &exposure_readback, &iso_readback) != 0);
    /* The scale hook also affects rollback writes, unlike real AIQ. Restore
     * the fixture explicitly; rollback itself is covered by the injected
     * second-sensor failure below. */
    fake.exposure_time_scale = 0.0f;
    fake.exposure[0] = original_exposure[0];
    fake.exposure[1] = original_exposure[1];
    original_exposure[0] = fake.exposure[0];
    original_exposure[1] = fake.exposure[1];
    fake.fail_set_sensor = 1;
    assert(ngcd_rk_image_set_exposure_iso(
               &graph, 0.25f, false, 1600U,
               &exposure_readback, &iso_readback) != 0);
    assert(memcmp(&fake.exposure[0], &original_exposure[0],
                  sizeof(original_exposure[0])) == 0);
    assert(memcmp(&fake.exposure[1], &original_exposure[1],
                  sizeof(original_exposure[1])) == 0);
    fake.fail_set_sensor = -1;
    assert(ngcd_rk_image_set_iso(&graph, 0U, &iso_readback) == 0);

    assert(ngcd_rk_image_set_exposure(&graph, 0.125f, false,
                                      &exposure_readback) == 0);
    assert(exposure_readback == 0.125f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 0.125f);
    assert(fake_load_f32(&fake.exposure[0], 0x484U) == 0.125f);
    assert(fake_load_f32(&fake.exposure[1], 0x480U) == 0.125f);
    assert(fake_load_f32(&fake.exposure[1], 0x484U) == 0.125f);
    assert(fake_load_f32(&fake.exposure[0], 0x488U) == 1.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x48cU) == 128.0f);
    original_exposure[0] = fake.exposure[0];
    original_exposure[1] = fake.exposure[1];
    fake.fail_set_sensor = 1;
    assert(ngcd_rk_image_set_exposure(&graph, 0.25f, false,
                                      &exposure_readback) != 0);
    assert(memcmp(&fake.exposure[0], &original_exposure[0],
                  sizeof(original_exposure[0])) == 0);
    assert(memcmp(&fake.exposure[1], &original_exposure[1],
                  sizeof(original_exposure[1])) == 0);
    fake.fail_set_sensor = -1;
    assert(ngcd_rk_image_set_exposure(&graph, 0.0f, true,
                                      &exposure_readback) == 0);
    assert(exposure_readback == -1.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 0.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x484U) == 0.0f);
    assert(ngcd_rk_image_set_exposure(&graph, 0.0f, false,
                                      &exposure_readback) != 0);
    assert(ngcd_rk_image_set_exposure(&graph, 12.0f, false,
                                      &exposure_readback) == 0);
    assert(exposure_readback == 12.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x480U) == 12.0f);
    assert(fake_load_f32(&fake.exposure[0], 0x484U) == 12.0f);
    assert(ngcd_rk_image_set_exposure(&graph, 12.1f, false,
                                      &exposure_readback) != 0);
    assert(ngcd_rk_image_set_exposure(&graph, 0.0f, true,
                                      &exposure_readback) == 0);

    {
        float measured_seconds = 1.0f / 125.0f;
        float queried_seconds = 0.0f;
        int measured_iso = 640;
        memcpy(fake.exposure_query[0].bytes + 0x3acU, &measured_seconds,
               sizeof(measured_seconds));
        memcpy(fake.exposure_query[0].bytes + 0x3bcU, &measured_iso,
               sizeof(measured_iso));
        assert(ngcd_rk_image_query_exposure(
                   &graph, &queried_seconds, &iso_readback) == 0);
        assert(queried_seconds == measured_seconds);
        assert(iso_readback == 640U);
        fake.fail_get_sensor = 0;
        assert(ngcd_rk_image_query_exposure(
                   &graph, &queried_seconds, &iso_readback) != 0);
        fake.fail_get_sensor = -1;
        measured_iso = 0;
        memcpy(fake.exposure_query[0].bytes + 0x3bcU, &measured_iso,
               sizeof(measured_iso));
        {
            float analog_gain = 2.0f;
            float digital_gain = 1.5f;
            float isp_gain = 1.0f;
            memcpy(fake.exposure_query[0].bytes + 0x3b0U, &analog_gain,
                   sizeof(analog_gain));
            memcpy(fake.exposure_query[0].bytes + 0x3b4U, &digital_gain,
                   sizeof(digital_gain));
            memcpy(fake.exposure_query[0].bytes + 0x3b8U, &isp_gain,
                   sizeof(isp_gain));
            assert(ngcd_rk_image_query_exposure(
                       &graph, &queried_seconds, &iso_readback) == 0);
            assert(iso_readback == 300U);
            analog_gain = 0.0f;
            memcpy(fake.exposure_query[0].bytes + 0x3b0U, &analog_gain,
                   sizeof(analog_gain));
            assert(ngcd_rk_image_query_exposure(
                       &graph, &queried_seconds, &iso_readback) != 0);
        }
    }

    memset(fake.linear_exposure, 0x35, sizeof(fake.linear_exposure));
    assert(ngcd_rk_image_set_exposure_compensation(&graph, -2,
                                                   &readback) == 0);
    assert(readback == -2);
    assert(fake_load_linear_f32(&fake.linear_exposure[0], 0x14U) ==
           -58.4900016784668f);
    assert(fake_load_linear_f32(&fake.linear_exposure[1], 0x14U) ==
           -58.4900016784668f);
    original_linear_exposure[0] = fake.linear_exposure[0];
    original_linear_exposure[1] = fake.linear_exposure[1];
    fake.fail_set_sensor = 1;
    assert(ngcd_rk_image_set_exposure_compensation(&graph, 3,
                                                   &readback) != 0);
    assert(memcmp(&fake.linear_exposure[0], &original_linear_exposure[0],
                  sizeof(original_linear_exposure[0])) == 0);
    assert(memcmp(&fake.linear_exposure[1], &original_linear_exposure[1],
                  sizeof(original_linear_exposure[1])) == 0);
    fake.fail_set_sensor = -1;
    assert(ngcd_rk_image_set_exposure_compensation(&graph, 4,
                                                   &readback) != 0);

    fake.white_balance_mode[0] = 0U;
    fake.white_balance_mode[1] = 1U;
    fake.white_balance_ct[1] = 6000U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    assert(ngcd_rk_image_set_white_balance(&graph, 5200U,
                                           &iso_readback) == 0);
    assert(iso_readback == 5200U);
    assert(fake.white_balance_mode[0] == 1U &&
           fake.white_balance_mode[1] == 1U);
    assert(fake.white_balance_ct[0] == 5200U &&
           fake.white_balance_ct[1] == 5200U);
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 2;
    assert(ngcd_rk_image_set_white_balance(&graph, 7000U,
                                           &iso_readback) != 0);
    assert(fake.white_balance_mode[0] == 1U &&
           fake.white_balance_mode[1] == 1U);
    assert(fake.white_balance_ct[0] == 5200U &&
           fake.white_balance_ct[1] == 5200U);
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    assert(ngcd_rk_image_set_white_balance(&graph, 0U,
                                           &iso_readback) == 0);
    assert(fake.white_balance_mode[0] == 0U &&
           fake.white_balance_mode[1] == 0U);
    assert(ngcd_rk_image_set_white_balance(&graph, 999U,
                                           &iso_readback) != 0);

    fake.flicker_enabled[0] = 0U;
    fake.flicker_mode[0] = 1U;
    fake.power_line_frequency[0] = 2U;
    fake.flicker_enabled[1] = 1U;
    fake.flicker_mode[1] = 0U;
    fake.power_line_frequency[1] = 1U;
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    fake.flicker_auto_changes_frequency = 1;
    assert(ngcd_rk_image_set_flicker(&graph, NGCD_RK_FLICKER_AUTO,
                                     &flicker_readback) == 0);
    assert(flicker_readback == NGCD_RK_FLICKER_AUTO);
    assert(fake.detail_set_calls == 3);
    assert(fake.flicker_enabled[0] == 1U && fake.flicker_mode[0] == 1U &&
           fake.power_line_frequency[0] == 1U);
    assert(fake.flicker_enabled[1] == 1U && fake.flicker_mode[1] == 1U &&
           fake.power_line_frequency[1] == 2U);
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 1;
    assert(ngcd_rk_image_set_flicker(&graph, NGCD_RK_FLICKER_AUTO,
                                     &flicker_readback) == 0);
    assert(fake.detail_set_calls == 0);
    memcpy(original_flicker_enabled, fake.flicker_enabled,
           sizeof(original_flicker_enabled));
    memcpy(original_flicker_mode, fake.flicker_mode,
           sizeof(original_flicker_mode));
    memcpy(original_power_line_frequency, fake.power_line_frequency,
           sizeof(original_power_line_frequency));
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 4;
    assert(ngcd_rk_image_set_flicker(&graph, NGCD_RK_FLICKER_50HZ,
                                     &flicker_readback) != 0);
    assert(memcmp(fake.flicker_enabled, original_flicker_enabled,
                  sizeof(original_flicker_enabled)) == 0);
    assert(memcmp(fake.flicker_mode, original_flicker_mode,
                  sizeof(original_flicker_mode)) == 0);
    assert(memcmp(fake.power_line_frequency, original_power_line_frequency,
                  sizeof(original_power_line_frequency)) == 0);

    memset(fake.effect, 0x4b, sizeof(fake.effect));
    fake.detail_set_calls = 0;
    fake.fail_detail_call = -1;
    assert(ngcd_rk_image_set_effect(&graph, 3U, &iso_readback) == 0);
    assert(iso_readback == 3U);
    assert(fake_load_effect(&fake.effect[0]) == 3U);
    assert(fake_load_effect(&fake.effect[1]) == 3U);
    assert(fake.effect[0].bytes[7] == 0x4bU);
    original_effect[0] = fake.effect[0];
    original_effect[1] = fake.effect[1];
    fake.detail_set_calls = 0;
    fake.fail_detail_call = 2;
    assert(ngcd_rk_image_set_effect(&graph, 5U, &iso_readback) != 0);
    assert(memcmp(&fake.effect[0], &original_effect[0],
                  sizeof(original_effect[0])) == 0);
    assert(memcmp(&fake.effect[1], &original_effect[1],
                  sizeof(original_effect[1])) == 0);

    fake.fail_get_sensor = -1;
    fake.fail_set_sensor = -1;
    fake.fail_detail_call = -1;
    fake.detail_set_calls = 0;
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_BRIGHTNESS,
                                 7, &readback) == 0);
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_CONTRAST,
                                 20, &readback) == 0);
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_SATURATION,
                                 4, &readback) == 0);
    assert(ngcd_rk_image_set_acp(&graph, NGCD_RK_ACP_HUE,
                                 6, &readback) == 0);
    assert(ngcd_rk_image_set_sharpness(&graph, 12, &readback) == 0);
    assert(ngcd_rk_image_set_noise_reduction(&graph, 5, &readback) == 0);
    assert(ngcd_rk_image_read(&graph, &image_readback) == 0);
    assert(image_readback.exposure_automatic);
    assert(image_readback.iso_automatic);
    assert(image_readback.white_balance_automatic);
    assert(image_readback.exposure_compensation == -2);
    assert(image_readback.brightness == 7);
    assert(image_readback.contrast == 20);
    assert(image_readback.saturation == 4);
    assert(image_readback.hue == 6);
    assert(image_readback.sharpness == 12);
    assert(image_readback.noise_reduction == 5);
    assert(image_readback.flicker == NGCD_RK_FLICKER_AUTO);
    assert(image_readback.effect == 3U);
    fake.sensor[1].bytes[8] = 100U;
    assert(ngcd_rk_image_read(&graph, &image_readback) != 0);
}

static void test_contract(void)
{
    struct ngcd_app app;
    const struct ngcd_backend_ops *available_ops;
    struct ngcd_backend_ops unavailable_ops;
    struct ngcd_response response;
    ngcd_app_init(&app);
    assert(strstr(app.version, "ngcd-c-0.1") != NULL);
    assert(strcmp(app.serial_number, "unknown") == 0);
    assert(app.backend.ops->start(&app.backend) == 0);

    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/productinfo", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"hardware\":\"4.0\"") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/graphctrlmb", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"ctrlmb\":1") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/lcdscreenshot", NULL, NULL);
    assert(response.status == 200);
    assert(strcmp(response.content_type, "image/bmp") == 0);
    assert(response.body_data != NULL && response.body_length == 58U);
    assert(memcmp(response.body_data, "BM", 2U) == 0);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/imu_sample", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"acc_x\":1000") != NULL);
    assert(strstr(response.body, "\"monotonic_ns\":") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/imu_calib_state", NULL, NULL);
    assert(strstr(response.body, "\"calib_state\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/imu", NULL,
                        "{\"action\":\"start_calib\",\"type\":0,"
                        "\"save\":false,\"count\":5}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/imu_calib_state", NULL, NULL);
    assert(strstr(response.body, "\"calib_state\":3") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/imu", NULL,
                        "{\"action\":\"invalid\"}");
    assert(response.status == 400);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/imgparams", NULL,
                        "{\"type\":\"exp\",\"value\":\"0.125\"}");
    assert(response.status == 200 && strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/imgparams", NULL, NULL);
    assert(strstr(response.body, "\"exp\":\"0.125\"") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/imgparams", NULL,
                        "{\"type\":\"exp\",\"value\":\"0.0333333\","
                        "\"fixed\":true}");
    assert(response.status == 200 && strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/imgparams", NULL, NULL);
    assert(strstr(response.body, "\"exp\":\"0.125\"") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/imgparams", NULL,
                        "{\"type\":\"wb\",\"value\":\"auto\","
                        "\"fixed\":true}");
    assert(strstr(response.body, "\"code\":-1") != NULL);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/nightpreview", NULL,
                        "{\"fps\":4,\"exposure\":\"0.24\","
                        "\"iso\":\"iso200\"}");
    assert(response.status == 200 &&
           strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/nightpreview", NULL,
                        "{\"fps\":5,\"exposure\":\"0.2\","
                        "\"iso\":\"iso100\"}");
    assert(response.status == 400);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/recording", NULL,
                        "{\"action\":\"start\",\"file_split_type\":2}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/recording", NULL, NULL);
    assert(strstr(response.body, "\"running\":1") != NULL);
    assert(strstr(response.body, "\"duration\":") != NULL);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/cameramode", NULL,
                        "{\"action\":\"stop\"}");
    assert(strstr(response.body, "\"code\":-1") != NULL);
    assert(app.backend.state.camera_running);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/recording", NULL,
                        "{\"action\":\"stop\"}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/cameramode", NULL,
                        "{\"action\":\"start\",\"mode\":\"VR180_8K\"}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(strcmp(app.backend.state.camera_mode, "VR180_8K") == 0);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/vencattr", NULL,
                        "{\"channel\":1,\"vcodec\":\"H265\",\"bitrate\":60000}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/vencattr", "channel=1", NULL);
    assert(strstr(response.body, "\"vcodec\":\"H265\"") != NULL);
    assert(strstr(response.body, "\"bitrate\":60000") != NULL);

    available_ops = app.backend.ops;
    app.backend.ops = ngcd_target_backend_ops();
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/audioctrl", NULL,
                        "{\"action\":\"input\",\"auto\":1}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/audioctrl", NULL,
                        "{\"action\":\"volume\",\"input\":0,\"value\":80}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/audioctrl", NULL,
                        "{\"action\":\"volume\",\"input\":0,\"value\":70}");
    assert(strstr(response.body, "\"code\":-1") != NULL);
    app.backend.ops = available_ops;

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/audioctrl", NULL,
                        "{\"action\":\"input\",\"auto\":0,\"input\":2}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/audioinfo", NULL, NULL);
    assert(strstr(response.body, "\"inputtype\":2") != NULL);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/lcd/backlight", NULL,
                        "{\"action\":\"set_brightness\",\"brightness\":87}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(app.backend.state.backlight == 87);
    assert(app.backend.state.backlight_saved == 87);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/lcd/backlight", NULL,
                        "{\"action\":\"turn_off\"}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(app.backend.state.backlight == 0);
    assert(app.backend.state.backlight_saved == 87);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/lcd/backlight", NULL,
                        "{\"action\":\"turn_on\"}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(app.backend.state.backlight == 87);

    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/wifi", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"essid\":\"CALF-MOCK\"") != NULL);
    assert(strstr(response.body, "\"level\":-55") != NULL);
    assert(strstr(response.body,
                  "\"usbnet\":{\"enabled\":0,\"configured\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/scanwifi", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"essid\":\"CALF-MOCK\"") != NULL);
    assert(strstr(response.body, "Lab \\\"Guest\\\"") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/wifi", NULL,
                        "{\"action\":\"setusbdc\","
                        "\"usb_port_name\":\"USB1\",\"os\":\"win\"}");
    assert(response.status == 200);
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/wifi", NULL, NULL);
    assert(strstr(response.body,
                  "\"usbnet\":{\"enabled\":1,\"configured\":1") != NULL);
    assert(strstr(response.body, "\"port\":\"USB1\"") != NULL);
    assert(strstr(response.body, "\"os\":\"win\"") != NULL);
    assert(strstr(response.body, "\"ipaddr\":\"192.168.2.101\"") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/wifi", NULL,
                        "{\"action\":\"setusbdc\","
                        "\"usb_port_name\":\"USB3\",\"os\":\"win\"}");
    assert(response.status == 400);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/wifi", NULL,
                        "{\"action\":\"setusbdc\","
                        "\"usb_port_name\":\"USB1\"}");
    assert(response.status == 400);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/wifi", NULL,
                        "{\"action\":\"closeusbdc\"}");
    assert(response.status == 200);
    assert(strstr(response.body, "\"code\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/wifi", NULL, NULL);
    assert(strstr(response.body,
                  "\"usbnet\":{\"enabled\":0,\"configured\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/systemstatus", NULL, "{\"ssids\":1}");
    assert(response.status == 200);
    assert(strstr(response.body, "\"ipaddr\":\"192.168.1.2\"") != NULL);
    assert(strstr(response.body, "\"is_usb_supply\":1") != NULL);
    assert(strstr(response.body, "\"batt_cap\":75") != NULL);
    assert(strstr(response.body, "\"sys_temp\":42") != NULL);
    assert(strstr(response.body, "\"core_temp\":48") != NULL);
    assert(strstr(response.body,
                  "\"eth0\":{\"ipaddr\":\"10.20.30.40\"}") != NULL);
    assert(strstr(response.body, "\"stor_loc\":\"/mnt/mmcblk1p1\"") != NULL);
    assert(strstr(response.body, "\"total_mb\":262144") != NULL);
    assert(strstr(response.body, "\"free_mb\":131072") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/ethaddr", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"ipaddr\":\"10.20.30.40\"") != NULL);

    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/mediastor", NULL,
                        "{\"action\":\"get_stor_info_act\"}");
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(strstr(response.body, "\"stor_loc\":\"/mnt/mmcblk1p1\"") != NULL);
    assert(strstr(response.body, "\"free_mb\":131072") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/mediastor", NULL,
                        "{\"action\":\"get_stor_infos\"}");
    assert(strstr(response.body, "\"stor_infos\":[{") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/mediastor", NULL,
                        "{\"action\":\"iotest_stor\",\"block_kb\":4,"
                        "\"count\":4}");
    assert(strstr(response.body, "\"kbps\":85000") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/mediastor", NULL,
                        "{\"action\":\"iotest_stor\",\"block_kb\":4096,"
                        "\"count\":17}");
    assert(strstr(response.body, "\"code\":-1") != NULL);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/media", "offset=-1&limit=64", NULL);
    assert(response.status == 400);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/media", "offset=0&limit=0", NULL);
    assert(response.status == 400);

    available_ops = app.backend.ops;
    unavailable_ops = *available_ops;
    unavailable_ops.storage_status = unavailable_storage_status;
    app.backend.ops = &unavailable_ops;
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/systemstatus", NULL, "{\"ssids\":4}");
    assert(strstr(response.body, "\"stor_loc\":\"\"") != NULL);
    assert(strstr(response.body, "\"total_mb\":0") != NULL);
    assert(strstr(response.body, "\"free_mb\":0") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/mediastor", NULL,
                        "{\"action\":\"get_stor_info_act\"}");
    assert(strstr(response.body, "\"code\":-1") != NULL);
    app.backend.ops = available_ops;

    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/exphist", NULL, NULL);
    assert(response.status == 200);
    assert(strstr(response.body, "\"code\":0") != NULL);
    assert(strstr(response.body,
                  "\"hist\":[1,2,3,4,5,6,7,8,9,10") != NULL);
    assert(strstr(response.body, ",63,64]") != NULL);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/exphist", NULL, "{}");
    assert(response.status == 405);
    response = dispatch(&app, NGCD_METHOD_GET, "/nope", NULL, NULL);
    assert(response.status == 404);
    response = dispatch(&app, NGCD_METHOD_GET,
                        "/camera/v2/poweroff", NULL, NULL);
    assert(response.status == 405 && !app.poweroff_requested);
    response = dispatch(&app, NGCD_METHOD_POST,
                        "/camera/v2/poweroff", NULL, NULL);
    assert(response.status == 200 && app.poweroff_requested);
    assert(strstr(response.body, "\"code\":0") != NULL);
    app.backend.ops->stop(&app.backend);
}

static void test_profile(void)
{
    static const char yaml[] =
        "config:\n"
        "  camera-mode: SBS_STITCH\n"
        "  isp-mode: 1\n"
        "  sensor:\n"
        "    - mode: 0\n"
        "      width: 3520\n"
        "      height: 3040\n"
        "      fps: 50\n"
        "    - mode: 0\n"
        "      width: 3520\n"
        "      height: 3040\n"
        "      fps: 50\n"
        "  vcap:\n"
        "    width: 3360\n"
        "    height: 2880\n"
        "    fps: 50\n"
        "  vout:\n"
        "    enable: 1\n"
        "    width: 3840\n"
        "    height: 2160\n"
        "    fps: 30\n"
        "  stitch:\n"
        "    mode: VR180\n"
        "    width: 6144\n"
        "    height: 3072\n"
        "    fovx: 36000\n"
        "    fovy: 18000\n"
        "  venc:\n"
        "    - vcodec: H264\n"
        "      rcmode: CBR\n"
        "      profile: HIGH\n"
        "      width: 6144\n"
        "      height: 3072\n"
        "      fps: 50\n"
        "      bitrate: 100000\n"
        "      gop: 50\n"
        "      mask: 1\n";
    struct ngcd_profile profile;
    char error[128];
    assert(ngcd_profile_parse(yaml, sizeof(yaml) - 1, &profile,
                              error, sizeof(error)) == 0);
    assert(strcmp(profile.camera_mode, "SBS_STITCH") == 0);
    assert(profile.sensor_count == 2);
    assert(profile.sensor[1].fps == 50);
    assert(profile.capture_count == 1);
    assert(profile.stitch.width == 6144);
    assert(profile.encoder_count == 1);
    assert(profile.encoder_mask[0]);
    assert(ngcd_profile_parse("config:\n  camera-mode: X\n", 25,
                              &profile, error, sizeof(error)) != 0);
}

int main(void)
{
    test_json();
    test_wifi_parsers();
    test_usb_ethernet_mapping();
    test_power_parser();
    test_stock_session_marker();
    test_storage_mount_parser();
    test_storage_io_file();
    test_storage_media_paths();
    test_rockchip_acp_controls();
    test_profile();
    test_contract();
    puts("ngcd unit tests passed");
    return 0;
}
