#include "ngcd_rk.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ngcd_rk_camgroup_config {
    const char *sensor_entity_name[8];
    int sensor_count;
    int padding;
    const char *config_file_directory;
    const char *single_iq_file;
    const char *group_iq_file;
    const char *overlap_map_file;
    void *hardware_event_callback;
    void *hardware_event_context;
};

_Static_assert(sizeof(struct ngcd_rk_camgroup_config) == 120U,
               "unexpected Rockchip camera-group configuration ABI");

struct ngcd_rk_target {
    void *rockit;
    void *rkaiq;
    void *aiq_group_context;
    void *aiq_group_sensor[2];
    unsigned int aiq_group_users;
    void *aiq_offline_sensor[2];
    struct ngcd_rk_aiq_wb_gain aiq_offline_white_balance[2];
    bool aiq_offline_started[2];

    int (*sys_init)(void);
    int (*sys_exit)(void);
    int (*sys_bind)(const void *, const void *);
    int (*sys_unbind)(const void *, const void *);

    int (*vi_get_dev_attr)(int, void *);
    int (*vi_set_dev_attr)(int, const void *);
    int (*vi_get_dev_enabled)(int);
    int (*vi_enable_dev)(int);
    int (*vi_disable_dev)(int);
    int (*vi_bind_pipe)(int, const void *);
    int (*vi_set_channel_attr)(int, int, const void *);
    int (*vi_enable_channel)(int, int);
    int (*vi_disable_channel)(int, int);

    int (*vpss_create_group)(int, const void *);
    int (*vpss_destroy_group)(int);
    int (*vpss_set_device)(int, int);
    int (*vpss_enable_backup)(int);
    int (*vpss_start_group)(int);
    int (*vpss_stop_group)(int);
    int (*vpss_set_channel_attr)(int, int, const void *);
    int (*vpss_enable_channel)(int, int);
    int (*vpss_disable_channel)(int, int);

    int (*vo_set_pub_attr)(int, const void *);
    int (*vo_enable)(int);
    int (*vo_disable)(int);
    int (*vo_bind_layer)(int, int, int);
    int (*vo_unbind_layer)(int, int);
    int (*vo_set_layer_buffer_length)(int, int);
    int (*vo_set_layer_attr)(int, const void *);
    int (*vo_set_layer_splice_mode)(int, int);
    int (*vo_enable_layer)(int);
    int (*vo_disable_layer)(int);
    int (*vo_set_channel_attr)(int, int, const void *);
    int (*vo_enable_channel)(int, int);
    int (*vo_disable_channel)(int, int);
    int (*vo_send_frame)(int, int, const void *, int);
    int (*vo_set_wbc_source)(int, const void *);
    int (*vo_set_wbc_attr)(int, const void *);
    int (*vo_enable_wbc)(int);
    int (*vo_disable_wbc)(int);
    int (*vo_create_graphics_buffer)(int, int, int, void **);
    int (*vo_destroy_graphics_buffer)(void *);

    int (*mmz_alloc)(void **, const char *, const char *, unsigned int);
    int (*mmz_free)(void *);
    int (*sys_create_mb)(void **, const void *);
    int (*mb_release)(void *);
    int (*mb_handle_to_id)(void *);
    void *(*mb_handle_to_address)(void *);
    size_t (*mb_get_size)(void *);

    int (*avs_set_mod_param)(const void *);
    int (*avs_create_group)(int, const void *);
    int (*avs_destroy_group)(int);
    int (*avs_start_group)(int);
    int (*avs_stop_group)(int);
    int (*avs_set_channel_attr)(int, int, const void *);
    int (*avs_enable_channel)(int, int);
    int (*avs_disable_channel)(int, int);
    int (*avs_get_channel_frame)(int, int, void *, int);
    int (*avs_release_channel_frame)(int, int, const void *);
    int (*vpss_get_channel_frame)(int, int, void *, int);
    int (*vpss_release_channel_frame)(int, int, const void *);

    int (*venc_create_channel)(int, const void *);
    int (*venc_destroy_channel)(int);
    int (*venc_set_rc_param)(int, const void *);
    int (*venc_start_receive)(int, const void *);
    int (*venc_stop_receive)(int);
    int (*venc_get_stream)(int, void *, int);
    int (*venc_release_stream)(int, const void *);
    int (*venc_request_idr)(int, int);
    int (*venc_get_h264_vui)(int, void *);
    int (*venc_set_h264_vui)(int, const void *);
    int (*venc_get_h265_vui)(int, void *);
    int (*venc_set_h265_vui)(int, const void *);
    int (*venc_get_jpeg_param)(int, void *);
    int (*venc_set_jpeg_param)(int, const void *);
    int (*venc_send_frame)(int, const void *, int);

    int (*vdec_create_channel)(int, const void *);
    int (*vdec_destroy_channel)(int);
    int (*vdec_start_receive)(int);
    int (*vdec_stop_receive)(int);
    int (*vdec_reset_channel)(int);
    int (*vdec_send_stream)(int, const void *, int);
    int (*vdec_query_status)(int, void *);
    int (*vdec_set_channel_param)(int, const void *);
    int (*vdec_set_display_mode)(int, int);

    int (*ai_set_pub_attr)(int, const void *);
    int (*ai_enable)(int);
    int (*ai_disable)(int);
    int (*ai_enable_channel)(int, int);
    int (*ai_disable_channel)(int, int);
    int (*ai_set_channel_param)(int, int, const void *);
    int (*ai_enable_resample)(int, int, int);
    int (*ai_disable_resample)(int, int);
    int (*ai_get_frame)(int, int, void *, void *, int);
    int (*ai_release_frame)(int, int, const void *, const void *);

    int (*ao_clear_pub_attr)(int);
    int (*ao_set_pub_attr)(int, const void *);
    int (*ao_enable)(int);
    int (*ao_disable)(int);
    int (*ao_enable_channel)(int, int);
    int (*ao_disable_channel)(int, int);
    int (*ao_set_channel_param)(int, int, const void *);
    int (*ao_enable_resample)(int, int, int);
    int (*ao_disable_resample)(int, int);
    int (*ao_send_frame)(int, int, const void *, int);
    int (*ao_wait_eos)(int, int, int);
    int (*ao_set_volume)(int, int);
    int (*ao_get_volume)(int, int *);

    int (*aiq_preinit_scene)(const char *, const char *, const char *);
    int (*aiq_enum_static_metas)(int, void *);
    void *(*aiq_init)(const char *, const char *, void *, void *);
    int (*aiq_prepare)(void *, int, int, int);
    int (*aiq_start)(void *);
    int (*aiq_stop)(void *, int);
    void (*aiq_deinit)(void *);
    void *(*aiq_group_create)(struct ngcd_rk_camgroup_config *);
    int (*aiq_group_prepare)(void *, int);
    int (*aiq_group_start)(void *);
    int (*aiq_group_stop)(void *);
    int (*aiq_group_destroy)(void *);
    void *(*aiq_group_get_sensor)(void *, const char *);
    int (*aiq_get_acp)(void *, struct ngcd_rk_aiq_acp_attr *);
    int (*aiq_set_acp)(void *, struct ngcd_rk_aiq_acp_attr);
    int (*aiq_get_sharpness)(void *, unsigned int *);
    int (*aiq_set_sharpness)(void *, unsigned int);
    int (*aiq_get_anr)(void *, unsigned int *);
    int (*aiq_set_anr)(void *, unsigned int);
    int (*aiq_get_spatial_nr)(void *, int, unsigned int *);
    int (*aiq_set_spatial_nr)(void *, int, unsigned int);
    int (*aiq_get_temporal_nr)(void *, int, unsigned int *);
    int (*aiq_set_temporal_nr)(void *, int, unsigned int);
    int (*aiq_get_exposure)(void *, struct ngcd_rk_aiq_exp_sw_attr *);
    int (*aiq_set_exposure)(void *, const struct ngcd_rk_aiq_exp_sw_attr *);
    int (*aiq_get_linear_exposure)(void *,
                                   struct ngcd_rk_aiq_lin_exp_attr *);
    int (*aiq_set_linear_exposure)(
        void *, const struct ngcd_rk_aiq_lin_exp_attr *);
    int (*aiq_query_exposure)(void *, struct ngcd_rk_aiq_exp_query_info *);
    int (*aiq_get_white_balance_mode)(void *, unsigned int *);
    int (*aiq_set_white_balance_mode)(void *, unsigned int);
    int (*aiq_get_white_balance_ct)(void *, unsigned int *);
    int (*aiq_set_white_balance_ct)(void *, unsigned int);
    int (*aiq_get_white_balance_gain)(void *, struct ngcd_rk_aiq_wb_gain *);
    int (*aiq_set_white_balance_gain)(void *, struct ngcd_rk_aiq_wb_gain *);
    int (*aiq_get_flicker_enabled)(void *, unsigned char *);
    int (*aiq_set_flicker_enabled)(void *, unsigned char);
    int (*aiq_get_flicker_mode)(void *, unsigned int *);
    int (*aiq_set_flicker_mode)(void *, unsigned int);
    int (*aiq_get_power_line_frequency)(void *, unsigned int *);
    int (*aiq_set_power_line_frequency)(void *, unsigned int);
    int (*aiq_get_effect)(void *, struct ngcd_rk_aiq_effect_attr *);
    int (*aiq_set_effect)(void *, struct ngcd_rk_aiq_effect_attr);
    int (*aiq_capture_raw)(void *, int, int, const char *, char *);
    int (*aiq_prepare_raw)(void *, struct ngcd_rk_raw_prop);
    int (*aiq_enqueue_raw)(void *, void *, bool);
    int (*aiq_enqueue_raw_file)(void *, const char *);
};

static int target_set_second_cif_link(bool enabled)
{
    const char *command = enabled
        ? "/usr/bin/media-ctl -d /dev/media3 -l "
          "'\"rkcif-mipi-lvds4\":0 -> \"rkisp-isp-subdev\":0 [1]'"
        : "/usr/bin/media-ctl -d /dev/media3 -l "
          "'\"rkcif-mipi-lvds4\":0 -> \"rkisp-isp-subdev\":0 [0]'";
    int result = system(command);
    if (result != 0)
        fprintf(stderr, "ngcd: media3 CIF link %s failed (status=%d)\n",
                enabled ? "restore" : "disable", result);
    return result == 0 ? 0 : -1;
}

static int load_symbol(void *library, const char *name, void *output,
                       size_t output_size)
{
    void *address;
    (void)dlerror();
    address = dlsym(library, name);
    if (address == NULL || dlerror() != NULL || output_size != sizeof(address))
        return -1;
    memcpy(output, &address, output_size);
    return 0;
}

#define LOAD_FROM(target, library, member, symbol)                            \
    do {                                                                      \
        if (load_symbol((library), (symbol), &(target)->member,                \
                        sizeof((target)->member)) != 0)                        \
            goto fail;                                                        \
    } while (0)

static int target_system_init(void *opaque)
{
    return ((struct ngcd_rk_target *)opaque)->sys_init();
}

static int target_system_exit(void *opaque)
{
    return ((struct ngcd_rk_target *)opaque)->sys_exit();
}

static int target_bind(void *opaque, const struct ngcd_rk_channel *source,
                       const struct ngcd_rk_channel *destination)
{
    return ((struct ngcd_rk_target *)opaque)->sys_bind(source, destination);
}

static int target_unbind(void *opaque, const struct ngcd_rk_channel *source,
                         const struct ngcd_rk_channel *destination)
{
    return ((struct ngcd_rk_target *)opaque)->sys_unbind(source, destination);
}

struct sensor_format_name {
    uint32_t code;
    const char *name;
};

static const struct sensor_format_name SENSOR_FORMAT_NAMES[] = {
    {0x3007U, "SBGGR10_1X10"},
    {0x300eU, "SGBRG10_1X10"},
    {0x300aU, "SGRBG10_1X10"},
    {0x300fU, "SRGGB10_1X10"},
    {0x3008U, "SBGGR12_1X12"},
    {0x3010U, "SGBRG12_1X12"},
    {0x3011U, "SGRBG12_1X12"},
    {0x3012U, "SRGGB12_1X12"},
};

static uint32_t read_u32(const unsigned char *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static int valid_sensor_name(const char *name)
{
    size_t index;
    for (index = 0; index < 32 && name[index] != '\0'; ++index) {
        unsigned char value = (unsigned char)name[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == ' ' ||
              value == '_' || value == '-' || value == '.'))
            return 0;
    }
    return index > 0 && index < 32;
}

static const char *sensor_format_name(uint32_t code)
{
    size_t index;
    for (index = 0;
         index < sizeof(SENSOR_FORMAT_NAMES) / sizeof(SENSOR_FORMAT_NAMES[0]);
         ++index)
        if (SENSOR_FORMAT_NAMES[index].code == code)
            return SENSOR_FORMAT_NAMES[index].name;
    return NULL;
}

static int target_configure_sensor(struct ngcd_rk_target *target, int sensor,
                                   int width, int height, int fps,
                                   int crop_width, int crop_height)
{
    unsigned char metadata[320];
    char command[512];
    const char *format_name = NULL;
    int format_count;
    int media_index;
    int selected = -1;
    int index;
    int count;

    memset(metadata, 0, sizeof(metadata));
    if (target->aiq_enum_static_metas(sensor, metadata) < 0)
        return -1;
    metadata[31] = '\0';
    if (!valid_sensor_name((const char *)metadata))
        return -1;
    format_count = (int)read_u32(metadata, 232);
    media_index = (int)read_u32(metadata, 240);
    if (format_count < 0 || format_count > 10 || media_index < 0 ||
        media_index > 63)
        return -1;
    for (index = 0; index < format_count; ++index) {
        size_t offset = 32U + (size_t)index * 20U;
        if ((int)read_u32(metadata, offset) == width &&
            (int)read_u32(metadata, offset + 4U) == height &&
            (int)read_u32(metadata, offset + 12U) >= fps) {
            selected = index;
            format_name = sensor_format_name(read_u32(metadata, offset + 8U));
            break;
        }
    }
    if (selected < 0 || format_name == NULL)
        return -1;
    count = snprintf(command, sizeof(command),
                     "media-ctl -d /dev/media%d --set-v4l2 "
                     "'\"%s\":0[fmt:%s/%dx%d@100/%d]'",
                     media_index, (const char *)metadata, format_name,
                     width, height, fps * 100);
    if (count < 0 || (size_t)count >= sizeof(command) || system(command) != 0)
        return -1;
    if (crop_width != width || crop_height != height) {
        int left = (width - crop_width) / 2;
        int top = (height - crop_height) / 2;
        /* The calibrated IMX577 pair is intentionally not center-cropped.
         * These are the offsets selected by the stock 2.1.6 photo graph and
         * expected by the shipped VR180 calibration data. */
        if (width == 4048 && height == 3040 && crop_width == 3520 &&
            crop_height == 2880) {
            left = sensor == 0 ? 244 : 284;
            top = sensor == 0 ? 104 : 70;
        }
        if (crop_width <= 0 || crop_height <= 0 || left < 0 || top < 0)
            return -1;
        count = snprintf(command, sizeof(command),
                         "media-ctl -d /dev/media%d --set-v4l2 "
                         "'\"%s\":0[crop:(%d,%d)/%dx%d]'",
                         media_index, (const char *)metadata, left, top,
                         crop_width, crop_height);
        if (count < 0 || (size_t)count >= sizeof(command) ||
            system(command) != 0)
            return -1;
    }
    return 0;
}

static int sensor_ioctl_u32(int descriptor, unsigned long request,
                            uint32_t value);

static int sensor_set_sync_mode(int sensor,
                                enum ngcd_rk_sensor_sync_mode sync_mode)
{
    static const char *const paths[] = {
        "/dev/v4l-subdev2", "/dev/v4l-subdev7"
    };
    enum { RKMODULE_SET_SYNC_MODE = 0x400456d6UL };
    int descriptor;
    int result;

    if (sensor < 0 || sensor >= 2)
        return -1;
    descriptor = open(paths[sensor], O_RDWR | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    result = sensor_ioctl_u32(descriptor, RKMODULE_SET_SYNC_MODE,
                             (uint32_t)sync_mode);
    (void)close(descriptor);
    return result;
}

static void target_sensor_group_destroy(struct ngcd_rk_target *target)
{
    if (target->aiq_group_context != NULL) {
        (void)target->aiq_group_stop(target->aiq_group_context);
        (void)target->aiq_group_destroy(target->aiq_group_context);
    }
    target->aiq_group_context = NULL;
    target->aiq_group_sensor[0] = NULL;
    target->aiq_group_sensor[1] = NULL;
    target->aiq_group_users = 0U;
}

static int target_sensor_group_start(struct ngcd_rk_target *target,
                                     int width, int height, int fps,
                                     int crop_width, int crop_height)
{
    static const char *const names[] = {
        "m00_imx577 2-001a", "m01_imx577 3-001a"
    };
    struct ngcd_rk_camgroup_config config;
    int sensor;
    if (target->aiq_group_context != NULL)
        return 0;
    for (sensor = 0; sensor < 2; ++sensor) {
        enum ngcd_rk_sensor_sync_mode sync_mode =
            sensor == 0 ? NGCD_RK_SENSOR_INTERNAL_MASTER :
                          NGCD_RK_SENSOR_EXTERNAL_MASTER;
        if (target_configure_sensor(target, sensor, width, height, fps,
                                    crop_width, crop_height) != 0 ||
            target->aiq_preinit_scene(names[sensor], "normal", "day") < 0 ||
            sensor_set_sync_mode(sensor, sync_mode) != 0)
            return -1;
    }
    memset(&config, 0, sizeof(config));
    config.sensor_entity_name[0] = names[0];
    config.sensor_entity_name[1] = names[1];
    config.sensor_count = 2;
    config.config_file_directory = "/app/data";
    target->aiq_group_context = target->aiq_group_create(&config);
    if (target->aiq_group_context == NULL)
        return -1;
    if (target->aiq_group_prepare(target->aiq_group_context, 0) < 0 ||
        target->aiq_group_start(target->aiq_group_context) < 0)
        goto fail;
    for (sensor = 0; sensor < 2; ++sensor) {
        target->aiq_group_sensor[sensor] = target->aiq_group_get_sensor(
            target->aiq_group_context, names[sensor]);
        if (target->aiq_group_sensor[sensor] == NULL)
            goto fail;
    }
    fprintf(stderr, "ngcd: Rockchip stereo camera group started\n");
    return 0;
fail:
    target_sensor_group_destroy(target);
    return -1;
}

static int target_sensor_start(void *opaque, int sensor, int width, int height,
                               int fps, int crop_width, int crop_height,
                               enum ngcd_rk_sensor_sync_mode sync_mode,
                               void **handle)
{
    static const char *const names[] = {
        "m00_imx577 2-001a", "m01_imx577 3-001a"
    };
    struct ngcd_rk_target *target = opaque;
    void *context;
    if (sensor < 0 || sensor >= 2 || handle == NULL)
        return -1;
    if (sync_mode != NGCD_RK_SENSOR_NO_SYNC) {
        if (target_sensor_group_start(target, width, height, fps, crop_width,
                                      crop_height) != 0)
            return -1;
        *handle = target->aiq_group_sensor[sensor];
        ++target->aiq_group_users;
        return 0;
    }
    if (target_configure_sensor(target, sensor, width, height, fps,
                                crop_width, crop_height) != 0)
        return -1;
    if (target->aiq_preinit_scene(names[sensor], "normal", "day") < 0)
        return -1;
    context = target->aiq_init(names[sensor], "/app/data", NULL, NULL);
    if (context == NULL)
        return -1;
    if (target->aiq_prepare(context, width, height, 0) < 0 ||
        sensor_set_sync_mode(sensor, sync_mode) != 0 ||
        target->aiq_start(context) < 0) {
        (void)target->aiq_stop(context, 0);
        target->aiq_deinit(context);
        return -1;
    }
    *handle = context;
    return 0;
}

static int target_offline_sensor_start(
    void *opaque, int sensor, int width, int height, uint32_t format,
    const struct ngcd_rk_aiq_wb_gain *white_balance, void **handle)
{
    static const char *const names[] = {
        "m00_imx577 2-001a", "m01_imx577 3-001a"
    };
    struct ngcd_rk_target *target = opaque;
    struct ngcd_rk_raw_prop property;
    const char *previous;
    char *saved = NULL;
    void *context = NULL;
    int result = -1;
    if (sensor < 0 || sensor >= 2 || width <= 0 || height <= 0 ||
        format == 0U || white_balance == NULL || handle == NULL)
        return -1;
    previous = getenv("USE_AS_FAKE_CAM");
    if (previous != NULL) {
        saved = malloc(strlen(previous) + 1U);
        if (saved == NULL)
            return -1;
        memcpy(saved, previous, strlen(previous) + 1U);
    }
    if (setenv("USE_AS_FAKE_CAM", "1", 1) != 0)
        goto finished;
    if (target->aiq_preinit_scene(names[sensor], "normal", "day") < 0)
        goto finished;
    context = target->aiq_init(names[sensor], "/app/data", NULL, NULL);
    if (context == NULL)
        goto finished;
    memset(&property, 0, sizeof(property));
    property.width = (uint32_t)width;
    property.height = (uint32_t)height;
    property.format = format;
    property.buffer_type = 3U; /* RK_AIQ_RAW_FILE */
    if (target->aiq_prepare_raw(context, property) < 0 ||
        target->aiq_prepare(context, width, height, 0) < 0)
        goto finished;
    /* Rockchip's v3.0x8.8 FakeCamHw only searches rkcif-mipi-lvds0..3
     * when it switches an ISP to raw readback.  This board routes the second
     * IMX577 through rkcif-mipi-lvds4, leaving both CIF and rawrd2 linked to
     * the ISP sink.  Correct that vendor-library omission before streaming. */
    if (sensor == 1 && target_set_second_cif_link(false) != 0)
        goto finished;
    *handle = context;
    target->aiq_offline_sensor[sensor] = context;
    target->aiq_offline_white_balance[sensor] = *white_balance;
    target->aiq_offline_started[sensor] = false;
    context = NULL;
    result = 0;
finished:
    if (context != NULL) {
        (void)target->aiq_stop(context, 0);
        target->aiq_deinit(context);
        if (sensor == 1)
            (void)target_set_second_cif_link(true);
    }
    if (saved != NULL)
        (void)setenv("USE_AS_FAKE_CAM", saved, 1);
    else
        (void)unsetenv("USE_AS_FAKE_CAM");
    free(saved);
    return result;
}

static int target_offline_sensor_run(void *opaque, void *handle)
{
    struct ngcd_rk_target *target = opaque;
    int sensor;
    if (handle == NULL)
        return -1;
    if (handle == target->aiq_offline_sensor[0])
        sensor = 0;
    else if (handle == target->aiq_offline_sensor[1])
        sensor = 1;
    else
        return -1;
    if (target->aiq_start(handle) < 0)
        return -1;
    target->aiq_offline_started[sensor] = true;
    return target->aiq_set_white_balance_gain(
        handle, &target->aiq_offline_white_balance[sensor]);
}

static int target_offline_sensor_enqueue(void *opaque, void *handle,
                                         void *raw_data)
{
    struct ngcd_rk_target *target = opaque;
    if (handle == NULL || raw_data == NULL)
        return -1;
    return target->aiq_enqueue_raw_file(handle, raw_data);
}

static void target_sensor_stop(void *opaque, void *handle)
{
    struct ngcd_rk_target *target = opaque;
    int offline_sensor = -1;
    if (handle != NULL && target->aiq_group_context != NULL &&
        (handle == target->aiq_group_sensor[0] ||
         handle == target->aiq_group_sensor[1])) {
        if (target->aiq_group_users > 0U)
            --target->aiq_group_users;
        if (target->aiq_group_users == 0U)
            target_sensor_group_destroy(target);
        return;
    }
    if (handle != NULL) {
        if (handle == target->aiq_offline_sensor[0])
            offline_sensor = 0;
        else if (handle == target->aiq_offline_sensor[1])
            offline_sensor = 1;
        if (offline_sensor < 0 || target->aiq_offline_started[offline_sensor])
            (void)target->aiq_stop(handle, 0);
        target->aiq_deinit(handle);
        if (offline_sensor >= 0) {
            target->aiq_offline_sensor[offline_sensor] = NULL;
            target->aiq_offline_started[offline_sensor] = false;
        }
        if (offline_sensor == 1)
            (void)target_set_second_cif_link(true);
    }
}

static int sensor_ioctl_u32(int descriptor, unsigned long request,
                            uint32_t value)
{
    return ioctl(descriptor, request, &value);
}

static int sensor_sync_pause(void)
{
    const struct timespec duration = {0, 20000000L};
    return nanosleep(&duration, NULL);
}

static int target_sensor_synchronize(void *opaque)
{
    enum {
        RKMODULE_SET_QUICK_STREAM = 0x400456caUL,
        RKMODULE_SET_SYNC_MODE = 0x400456d6UL,
        EXTERNAL_MASTER_MODE = 1,
        INTERNAL_MASTER_MODE = 2,
    };
    int internal_descriptor;
    int external_descriptor;
    int result = -1;
    struct ngcd_rk_target *target = opaque;
    if (target->aiq_group_context != NULL)
        return 0;
    internal_descriptor = open("/dev/v4l-subdev2", O_RDWR | O_CLOEXEC);
    if (internal_descriptor < 0)
        return -1;
    external_descriptor = open("/dev/v4l-subdev7", O_RDWR | O_CLOEXEC);
    if (external_descriptor < 0)
        goto done;
    if (sensor_ioctl_u32(internal_descriptor, RKMODULE_SET_SYNC_MODE,
                         INTERNAL_MASTER_MODE) != 0 ||
        sensor_ioctl_u32(external_descriptor, RKMODULE_SET_SYNC_MODE,
                         EXTERNAL_MASTER_MODE) != 0 ||
        sensor_ioctl_u32(internal_descriptor, RKMODULE_SET_QUICK_STREAM, 0U) !=
            0 ||
        sensor_sync_pause() != 0 ||
        sensor_ioctl_u32(external_descriptor, RKMODULE_SET_QUICK_STREAM, 0U) !=
            0 ||
        sensor_sync_pause() != 0 ||
        sensor_ioctl_u32(external_descriptor, RKMODULE_SET_QUICK_STREAM, 1U) !=
            0 ||
        sensor_sync_pause() != 0 ||
        sensor_ioctl_u32(internal_descriptor, RKMODULE_SET_QUICK_STREAM, 1U) !=
            0)
        goto close_external;
    result = 0;
close_external:
    (void)close(external_descriptor);
done:
    (void)close(internal_descriptor);
    return result;
}

static int target_aiq_get_acp(void *opaque, void *sensor_context,
                              struct ngcd_rk_aiq_acp_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_acp(sensor_context, attribute);
}

static int target_aiq_set_acp(void *opaque, void *sensor_context,
                              const struct ngcd_rk_aiq_acp_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    struct ngcd_rk_aiq_acp_attr value;
    memcpy(&value, attribute, sizeof(value));
    return target->aiq_set_acp(sensor_context, value);
}

#define TARGET_AIQ_STRENGTH_WRAPPERS(name)                                   \
    static int target_aiq_get_##name(void *opaque, void *sensor_context,     \
                                     unsigned int *strength)                  \
    {                                                                         \
        struct ngcd_rk_target *target = opaque;                               \
        return target->aiq_get_##name(sensor_context, strength);              \
    }                                                                         \
    static int target_aiq_set_##name(void *opaque, void *sensor_context,     \
                                     unsigned int strength)                   \
    {                                                                         \
        struct ngcd_rk_target *target = opaque;                               \
        return target->aiq_set_##name(sensor_context, strength);              \
    }

TARGET_AIQ_STRENGTH_WRAPPERS(sharpness)
TARGET_AIQ_STRENGTH_WRAPPERS(anr)

static int target_aiq_get_spatial_nr(void *opaque, void *sensor_context,
                                     unsigned int *strength)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_spatial_nr(sensor_context, 1, strength);
}

static int target_aiq_set_spatial_nr(void *opaque, void *sensor_context,
                                     unsigned int strength)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_set_spatial_nr(sensor_context, 1, strength);
}

static int target_aiq_get_temporal_nr(void *opaque, void *sensor_context,
                                      unsigned int *strength)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_temporal_nr(sensor_context, 1, strength);
}

static int target_aiq_set_temporal_nr(void *opaque, void *sensor_context,
                                      unsigned int strength)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_set_temporal_nr(sensor_context, 1, strength);
}

static int target_aiq_get_exposure(
    void *opaque, void *sensor_context,
    struct ngcd_rk_aiq_exp_sw_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_exposure(sensor_context, attribute);
}

static int target_aiq_set_exposure(
    void *opaque, void *sensor_context,
    const struct ngcd_rk_aiq_exp_sw_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    /* This firmware's camera-group AE implementation keeps group state and
     * child pipeline state separately.  Updating a child alone is accepted
     * by the attribute getter but AUTO continues driving the running group.
     * Mirror the sequence established by the hardware diagnostic:
     * update the group AE handle, then the addressed physical child. */
    if (target->aiq_group_context != NULL &&
        target->aiq_set_exposure(target->aiq_group_context, attribute) != 0)
        return -1;
    return target->aiq_set_exposure(sensor_context, attribute);
}

static int target_aiq_get_linear_exposure(
    void *opaque, void *sensor_context,
    struct ngcd_rk_aiq_lin_exp_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_linear_exposure(sensor_context, attribute);
}

static int target_aiq_set_linear_exposure(
    void *opaque, void *sensor_context,
    const struct ngcd_rk_aiq_lin_exp_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_set_linear_exposure(sensor_context, attribute);
}

static int target_aiq_query_exposure(
    void *opaque, void *sensor_context,
    struct ngcd_rk_aiq_exp_query_info *information)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_query_exposure(sensor_context, information);
}

static int target_aiq_get_white_balance_mode(void *opaque,
                                             void *sensor_context,
                                             unsigned int *mode)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_white_balance_mode(sensor_context, mode);
}

static int target_aiq_set_white_balance_mode(void *opaque,
                                             void *sensor_context,
                                             unsigned int mode)
{
    struct ngcd_rk_target *target = opaque;
    /* Like AE, the stereo camera group owns live AWB state independently of
     * its child contexts.  Updating only a child can read back briefly and is
     * then overwritten by the group, especially after an offline Night graph
     * has been replaced.  Keep the group and addressed child in one state. */
    if (target->aiq_group_context != NULL &&
        target->aiq_set_white_balance_mode(
            target->aiq_group_context, mode) != 0)
        return -1;
    return target->aiq_set_white_balance_mode(sensor_context, mode);
}

static int target_aiq_get_white_balance_ct(void *opaque,
                                           void *sensor_context,
                                           unsigned int *kelvin)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_white_balance_ct(sensor_context, kelvin);
}

static int target_aiq_set_white_balance_ct(void *opaque,
                                           void *sensor_context,
                                           unsigned int kelvin)
{
    struct ngcd_rk_target *target = opaque;
    if (target->aiq_group_context != NULL &&
        target->aiq_set_white_balance_ct(
            target->aiq_group_context, kelvin) != 0)
        return -1;
    return target->aiq_set_white_balance_ct(sensor_context, kelvin);
}

TARGET_AIQ_STRENGTH_WRAPPERS(flicker_mode)
TARGET_AIQ_STRENGTH_WRAPPERS(power_line_frequency)

static int target_aiq_get_white_balance_gain(
    void *opaque, void *sensor_context, struct ngcd_rk_aiq_wb_gain *gain)
{
    struct ngcd_rk_target *target = opaque;
    if (sensor_context == NULL || gain == NULL)
        return -1;
    return target->aiq_get_white_balance_gain(sensor_context, gain);
}

static int target_aiq_get_flicker_enabled(void *opaque, void *sensor_context,
                                          unsigned char *enabled)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_flicker_enabled(sensor_context, enabled);
}

static int target_aiq_set_flicker_enabled(void *opaque, void *sensor_context,
                                          unsigned char enabled)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_set_flicker_enabled(sensor_context, enabled);
}

static int target_aiq_get_effect(void *opaque, void *sensor_context,
                                 struct ngcd_rk_aiq_effect_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    return target->aiq_get_effect(sensor_context, attribute);
}

static int target_aiq_set_effect(
    void *opaque, void *sensor_context,
    const struct ngcd_rk_aiq_effect_attr *attribute)
{
    struct ngcd_rk_target *target = opaque;
    struct ngcd_rk_aiq_effect_attr value;
    memcpy(&value, attribute, sizeof(value));
    return target->aiq_set_effect(sensor_context, value);
}

static int target_aiq_capture_raw(void *opaque, void *sensor_context,
                                  int count, const char *capture_directory,
                                  char *output_directory)
{
    struct ngcd_rk_target *target = opaque;
    enum { CAPTURE_RAW_SYNC = 1 };
    if (sensor_context == NULL || count <= 0 || capture_directory == NULL ||
        output_directory == NULL)
        return -1;
    return target->aiq_capture_raw(sensor_context, CAPTURE_RAW_SYNC, count,
                                   capture_directory, output_directory);
}

static int target_prepare_directory(void *opaque, const char *path)
{
    (void)opaque;
    if (mkdir(path, 0775) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int target_wait_output(void *opaque, bool stitched, int device,
                              int channel, int timeout_ms)
{
    struct ngcd_rk_target *target = opaque;
    unsigned char frame[160];
    int result;
    memset(frame, 0, sizeof(frame));
    if (stitched) {
        result = target->avs_get_channel_frame(device, channel, frame,
                                                timeout_ms);
        if (result == 0)
            result = target->avs_release_channel_frame(device, channel,
                                                        frame);
    } else {
        result = target->vpss_get_channel_frame(device, channel, frame,
                                                 timeout_ms);
        if (result == 0)
            result = target->vpss_release_channel_frame(device, channel,
                                                         frame);
    }
    /* A zero-timeout call is also the nonblocking queue-drain primitive. */
    if (result != 0 && timeout_ms != 0)
        fprintf(stderr, "ngcd: %s output wait failed: 0x%x\n",
                stitched ? "AVS" : "VPSS", (unsigned int)result);
    return result;
}

#define WRAP_ONE(name, member)                                                 \
    static int name(void *opaque, int first)                                  \
    {                                                                          \
        return ((struct ngcd_rk_target *)opaque)->member(first);               \
    }

#define WRAP_TWO(name, member)                                                 \
    static int name(void *opaque, int first, int second)                       \
    {                                                                          \
        return ((struct ngcd_rk_target *)opaque)->member(first, second);        \
    }

#define WRAP_ONE_PTR(name, member, qualifier)                                  \
    static int name(void *opaque, int first, qualifier void *pointer)          \
    {                                                                          \
        return ((struct ngcd_rk_target *)opaque)->member(first, pointer);       \
    }

#define WRAP_TWO_PTR(name, member, qualifier)                                  \
    static int name(void *opaque, int first, int second,                       \
                    qualifier void *pointer)                                   \
    {                                                                          \
        return ((struct ngcd_rk_target *)opaque)->member(first, second,         \
                                                          pointer);             \
    }

WRAP_ONE_PTR(target_vi_get_dev_attr, vi_get_dev_attr, )
WRAP_ONE_PTR(target_vi_set_dev_attr, vi_set_dev_attr, const)
WRAP_ONE(target_vi_get_dev_enabled, vi_get_dev_enabled)
WRAP_ONE(target_vi_enable_dev, vi_enable_dev)
WRAP_ONE(target_vi_disable_dev, vi_disable_dev)
WRAP_ONE_PTR(target_vi_bind_pipe, vi_bind_pipe, const)
WRAP_TWO_PTR(target_vi_set_channel_attr, vi_set_channel_attr, const)
WRAP_TWO(target_vi_enable_channel, vi_enable_channel)
WRAP_TWO(target_vi_disable_channel, vi_disable_channel)
WRAP_ONE_PTR(target_vpss_create_group, vpss_create_group, const)
WRAP_ONE(target_vpss_destroy_group, vpss_destroy_group)
WRAP_TWO(target_vpss_set_device, vpss_set_device)
WRAP_ONE(target_vpss_enable_backup, vpss_enable_backup)
WRAP_ONE(target_vpss_start_group, vpss_start_group)
WRAP_ONE(target_vpss_stop_group, vpss_stop_group)
WRAP_TWO_PTR(target_vpss_set_channel_attr, vpss_set_channel_attr, const)
WRAP_TWO(target_vpss_enable_channel, vpss_enable_channel)
WRAP_TWO(target_vpss_disable_channel, vpss_disable_channel)
WRAP_ONE_PTR(target_vo_set_pub_attr, vo_set_pub_attr, const)
WRAP_ONE(target_vo_enable, vo_enable)
WRAP_ONE(target_vo_disable, vo_disable)
WRAP_TWO(target_vo_unbind_layer, vo_unbind_layer)
WRAP_TWO(target_vo_set_layer_buffer_length, vo_set_layer_buffer_length)
WRAP_ONE_PTR(target_vo_set_layer_attr, vo_set_layer_attr, const)
WRAP_TWO(target_vo_set_layer_splice_mode, vo_set_layer_splice_mode)
WRAP_ONE(target_vo_enable_layer, vo_enable_layer)
WRAP_ONE(target_vo_disable_layer, vo_disable_layer)
WRAP_TWO_PTR(target_vo_set_channel_attr, vo_set_channel_attr, const)
WRAP_TWO(target_vo_enable_channel, vo_enable_channel)
WRAP_TWO(target_vo_disable_channel, vo_disable_channel)
WRAP_ONE_PTR(target_avs_create_group, avs_create_group, const)
WRAP_ONE(target_avs_destroy_group, avs_destroy_group)
WRAP_ONE(target_avs_start_group, avs_start_group)
WRAP_ONE(target_avs_stop_group, avs_stop_group)
WRAP_TWO_PTR(target_avs_set_channel_attr, avs_set_channel_attr, const)
WRAP_TWO(target_avs_enable_channel, avs_enable_channel)
WRAP_TWO(target_avs_disable_channel, avs_disable_channel)
WRAP_ONE_PTR(target_venc_create_channel, venc_create_channel, const)
WRAP_ONE(target_venc_destroy_channel, venc_destroy_channel)
WRAP_ONE_PTR(target_venc_set_rc_param, venc_set_rc_param, const)
WRAP_ONE_PTR(target_venc_start_receive, venc_start_receive, const)
WRAP_ONE(target_venc_stop_receive, venc_stop_receive)
WRAP_ONE_PTR(target_venc_get_h264_vui, venc_get_h264_vui, )
WRAP_ONE_PTR(target_venc_set_h264_vui, venc_set_h264_vui, const)
WRAP_ONE_PTR(target_venc_get_h265_vui, venc_get_h265_vui, )
WRAP_ONE_PTR(target_venc_set_h265_vui, venc_set_h265_vui, const)
WRAP_ONE_PTR(target_venc_get_jpeg_param, venc_get_jpeg_param, )
WRAP_ONE_PTR(target_venc_set_jpeg_param, venc_set_jpeg_param, const)
WRAP_ONE_PTR(target_ai_set_pub_attr, ai_set_pub_attr, const)
WRAP_ONE(target_ai_enable, ai_enable)
WRAP_ONE(target_ai_disable, ai_disable)
WRAP_TWO(target_ai_enable_channel, ai_enable_channel)
WRAP_TWO(target_ai_disable_channel, ai_disable_channel)
WRAP_TWO_PTR(target_ai_set_channel_param, ai_set_channel_param, const)
WRAP_TWO(target_ai_disable_resample, ai_disable_resample)
WRAP_ONE(target_ao_clear_pub_attr, ao_clear_pub_attr)
WRAP_ONE_PTR(target_ao_set_pub_attr, ao_set_pub_attr, const)
WRAP_ONE(target_ao_enable, ao_enable)
WRAP_ONE(target_ao_disable, ao_disable)
WRAP_TWO(target_ao_enable_channel, ao_enable_channel)
WRAP_TWO(target_ao_disable_channel, ao_disable_channel)
WRAP_TWO_PTR(target_ao_set_channel_param, ao_set_channel_param, const)
WRAP_TWO(target_ao_disable_resample, ao_disable_resample)

static int target_ai_enable_resample(void *opaque, int device, int channel,
                                     int sample_rate)
{
    return ((struct ngcd_rk_target *)opaque)->ai_enable_resample(
        device, channel, sample_rate);
}

static int target_ai_get_frame(void *opaque, int device, int channel,
                               void *frame, void *aec_frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->ai_get_frame(
        device, channel, frame, aec_frame, timeout_ms);
}

static int target_ai_release_frame(void *opaque, int device, int channel,
                                   const void *frame, const void *aec_frame)
{
    return ((struct ngcd_rk_target *)opaque)->ai_release_frame(
        device, channel, frame, aec_frame);
}

static int target_ao_enable_resample(void *opaque, int device, int channel,
                                     int sample_rate)
{
    return ((struct ngcd_rk_target *)opaque)->ao_enable_resample(
        device, channel, sample_rate);
}

static int target_ao_send_frame(void *opaque, int device, int channel,
                                const void *frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->ao_send_frame(
        device, channel, frame, timeout_ms);
}

static int target_ao_wait_eos(void *opaque, int device, int channel,
                              int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->ao_wait_eos(
        device, channel, timeout_ms);
}

static int target_ao_set_volume(void *opaque, int device, int volume)
{
    return ((struct ngcd_rk_target *)opaque)->ao_set_volume(device, volume);
}

static int target_ao_get_volume(void *opaque, int device, int *volume)
{
    return ((struct ngcd_rk_target *)opaque)->ao_get_volume(device, volume);
}

static int target_avs_get_channel_frame(void *opaque, int group, int channel,
                                        void *frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->avs_get_channel_frame(
        group, channel, frame, timeout_ms);
}

static int target_vpss_get_channel_frame(void *opaque, int group, int channel,
                                         void *frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->vpss_get_channel_frame(
        group, channel, frame, timeout_ms);
}

static int target_vpss_release_channel_frame(void *opaque, int group,
                                             int channel, const void *frame)
{
    return ((struct ngcd_rk_target *)opaque)->vpss_release_channel_frame(
        group, channel, frame);
}

static int target_avs_release_channel_frame(void *opaque, int group,
                                            int channel, const void *frame)
{
    return ((struct ngcd_rk_target *)opaque)->avs_release_channel_frame(
        group, channel, frame);
}

static int target_venc_send_frame(void *opaque, int channel,
                                  const void *frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->venc_send_frame(
        channel, frame, timeout_ms);
}

static int target_venc_get_stream(void *opaque, int channel, void *stream,
                                  int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->venc_get_stream(
        channel, stream, timeout_ms);
}

static int target_venc_release_stream(void *opaque, int channel,
                                      const void *stream)
{
    return ((struct ngcd_rk_target *)opaque)->venc_release_stream(channel,
                                                                  stream);
}

static int target_venc_request_idr(void *opaque, int channel, bool instant)
{
    return ((struct ngcd_rk_target *)opaque)->venc_request_idr(
        channel, instant ? 1 : 0);
}

static int target_avs_set_working_set(void *opaque, uint64_t bytes)
{
    return ((struct ngcd_rk_target *)opaque)->avs_set_mod_param(&bytes);
}

static int target_vo_bind_layer(void *opaque, int layer, int device, int mode)
{
    return ((struct ngcd_rk_target *)opaque)->vo_bind_layer(layer, device,
                                                             mode);
}

static int target_vo_send_frame(void *opaque, int layer, int channel,
                                const void *frame, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->vo_send_frame(
        layer, channel, frame, timeout_ms);
}

static int target_vo_set_wbc_source(void *opaque, int wbc,
                                    const void *source)
{
    return ((struct ngcd_rk_target *)opaque)->vo_set_wbc_source(wbc, source);
}

static int target_vo_set_wbc_attr(void *opaque, int wbc,
                                  const void *attribute)
{
    return ((struct ngcd_rk_target *)opaque)->vo_set_wbc_attr(wbc, attribute);
}

static int target_vo_enable_wbc(void *opaque, int wbc)
{
    return ((struct ngcd_rk_target *)opaque)->vo_enable_wbc(wbc);
}

static int target_vo_disable_wbc(void *opaque, int wbc)
{
    return ((struct ngcd_rk_target *)opaque)->vo_disable_wbc(wbc);
}

static int target_mmz_alloc(void *opaque, void **handle, size_t bytes)
{
    if (bytes > UINT32_MAX)
        return -1;
    return ((struct ngcd_rk_target *)opaque)->mmz_alloc(
        handle, NULL, NULL, (unsigned int)bytes);
}

static int target_mmz_free(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->mmz_free(handle);
}

static int target_mb_create(void *opaque, void **handle, void *address,
                            size_t bytes)
{
    unsigned char configuration[40];
    if (handle == NULL || address == NULL || bytes == 0U)
        return -1;
    memset(configuration, 0, sizeof(configuration));
    memcpy(configuration, &address, sizeof(address));
    memcpy(configuration + 24U, &bytes, sizeof(bytes));
    return ((struct ngcd_rk_target *)opaque)->sys_create_mb(
        handle, configuration);
}

static int target_mb_release(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->mb_release(handle);
}

static int target_mb_handle_to_id(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->mb_handle_to_id(handle);
}

static void *target_mb_handle_to_address(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->mb_handle_to_address(handle);
}

static size_t target_mb_get_size(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->mb_get_size(handle);
}

static int target_vo_create_graphics_buffer(void *opaque, int width,
                                             int height, int format,
                                             void **handle)
{
    return ((struct ngcd_rk_target *)opaque)->vo_create_graphics_buffer(
        width, height, format, handle);
}

static int target_vo_destroy_graphics_buffer(void *opaque, void *handle)
{
    return ((struct ngcd_rk_target *)opaque)->vo_destroy_graphics_buffer(
        handle);
}

static int target_vdec_create_channel(void *opaque, int channel,
                                      const void *attribute)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_create_channel(
        channel, attribute);
}

static int target_vdec_destroy_channel(void *opaque, int channel)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_destroy_channel(channel);
}

static int target_vdec_start_receive(void *opaque, int channel)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_start_receive(channel);
}

static int target_vdec_stop_receive(void *opaque, int channel)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_stop_receive(channel);
}

static int target_vdec_reset_channel(void *opaque, int channel)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_reset_channel(channel);
}

static int target_vdec_send_stream(void *opaque, int channel,
                                   const void *stream, int timeout_ms)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_send_stream(
        channel, stream, timeout_ms);
}

static int target_vdec_query_status(void *opaque, int channel, void *status)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_query_status(channel,
                                                                status);
}

static int target_vdec_set_channel_param(void *opaque, int channel,
                                         const void *parameter)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_set_channel_param(
        channel, parameter);
}

static int target_vdec_set_display_mode(void *opaque, int channel, int mode)
{
    return ((struct ngcd_rk_target *)opaque)->vdec_set_display_mode(
        channel, mode);
}

int ngcd_rk_target_open(struct ngcd_rk_target **output,
                        struct ngcd_rk_api *api)
{
    struct ngcd_rk_target *target;
    if (output == NULL || api == NULL)
        return -1;
    *output = NULL;
    memset(api, 0, sizeof(*api));
    target = malloc(sizeof(*target));
    if (target == NULL)
        return -1;
    memset(target, 0, sizeof(*target));
    /*
     * Rockit's VO implementation loads libgraphic_lsf.so at runtime.  That
     * plugin expects symbols such as drmIoctl from librockit's dependencies
     * to be in the global lookup scope, matching the stock ngcd's direct
     * linkage.  Keeping librockit local makes the plugin abort during LCD
     * initialization even though dlopen itself succeeds.
     */
    target->rockit = dlopen("/app/lib/librockit.so", RTLD_NOW | RTLD_GLOBAL);
    if (target->rockit == NULL)
        goto fail;
    target->rkaiq = dlopen("/app/lib/librkaiq.so", RTLD_NOW | RTLD_LOCAL);
    if (target->rkaiq == NULL)
        goto fail;

    LOAD_FROM(target, target->rockit, sys_init, "RK_MPI_SYS_Init");
    LOAD_FROM(target, target->rockit, sys_exit, "RK_MPI_SYS_Exit");
    LOAD_FROM(target, target->rockit, sys_bind, "RK_MPI_SYS_Bind");
    LOAD_FROM(target, target->rockit, sys_unbind, "RK_MPI_SYS_UnBind");
    LOAD_FROM(target, target->rockit, vi_get_dev_attr, "RK_MPI_VI_GetDevAttr");
    LOAD_FROM(target, target->rockit, vi_set_dev_attr, "RK_MPI_VI_SetDevAttr");
    LOAD_FROM(target, target->rockit, vi_get_dev_enabled,
              "RK_MPI_VI_GetDevIsEnable");
    LOAD_FROM(target, target->rockit, vi_enable_dev, "RK_MPI_VI_EnableDev");
    LOAD_FROM(target, target->rockit, vi_disable_dev, "RK_MPI_VI_DisableDev");
    LOAD_FROM(target, target->rockit, vi_bind_pipe, "RK_MPI_VI_SetDevBindPipe");
    LOAD_FROM(target, target->rockit, vi_set_channel_attr,
              "RK_MPI_VI_SetChnAttr");
    LOAD_FROM(target, target->rockit, vi_enable_channel,
              "RK_MPI_VI_EnableChn");
    LOAD_FROM(target, target->rockit, vi_disable_channel,
              "RK_MPI_VI_DisableChn");
    LOAD_FROM(target, target->rockit, vpss_create_group,
              "RK_MPI_VPSS_CreateGrp");
    LOAD_FROM(target, target->rockit, vpss_destroy_group,
              "RK_MPI_VPSS_DestroyGrp");
    LOAD_FROM(target, target->rockit, vpss_set_device,
              "RK_MPI_VPSS_SetVProcDev");
    LOAD_FROM(target, target->rockit, vpss_enable_backup,
              "RK_MPI_VPSS_EnableBackupFrame");
    LOAD_FROM(target, target->rockit, vpss_start_group,
              "RK_MPI_VPSS_StartGrp");
    LOAD_FROM(target, target->rockit, vpss_stop_group,
              "RK_MPI_VPSS_StopGrp");
    LOAD_FROM(target, target->rockit, vpss_set_channel_attr,
              "RK_MPI_VPSS_SetChnAttr");
    LOAD_FROM(target, target->rockit, vpss_enable_channel,
              "RK_MPI_VPSS_EnableChn");
    LOAD_FROM(target, target->rockit, vpss_disable_channel,
              "RK_MPI_VPSS_DisableChn");
    LOAD_FROM(target, target->rockit, vo_set_pub_attr,
              "RK_MPI_VO_SetPubAttr");
    LOAD_FROM(target, target->rockit, vo_enable, "RK_MPI_VO_Enable");
    LOAD_FROM(target, target->rockit, vo_disable, "RK_MPI_VO_Disable");
    LOAD_FROM(target, target->rockit, vo_bind_layer, "RK_MPI_VO_BindLayer");
    LOAD_FROM(target, target->rockit, vo_unbind_layer,
              "RK_MPI_VO_UnBindLayer");
    LOAD_FROM(target, target->rockit, vo_set_layer_buffer_length,
              "RK_MPI_VO_SetLayerDispBufLen");
    LOAD_FROM(target, target->rockit, vo_set_layer_attr,
              "RK_MPI_VO_SetLayerAttr");
    LOAD_FROM(target, target->rockit, vo_set_layer_splice_mode,
              "RK_MPI_VO_SetLayerSpliceMode");
    LOAD_FROM(target, target->rockit, vo_enable_layer,
              "RK_MPI_VO_EnableLayer");
    LOAD_FROM(target, target->rockit, vo_disable_layer,
              "RK_MPI_VO_DisableLayer");
    LOAD_FROM(target, target->rockit, vo_set_channel_attr,
              "RK_MPI_VO_SetChnAttr");
    LOAD_FROM(target, target->rockit, vo_enable_channel,
              "RK_MPI_VO_EnableChn");
    LOAD_FROM(target, target->rockit, vo_disable_channel,
              "RK_MPI_VO_DisableChn");
    LOAD_FROM(target, target->rockit, vo_send_frame,
              "RK_MPI_VO_SendFrame");
    LOAD_FROM(target, target->rockit, vo_set_wbc_source,
              "RK_MPI_VO_SetWbcSource");
    LOAD_FROM(target, target->rockit, vo_set_wbc_attr,
              "RK_MPI_VO_SetWbcAttr");
    LOAD_FROM(target, target->rockit, vo_enable_wbc,
              "RK_MPI_VO_EnableWbc");
    LOAD_FROM(target, target->rockit, vo_disable_wbc,
              "RK_MPI_VO_DisableWbc");
    LOAD_FROM(target, target->rockit, vo_create_graphics_buffer,
              "RK_MPI_VO_CreateGraphicsFrameBuffer");
    LOAD_FROM(target, target->rockit, vo_destroy_graphics_buffer,
              "RK_MPI_VO_DestroyGraphicsFrameBuffer");
    LOAD_FROM(target, target->rockit, mmz_alloc, "RK_MPI_SYS_MmzAlloc");
    LOAD_FROM(target, target->rockit, mmz_free, "RK_MPI_SYS_MmzFree");
    LOAD_FROM(target, target->rockit, sys_create_mb, "RK_MPI_SYS_CreateMB");
    LOAD_FROM(target, target->rockit, mb_release, "RK_MPI_MB_ReleaseMB");
    LOAD_FROM(target, target->rockit, mb_handle_to_id,
              "RK_MPI_MB_Handle2UniqueId");
    LOAD_FROM(target, target->rockit, mb_handle_to_address,
              "RK_MPI_MB_Handle2VirAddr");
    LOAD_FROM(target, target->rockit, mb_get_size, "RK_MPI_MB_GetSize");
    LOAD_FROM(target, target->rockit, avs_set_mod_param,
              "RK_MPI_AVS_SetModParam");
    LOAD_FROM(target, target->rockit, avs_create_group,
              "RK_MPI_AVS_CreateGrp");
    LOAD_FROM(target, target->rockit, avs_destroy_group,
              "RK_MPI_AVS_DestroyGrp");
    LOAD_FROM(target, target->rockit, avs_start_group,
              "RK_MPI_AVS_StartGrp");
    LOAD_FROM(target, target->rockit, avs_stop_group,
              "RK_MPI_AVS_StopGrp");
    LOAD_FROM(target, target->rockit, avs_set_channel_attr,
              "RK_MPI_AVS_SetChnAttr");
    LOAD_FROM(target, target->rockit, avs_enable_channel,
              "RK_MPI_AVS_EnableChn");
    LOAD_FROM(target, target->rockit, avs_disable_channel,
              "RK_MPI_AVS_DisableChn");
    LOAD_FROM(target, target->rockit, avs_get_channel_frame,
              "RK_MPI_AVS_GetChnFrame");
    LOAD_FROM(target, target->rockit, avs_release_channel_frame,
              "RK_MPI_AVS_ReleaseChnFrame");
    LOAD_FROM(target, target->rockit, vpss_get_channel_frame,
              "RK_MPI_VPSS_GetChnFrame");
    LOAD_FROM(target, target->rockit, vpss_release_channel_frame,
              "RK_MPI_VPSS_ReleaseChnFrame");
    LOAD_FROM(target, target->rockit, venc_create_channel,
              "RK_MPI_VENC_CreateChn");
    LOAD_FROM(target, target->rockit, venc_destroy_channel,
              "RK_MPI_VENC_DestroyChn");
    LOAD_FROM(target, target->rockit, venc_set_rc_param,
              "RK_MPI_VENC_SetRcParam");
    LOAD_FROM(target, target->rockit, venc_start_receive,
              "RK_MPI_VENC_StartRecvFrame");
    LOAD_FROM(target, target->rockit, venc_stop_receive,
              "RK_MPI_VENC_StopRecvFrame");
    LOAD_FROM(target, target->rockit, venc_get_stream,
              "RK_MPI_VENC_GetStream");
    LOAD_FROM(target, target->rockit, venc_release_stream,
              "RK_MPI_VENC_ReleaseStream");
    LOAD_FROM(target, target->rockit, venc_request_idr,
              "RK_MPI_VENC_RequestIDR");
    LOAD_FROM(target, target->rockit, venc_get_h264_vui,
              "RK_MPI_VENC_GetH264Vui");
    LOAD_FROM(target, target->rockit, venc_set_h264_vui,
              "RK_MPI_VENC_SetH264Vui");
    LOAD_FROM(target, target->rockit, venc_get_h265_vui,
              "RK_MPI_VENC_GetH265Vui");
    LOAD_FROM(target, target->rockit, venc_set_h265_vui,
              "RK_MPI_VENC_SetH265Vui");
    LOAD_FROM(target, target->rockit, venc_get_jpeg_param,
              "RK_MPI_VENC_GetJpegParam");
    LOAD_FROM(target, target->rockit, venc_set_jpeg_param,
              "RK_MPI_VENC_SetJpegParam");
    LOAD_FROM(target, target->rockit, venc_send_frame,
              "RK_MPI_VENC_SendFrame");
    LOAD_FROM(target, target->rockit, vdec_create_channel,
              "RK_MPI_VDEC_CreateChn");
    LOAD_FROM(target, target->rockit, vdec_destroy_channel,
              "RK_MPI_VDEC_DestroyChn");
    LOAD_FROM(target, target->rockit, vdec_start_receive,
              "RK_MPI_VDEC_StartRecvStream");
    LOAD_FROM(target, target->rockit, vdec_stop_receive,
              "RK_MPI_VDEC_StopRecvStream");
    LOAD_FROM(target, target->rockit, vdec_reset_channel,
              "RK_MPI_VDEC_ResetChn");
    LOAD_FROM(target, target->rockit, vdec_send_stream,
              "RK_MPI_VDEC_SendStream");
    LOAD_FROM(target, target->rockit, vdec_query_status,
              "RK_MPI_VDEC_QueryStatus");
    LOAD_FROM(target, target->rockit, vdec_set_channel_param,
              "RK_MPI_VDEC_SetChnParam");
    LOAD_FROM(target, target->rockit, vdec_set_display_mode,
              "RK_MPI_VDEC_SetDisplayMode");
    LOAD_FROM(target, target->rockit, ai_set_pub_attr,
              "RK_MPI_AI_SetPubAttr");
    LOAD_FROM(target, target->rockit, ai_enable, "RK_MPI_AI_Enable");
    LOAD_FROM(target, target->rockit, ai_disable, "RK_MPI_AI_Disable");
    LOAD_FROM(target, target->rockit, ai_enable_channel,
              "RK_MPI_AI_EnableChn");
    LOAD_FROM(target, target->rockit, ai_disable_channel,
              "RK_MPI_AI_DisableChn");
    LOAD_FROM(target, target->rockit, ai_set_channel_param,
              "RK_MPI_AI_SetChnParam");
    LOAD_FROM(target, target->rockit, ai_enable_resample,
              "RK_MPI_AI_EnableReSmp");
    LOAD_FROM(target, target->rockit, ai_disable_resample,
              "RK_MPI_AI_DisableReSmp");
    LOAD_FROM(target, target->rockit, ai_get_frame,
              "RK_MPI_AI_GetFrame");
    LOAD_FROM(target, target->rockit, ai_release_frame,
              "RK_MPI_AI_ReleaseFrame");
    LOAD_FROM(target, target->rockit, ao_clear_pub_attr,
              "RK_MPI_AO_ClrPubAttr");
    LOAD_FROM(target, target->rockit, ao_set_pub_attr,
              "RK_MPI_AO_SetPubAttr");
    LOAD_FROM(target, target->rockit, ao_enable, "RK_MPI_AO_Enable");
    LOAD_FROM(target, target->rockit, ao_disable, "RK_MPI_AO_Disable");
    LOAD_FROM(target, target->rockit, ao_enable_channel,
              "RK_MPI_AO_EnableChn");
    LOAD_FROM(target, target->rockit, ao_disable_channel,
              "RK_MPI_AO_DisableChn");
    LOAD_FROM(target, target->rockit, ao_set_channel_param,
              "RK_MPI_AO_SetChnParams");
    LOAD_FROM(target, target->rockit, ao_enable_resample,
              "RK_MPI_AO_EnableReSmp");
    LOAD_FROM(target, target->rockit, ao_disable_resample,
              "RK_MPI_AO_DisableReSmp");
    LOAD_FROM(target, target->rockit, ao_send_frame,
              "RK_MPI_AO_SendFrame");
    LOAD_FROM(target, target->rockit, ao_wait_eos,
              "RK_MPI_AO_WaitEos");
    LOAD_FROM(target, target->rockit, ao_set_volume,
              "RK_MPI_AO_SetVolume");
    LOAD_FROM(target, target->rockit, ao_get_volume,
              "RK_MPI_AO_GetVolume");

    LOAD_FROM(target, target->rkaiq, aiq_preinit_scene,
              "rk_aiq_uapi2_sysctl_preInit_scene");
    LOAD_FROM(target, target->rkaiq, aiq_enum_static_metas,
              "rk_aiq_uapi2_sysctl_enumStaticMetas");
    LOAD_FROM(target, target->rkaiq, aiq_init,
              "rk_aiq_uapi2_sysctl_init");
    LOAD_FROM(target, target->rkaiq, aiq_prepare,
              "rk_aiq_uapi2_sysctl_prepare");
    LOAD_FROM(target, target->rkaiq, aiq_start,
              "rk_aiq_uapi2_sysctl_start");
    LOAD_FROM(target, target->rkaiq, aiq_stop,
              "rk_aiq_uapi2_sysctl_stop");
    LOAD_FROM(target, target->rkaiq, aiq_deinit,
              "rk_aiq_uapi2_sysctl_deinit");
    LOAD_FROM(target, target->rkaiq, aiq_group_create,
              "rk_aiq_uapi2_camgroup_create");
    LOAD_FROM(target, target->rkaiq, aiq_group_prepare,
              "rk_aiq_uapi2_camgroup_prepare");
    LOAD_FROM(target, target->rkaiq, aiq_group_start,
              "rk_aiq_uapi2_camgroup_start");
    LOAD_FROM(target, target->rkaiq, aiq_group_stop,
              "rk_aiq_uapi2_camgroup_stop");
    LOAD_FROM(target, target->rkaiq, aiq_group_destroy,
              "rk_aiq_uapi2_camgroup_destroy");
    LOAD_FROM(target, target->rkaiq, aiq_group_get_sensor,
              "rk_aiq_uapi2_camgroup_getAiqCtxBySnsNm");
    LOAD_FROM(target, target->rkaiq, aiq_get_acp,
              "rk_aiq_user_api2_acp_GetAttrib");
    LOAD_FROM(target, target->rkaiq, aiq_set_acp,
              "rk_aiq_user_api2_acp_SetAttrib");
    LOAD_FROM(target, target->rkaiq, aiq_get_sharpness,
              "rk_aiq_uapi2_getSharpness");
    LOAD_FROM(target, target->rkaiq, aiq_set_sharpness,
              "rk_aiq_uapi2_setSharpness");
    LOAD_FROM(target, target->rkaiq, aiq_get_anr,
              "rk_aiq_uapi2_getANRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_set_anr,
              "rk_aiq_uapi2_setANRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_get_spatial_nr,
              "rk_aiq_uapi2_getMSpaNRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_set_spatial_nr,
              "rk_aiq_uapi2_setMSpaNRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_get_temporal_nr,
              "rk_aiq_uapi2_getMTNRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_set_temporal_nr,
              "rk_aiq_uapi2_setMTNRStrth");
    LOAD_FROM(target, target->rkaiq, aiq_get_exposure,
              "rk_aiq_user_api2_ae_getExpSwAttr");
    LOAD_FROM(target, target->rkaiq, aiq_set_exposure,
              "rk_aiq_user_api2_ae_setExpSwAttr");
    LOAD_FROM(target, target->rkaiq, aiq_get_linear_exposure,
              "rk_aiq_user_api2_ae_getLinExpAttr");
    LOAD_FROM(target, target->rkaiq, aiq_set_linear_exposure,
              "rk_aiq_user_api2_ae_setLinExpAttr");
    LOAD_FROM(target, target->rkaiq, aiq_query_exposure,
              "rk_aiq_user_api2_ae_queryExpResInfo");
    LOAD_FROM(target, target->rkaiq, aiq_get_white_balance_mode,
              "rk_aiq_uapi2_getWBMode");
    LOAD_FROM(target, target->rkaiq, aiq_set_white_balance_mode,
              "rk_aiq_uapi2_setWBMode");
    LOAD_FROM(target, target->rkaiq, aiq_get_white_balance_ct,
              "rk_aiq_uapi2_getWBCT");
    LOAD_FROM(target, target->rkaiq, aiq_set_white_balance_ct,
              "rk_aiq_uapi2_setMWBCT");
    LOAD_FROM(target, target->rkaiq, aiq_get_white_balance_gain,
              "rk_aiq_uapi2_getWBGain");
    LOAD_FROM(target, target->rkaiq, aiq_set_white_balance_gain,
              "rk_aiq_uapi2_setMWBGain");
    LOAD_FROM(target, target->rkaiq, aiq_get_flicker_enabled,
              "rk_aiq_uapi2_getAntiFlickerEn");
    LOAD_FROM(target, target->rkaiq, aiq_set_flicker_enabled,
              "rk_aiq_uapi2_setAntiFlickerEn");
    LOAD_FROM(target, target->rkaiq, aiq_get_flicker_mode,
              "rk_aiq_uapi2_getAntiFlickerMode");
    LOAD_FROM(target, target->rkaiq, aiq_set_flicker_mode,
              "rk_aiq_uapi2_setAntiFlickerMode");
    LOAD_FROM(target, target->rkaiq, aiq_get_power_line_frequency,
              "rk_aiq_uapi2_getExpPwrLineFreqMode");
    LOAD_FROM(target, target->rkaiq, aiq_set_power_line_frequency,
              "rk_aiq_uapi2_setExpPwrLineFreqMode");
    LOAD_FROM(target, target->rkaiq, aiq_get_effect,
              "rk_aiq_user_api2_aie_GetAttrib");
    LOAD_FROM(target, target->rkaiq, aiq_set_effect,
              "rk_aiq_user_api2_aie_SetAttrib");
    LOAD_FROM(target, target->rkaiq, aiq_capture_raw,
              "rk_aiq_uapi2_debug_captureRawSync");
    LOAD_FROM(target, target->rkaiq, aiq_prepare_raw,
              "rk_aiq_uapi2_sysctl_prepareRkRaw");
    LOAD_FROM(target, target->rkaiq, aiq_enqueue_raw,
              "rk_aiq_uapi2_sysctl_enqueueRkRawBuf");
    LOAD_FROM(target, target->rkaiq, aiq_enqueue_raw_file,
              "rk_aiq_uapi2_sysctl_enqueueRkRawFile");

    api->system_init = target_system_init;
    api->system_exit = target_system_exit;
    api->bind = target_bind;
    api->unbind = target_unbind;
    api->sensor_start = target_sensor_start;
    api->offline_sensor_start = target_offline_sensor_start;
    api->offline_sensor_run = target_offline_sensor_run;
    api->offline_sensor_enqueue = target_offline_sensor_enqueue;
    api->sensor_stop = target_sensor_stop;
    api->sensor_synchronize = target_sensor_synchronize;
    api->aiq_get_acp = target_aiq_get_acp;
    api->aiq_set_acp = target_aiq_set_acp;
    api->aiq_get_sharpness = target_aiq_get_sharpness;
    api->aiq_set_sharpness = target_aiq_set_sharpness;
    api->aiq_get_anr = target_aiq_get_anr;
    api->aiq_set_anr = target_aiq_set_anr;
    api->aiq_get_spatial_nr = target_aiq_get_spatial_nr;
    api->aiq_set_spatial_nr = target_aiq_set_spatial_nr;
    api->aiq_get_temporal_nr = target_aiq_get_temporal_nr;
    api->aiq_set_temporal_nr = target_aiq_set_temporal_nr;
    api->aiq_get_exposure = target_aiq_get_exposure;
    api->aiq_set_exposure = target_aiq_set_exposure;
    api->aiq_get_linear_exposure = target_aiq_get_linear_exposure;
    api->aiq_set_linear_exposure = target_aiq_set_linear_exposure;
    api->aiq_query_exposure = target_aiq_query_exposure;
    api->aiq_get_white_balance_mode = target_aiq_get_white_balance_mode;
    api->aiq_set_white_balance_mode = target_aiq_set_white_balance_mode;
    api->aiq_get_white_balance_ct = target_aiq_get_white_balance_ct;
    api->aiq_set_white_balance_ct = target_aiq_set_white_balance_ct;
    api->aiq_get_white_balance_gain = target_aiq_get_white_balance_gain;
    api->aiq_get_flicker_enabled = target_aiq_get_flicker_enabled;
    api->aiq_set_flicker_enabled = target_aiq_set_flicker_enabled;
    api->aiq_get_flicker_mode = target_aiq_get_flicker_mode;
    api->aiq_set_flicker_mode = target_aiq_set_flicker_mode;
    api->aiq_get_power_line_frequency = target_aiq_get_power_line_frequency;
    api->aiq_set_power_line_frequency = target_aiq_set_power_line_frequency;
    api->aiq_get_effect = target_aiq_get_effect;
    api->aiq_set_effect = target_aiq_set_effect;
    api->aiq_capture_raw = target_aiq_capture_raw;
    api->prepare_directory = target_prepare_directory;
    api->wait_output = target_wait_output;
    api->vi_get_dev_attr = target_vi_get_dev_attr;
    api->vi_set_dev_attr = target_vi_set_dev_attr;
    api->vi_get_dev_enabled = target_vi_get_dev_enabled;
    api->vi_enable_dev = target_vi_enable_dev;
    api->vi_disable_dev = target_vi_disable_dev;
    api->vi_bind_pipe = target_vi_bind_pipe;
    api->vi_set_channel_attr = target_vi_set_channel_attr;
    api->vi_enable_channel = target_vi_enable_channel;
    api->vi_disable_channel = target_vi_disable_channel;
    api->vpss_create_group = target_vpss_create_group;
    api->vpss_destroy_group = target_vpss_destroy_group;
    api->vpss_set_device = target_vpss_set_device;
    api->vpss_enable_backup = target_vpss_enable_backup;
    api->vpss_start_group = target_vpss_start_group;
    api->vpss_stop_group = target_vpss_stop_group;
    api->vpss_set_channel_attr = target_vpss_set_channel_attr;
    api->vpss_enable_channel = target_vpss_enable_channel;
    api->vpss_disable_channel = target_vpss_disable_channel;
    api->vpss_get_channel_frame = target_vpss_get_channel_frame;
    api->vpss_release_channel_frame = target_vpss_release_channel_frame;
    api->vo_set_pub_attr = target_vo_set_pub_attr;
    api->vo_enable = target_vo_enable;
    api->vo_disable = target_vo_disable;
    api->vo_bind_layer = target_vo_bind_layer;
    api->vo_unbind_layer = target_vo_unbind_layer;
    api->vo_set_layer_buffer_length = target_vo_set_layer_buffer_length;
    api->vo_set_layer_attr = target_vo_set_layer_attr;
    api->vo_set_layer_splice_mode = target_vo_set_layer_splice_mode;
    api->vo_enable_layer = target_vo_enable_layer;
    api->vo_disable_layer = target_vo_disable_layer;
    api->vo_set_channel_attr = target_vo_set_channel_attr;
    api->vo_enable_channel = target_vo_enable_channel;
    api->vo_disable_channel = target_vo_disable_channel;
    api->vo_send_frame = target_vo_send_frame;
    api->vo_set_wbc_source = target_vo_set_wbc_source;
    api->vo_set_wbc_attr = target_vo_set_wbc_attr;
    api->vo_enable_wbc = target_vo_enable_wbc;
    api->vo_disable_wbc = target_vo_disable_wbc;
    api->mmz_alloc = target_mmz_alloc;
    api->mmz_free = target_mmz_free;
    api->mb_create = target_mb_create;
    api->mb_release = target_mb_release;
    api->mb_handle_to_id = target_mb_handle_to_id;
    api->mb_handle_to_address = target_mb_handle_to_address;
    api->mb_get_size = target_mb_get_size;
    api->vo_create_graphics_buffer = target_vo_create_graphics_buffer;
    api->vo_destroy_graphics_buffer = target_vo_destroy_graphics_buffer;
    api->avs_set_working_set = target_avs_set_working_set;
    api->avs_create_group = target_avs_create_group;
    api->avs_destroy_group = target_avs_destroy_group;
    api->avs_start_group = target_avs_start_group;
    api->avs_stop_group = target_avs_stop_group;
    api->avs_set_channel_attr = target_avs_set_channel_attr;
    api->avs_enable_channel = target_avs_enable_channel;
    api->avs_disable_channel = target_avs_disable_channel;
    api->avs_get_channel_frame = target_avs_get_channel_frame;
    api->avs_release_channel_frame = target_avs_release_channel_frame;
    api->venc_create_channel = target_venc_create_channel;
    api->venc_destroy_channel = target_venc_destroy_channel;
    api->venc_set_rc_param = target_venc_set_rc_param;
    api->venc_start_receive = target_venc_start_receive;
    api->venc_stop_receive = target_venc_stop_receive;
    api->venc_get_stream = target_venc_get_stream;
    api->venc_release_stream = target_venc_release_stream;
    api->venc_request_idr = target_venc_request_idr;
    api->venc_get_h264_vui = target_venc_get_h264_vui;
    api->venc_set_h264_vui = target_venc_set_h264_vui;
    api->venc_get_h265_vui = target_venc_get_h265_vui;
    api->venc_set_h265_vui = target_venc_set_h265_vui;
    api->venc_get_jpeg_param = target_venc_get_jpeg_param;
    api->venc_set_jpeg_param = target_venc_set_jpeg_param;
    api->venc_send_frame = target_venc_send_frame;
    api->vdec_create_channel = target_vdec_create_channel;
    api->vdec_destroy_channel = target_vdec_destroy_channel;
    api->vdec_start_receive = target_vdec_start_receive;
    api->vdec_stop_receive = target_vdec_stop_receive;
    api->vdec_reset_channel = target_vdec_reset_channel;
    api->vdec_send_stream = target_vdec_send_stream;
    api->vdec_query_status = target_vdec_query_status;
    api->vdec_set_channel_param = target_vdec_set_channel_param;
    api->vdec_set_display_mode = target_vdec_set_display_mode;
    api->ai_set_pub_attr = target_ai_set_pub_attr;
    api->ai_enable = target_ai_enable;
    api->ai_disable = target_ai_disable;
    api->ai_enable_channel = target_ai_enable_channel;
    api->ai_disable_channel = target_ai_disable_channel;
    api->ai_set_channel_param = target_ai_set_channel_param;
    api->ai_enable_resample = target_ai_enable_resample;
    api->ai_disable_resample = target_ai_disable_resample;
    api->ai_get_frame = target_ai_get_frame;
    api->ai_release_frame = target_ai_release_frame;
    api->ao_clear_pub_attr = target_ao_clear_pub_attr;
    api->ao_set_pub_attr = target_ao_set_pub_attr;
    api->ao_enable = target_ao_enable;
    api->ao_disable = target_ao_disable;
    api->ao_enable_channel = target_ao_enable_channel;
    api->ao_disable_channel = target_ao_disable_channel;
    api->ao_set_channel_param = target_ao_set_channel_param;
    api->ao_enable_resample = target_ao_enable_resample;
    api->ao_disable_resample = target_ao_disable_resample;
    api->ao_send_frame = target_ao_send_frame;
    api->ao_wait_eos = target_ao_wait_eos;
    api->ao_set_volume = target_ao_set_volume;
    api->ao_get_volume = target_ao_get_volume;
    *output = target;
    return 0;

fail:
    ngcd_rk_target_close(target);
    return -1;
}

void ngcd_rk_target_close(struct ngcd_rk_target *target)
{
    if (target == NULL)
        return;
    if (target->aiq_group_context != NULL)
        target_sensor_group_destroy(target);
    if (target->rkaiq != NULL)
        (void)dlclose(target->rkaiq);
    if (target->rockit != NULL)
        (void)dlclose(target->rockit);
    free(target);
}
