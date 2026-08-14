#ifndef CALF_NGCD_H
#define CALF_NGCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NGCD_STOCK_SESSION_MARKER "/tmp/calf-ui-stock-session"

#define NGCD_HTTP_BODY_MAX 65536U
#define NGCD_HTTP_RESPONSE_MAX 65536U
#define NGCD_PATH_MAX 512U
#define NGCD_VALUE_MAX 64U
#define NGCD_URL_MAX 1024U
#define NGCD_HISTOGRAM_BINS 64U
#define NGCD_PROFILE_ENCODERS 4U
#define NGCD_WIFI_NETWORKS_MAX 32U
#define NGCD_WIFI_SSID_MAX 64U

enum ngcd_method {
    NGCD_METHOD_GET,
    NGCD_METHOD_POST,
    NGCD_METHOD_OPTIONS,
    NGCD_METHOD_UNKNOWN,
};

struct ngcd_request {
    enum ngcd_method method;
    char path[NGCD_PATH_MAX];
    char query[NGCD_PATH_MAX];
    const char *body;
    size_t body_length;
};

struct ngcd_response {
    int status;
    char content_type[32];
    char body[NGCD_HTTP_RESPONSE_MAX];
    const void *body_data;
    size_t body_length;
};

struct ngcd_image_state {
    char exposure[NGCD_VALUE_MAX];
    char iso[NGCD_VALUE_MAX];
    char white_balance[NGCD_VALUE_MAX];
    char exposure_compensation[NGCD_VALUE_MAX];
    int brightness;
    int contrast;
    int saturation;
    int hue;
    int sharpness;
    int noise_reduction;
    char anti_flicker[NGCD_VALUE_MAX];
    char effect[NGCD_VALUE_MAX];
};

struct ngcd_encoder_state {
    char codec[8];
    char rate_control[8];
    char profile[8];
    int width;
    int height;
    int fps;
    int bitrate;
    int gop;
    int color_range;
};

struct ngcd_video_geometry {
    int mode;
    int width;
    int height;
    int fps;
};

struct ngcd_profile {
    char camera_mode[NGCD_VALUE_MAX];
    int isp_mode;
    bool preview;
    struct ngcd_video_geometry sensor[2];
    size_t sensor_count;
    struct ngcd_video_geometry capture[2];
    size_t capture_count;
    bool gdc_enabled;
    struct ngcd_video_geometry gdc;
    char gdc_mesh[NGCD_PATH_MAX];
    bool output_enabled;
    struct ngcd_video_geometry output;
    char stitch_mode[NGCD_VALUE_MAX];
    struct ngcd_video_geometry stitch;
    int stitch_fov_x;
    int stitch_fov_y;
    struct ngcd_encoder_state encoder[NGCD_PROFILE_ENCODERS];
    bool encoder_mask[NGCD_PROFILE_ENCODERS];
    size_t encoder_count;
};

struct ngcd_imu_sample {
    int gyro_x;
    int gyro_y;
    int gyro_z;
    int acceleration_x;
    int acceleration_y;
    int acceleration_z;
    uint64_t monotonic_ns;
};

struct ngcd_storage_info {
    char location[32];
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool read_only;
};

struct ngcd_media_entry {
    char path[NGCD_PATH_MAX];
    char name[16];
    uint64_t size;
    uint64_t create_time;
    bool video;
};

struct ngcd_wifi_info {
    char ip_address[64];
    char mac_address[18];
    char ssid[NGCD_WIFI_SSID_MAX];
    int quality;
    int level;
};

struct ngcd_wifi_network {
    char ssid[NGCD_WIFI_SSID_MAX];
    int quality;
    int level;
};

struct ngcd_power_info {
    int battery_percent;
    int usb_supply;
    int system_temperature;
    int core_temperature;
};

struct ngcd_runtime_state {
    bool camera_running;
    char camera_mode[NGCD_VALUE_MAX];
    bool recording;
    uint64_t recording_started_ns;
    bool playback;
    bool playback_paused;
    int playback_sample_index;
    int playback_sample_count;
    uint32_t playback_decoder_received;
    uint32_t playback_decoder_decoded;
    uint32_t playback_decoder_pending_stream;
    uint32_t playback_decoder_pending_pictures;
    uint32_t playback_decoder_errors;
    uint32_t playback_presented_frames;
    uint32_t playback_output_errors;
    uint64_t playback_file_size;
    uint64_t playback_create_time;
    uint64_t playback_duration_us;
    char playback_codec[8];
    int playback_width;
    int playback_height;
    bool playback_picture;
    bool live;
    bool rtmp;
    bool rtsp;
    bool srt;
    bool open_stream;
    bool uvc;
    int backlight;
    int backlight_saved;
    int audio_auto;
    int audio_input;
    int audio_volume[3];
    int speaker_volume;
    int imu_calibration_state;
    struct ngcd_image_state image;
    struct ngcd_encoder_state encoder[3];
};

struct ngcd_backend;

struct ngcd_backend_ops {
    int (*start)(struct ngcd_backend *backend);
    void (*stop)(struct ngcd_backend *backend);
    int (*tick)(struct ngcd_backend *backend);
    int (*graphics_control_id)(struct ngcd_backend *backend);
    int (*histogram)(struct ngcd_backend *backend,
                     uint32_t bins[NGCD_HISTOGRAM_BINS]);
    int (*lcd_screenshot)(struct ngcd_backend *backend,
                          const unsigned char **data, size_t *size);
    int (*camera_mode)(struct ngcd_backend *backend, const char *mode,
                       bool start);
    int (*set_image)(struct ngcd_backend *backend, const char *type,
                     const char *value, bool fixed);
    int (*night_preview)(struct ngcd_backend *backend, int fps,
                         const char *exposure, const char *iso);
    int (*read_image)(struct ngcd_backend *backend);
    int (*snapshot)(struct ngcd_backend *backend, char *filename,
                    size_t filename_size);
    int (*recording)(struct ngcd_backend *backend, const char *action,
                     int split_type, uint64_t size_limit,
                     uint64_t time_limit);
    int (*playback)(struct ngcd_backend *backend, const char *action,
                    const char *path, int first, int second);
    int (*stream)(struct ngcd_backend *backend, const char *service,
                  const char *action, const char *url);
    int (*uvc)(struct ngcd_backend *backend, bool enable);
    int (*read_imu)(struct ngcd_backend *backend,
                    struct ngcd_imu_sample *sample);
    int (*calibrate_imu)(struct ngcd_backend *backend, int type, bool save,
                         int count);
    int (*imu_calibration_state)(struct ngcd_backend *backend, int *state);
    int (*set_encoder)(struct ngcd_backend *backend, int channel,
                       const struct ngcd_encoder_state *encoder,
                       uint32_t changed_fields);
    int (*set_backlight)(struct ngcd_backend *backend, int value);
    int (*set_audio)(struct ngcd_backend *backend, int input, bool automatic,
                     int volume, bool set_volume);
    int (*set_speaker)(struct ngcd_backend *backend, int volume);
    int (*storage)(struct ngcd_backend *backend, const char *action,
                   const char *argument, int first, int second,
                   int *result);
    int (*storage_status)(struct ngcd_backend *backend,
                          struct ngcd_storage_info *info);
    int (*wifi_status)(struct ngcd_backend *backend,
                       struct ngcd_wifi_info *info);
    int (*wifi_scan)(struct ngcd_backend *backend,
                     struct ngcd_wifi_network *networks, size_t capacity,
                     size_t *count);
    int (*power_status)(struct ngcd_backend *backend,
                        struct ngcd_power_info *info);
    int (*system_action)(struct ngcd_backend *backend, const char *action);
};

struct ngcd_backend {
    const struct ngcd_backend_ops *ops;
    struct ngcd_runtime_state state;
    void *private_data;
};

struct ngcd_app {
    struct ngcd_backend backend;
    bool poweroff_requested;
    const char *manufacturer;
    const char *brand;
    const char *product;
    const char *version;
    const char *build_time;
    const char *hardware;
    const char *serial_number;
    char manufacturer_storage[64];
    char brand_storage[64];
    char product_storage[128];
    char version_storage[128];
    char build_time_storage[64];
    char hardware_storage[64];
    char serial_number_storage[128];
};

void ngcd_app_init(struct ngcd_app *app);
void ngcd_app_load_product_identity(struct ngcd_app *app);
int ngcd_dispatch(struct ngcd_app *app, const struct ngcd_request *request,
                  struct ngcd_response *response);
int ngcd_serve(struct ngcd_app *app, const char *address, uint16_t port);
void ngcd_request_shutdown(void);
int ngcd_system_poweroff(void);
int ngcd_select_stock_session(void);
int ngcd_write_stock_session_marker(const char *path);

int ngcd_json_get_string(const char *json, size_t length, const char *key,
                         char *output, size_t output_size);
int ngcd_json_get_int64(const char *json, size_t length, const char *key,
                        int64_t *value);
int ngcd_json_get_bool(const char *json, size_t length, const char *key,
                       bool *value);
int ngcd_json_escape(char *output, size_t output_size, const char *value);
int ngcd_profile_parse(const char *yaml, size_t length,
                       struct ngcd_profile *profile, char *error,
                       size_t error_size);
int ngcd_profile_load(const char *path, struct ngcd_profile *profile,
                      char *error, size_t error_size);
int ngcd_wifi_parse_status(const char *text, size_t length,
                           struct ngcd_wifi_info *info);
int ngcd_wifi_parse_scan_results(const char *text, size_t length,
                                 struct ngcd_wifi_network *networks,
                                 size_t capacity, size_t *count);
int ngcd_wifi_read_status(struct ngcd_wifi_info *info);
int ngcd_wifi_scan_begin(void);
int ngcd_wifi_scan_results(struct ngcd_wifi_network *networks, size_t capacity,
                           size_t *count);
int ngcd_wifi_scan(struct ngcd_wifi_network *networks, size_t capacity,
                   size_t *count);
int ngcd_power_parse_value(const char *text, int minimum, int maximum,
                           int *value);
int ngcd_power_read_status(struct ngcd_power_info *info);
int ngcd_backlight_read(int *brightness);
int ngcd_backlight_write(int brightness);
int ngcd_storage_parse_mounts(const char *text, size_t length,
                              char *location, size_t location_size,
                              bool *read_only);
int ngcd_storage_read_status(struct ngcd_storage_info *info);
int ngcd_storage_media_paths(const char *root, const char *extension,
                             unsigned int *sequence,
                             char *basename, size_t basename_size,
                             char *temporary, size_t temporary_size,
                             char *final_path, size_t final_size);
int ngcd_storage_media_list(const char *root, size_t offset,
                            struct ngcd_media_entry *entries,
                            size_t capacity, size_t *count, size_t *total);
int ngcd_storage_io_test_file(const char *path, int block_kb, int count,
                              uint64_t available_bytes, int *kilobytes_per_second);

const struct ngcd_backend_ops *ngcd_mock_backend_ops(void);
const struct ngcd_backend_ops *ngcd_target_backend_ops(void);

#endif
