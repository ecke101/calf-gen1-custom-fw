#ifndef CALF_UI_H
#define CALF_UI_H

#include <stddef.h>
#include <stdint.h>

#define CALF_UI_WIDTH 800
#define CALF_UI_HEIGHT 480
#define CALF_IMAGE_LEVEL_COUNT 5
#define CALF_AUDIO_INPUT_VOLUME_COUNT 3
#define CALF_WIFI_MAX_NETWORKS 12
#define CALF_WIFI_SSID_CAPACITY 33
#define CALF_WIFI_PASSWORD_CAPACITY 64
#define CALF_HISTOGRAM_BIN_COUNT 64
#define CALF_POWER_HISTORY_COUNT 360
#define CALF_DRIVE_MODE_COUNT 13

typedef enum {
    CALF_SCREEN_MAIN = 0,
    CALF_SCREEN_EXPOSURE,
    CALF_SCREEN_ISO,
    CALF_SCREEN_LENS,
    CALF_SCREEN_SETTINGS,
    CALF_SCREEN_SETTINGS_CAMERA,
    CALF_SCREEN_SETTINGS_IMAGE,
    CALF_SCREEN_SETTINGS_ENCODING,
    CALF_SCREEN_SETTINGS_RECORDING,
    CALF_SCREEN_SETTINGS_LIVE,
    CALF_SCREEN_SETTINGS_NETWORK,
    CALF_SCREEN_SETTINGS_AUDIO,
    CALF_SCREEN_SETTINGS_STORAGE,
    CALF_SCREEN_SETTINGS_DATETIME,
    CALF_SCREEN_SETTINGS_GENERAL,
    CALF_SCREEN_WHITE_BALANCE,
    CALF_SCREEN_EV,
    CALF_SCREEN_ANTIFLICKER,
    CALF_SCREEN_IMAGE_BRIGHTNESS,
    CALF_SCREEN_IMAGE_CONTRAST,
    CALF_SCREEN_IMAGE_SATURATION,
    CALF_SCREEN_IMAGE_SHARPNESS,
    CALF_SCREEN_IMAGE_DNR,
    CALF_SCREEN_IMAGE_EFFECT,
    CALF_SCREEN_DISPLAY,
    CALF_SCREEN_DISPLAY_OFF,
    CALF_SCREEN_LANGUAGE,
    CALF_SCREEN_INDICATOR_LED,
    CALF_SCREEN_AUDIO_INPUT,
    CALF_SCREEN_AUDIO_BUILTIN_VOLUME,
    CALF_SCREEN_AUDIO_LINEIN_VOLUME,
    CALF_SCREEN_AUDIO_USB_VOLUME,
    CALF_SCREEN_AUDIO_SPEAKER_VOLUME,
    CALF_SCREEN_TIMEZONE,
    CALF_SCREEN_AUTO_TIME,
    CALF_SCREEN_ADJUST_DATETIME,
    CALF_SCREEN_CAPTURE_MODE,
    CALF_SCREEN_CAMERA_RESOLUTION,
    CALF_SCREEN_PHOTO_FORMAT,
    CALF_SCREEN_DRIVE_MODE,
    CALF_SCREEN_ENCODING_CODEC,
    CALF_SCREEN_ENCODING_IMAGE_QUALITY,
    CALF_SCREEN_ENCODING_COLOR_RANGE,
    CALF_SCREEN_RECORDING_CODEC,
    CALF_SCREEN_RECORDING_BITRATE,
    CALF_SCREEN_RECORDING_GOP,
    CALF_SCREEN_RECORDING_COLOR_RANGE,
    CALF_SCREEN_GALLERY,
    CALF_SCREEN_DELETE_CONFIRM,
    CALF_SCREEN_WIFI_LIST,
    CALF_SCREEN_WIFI_PASSWORD,
    CALF_SCREEN_WIFI_OFF_CONFIRM,
    CALF_SCREEN_UPDATE_CONFIRM,
    CALF_SCREEN_STOCK_UI_CONFIRM,
    CALF_SCREEN_POWER_HISTORY,
} calf_screen_t;

typedef enum {
    CALF_ACTION_NONE = 0,
    CALF_ACTION_SET_EXPOSURE,
    CALF_ACTION_SET_ISO,
    CALF_ACTION_SNAPSHOT,
    CALF_ACTION_CAPTURE_SEQUENCE_START,
    CALF_ACTION_CAPTURE_SEQUENCE_CANCEL,
    CALF_ACTION_RECORD_TOGGLE,
    CALF_ACTION_SET_CAMERA_MODE,
    CALF_ACTION_SET_WHITE_BALANCE,
    CALF_ACTION_SET_EV,
    CALF_ACTION_SET_ANTIFLICKER,
    CALF_ACTION_SET_BRIGHTNESS,
    CALF_ACTION_SET_CONTRAST,
    CALF_ACTION_SET_SATURATION,
    CALF_ACTION_SET_SHARPNESS,
    CALF_ACTION_SET_DNR,
    CALF_ACTION_SET_EFFECT,
    CALF_ACTION_SET_BACKLIGHT,
    CALF_ACTION_SET_LCD_POWER,
    CALF_ACTION_SET_DISPLAY_OFF,
    CALF_ACTION_SET_LANGUAGE,
    CALF_ACTION_SET_INDICATOR_LED,
    CALF_ACTION_SET_AUDIO_INPUT,
    CALF_ACTION_SET_BUILTIN_MIC_VOLUME,
    CALF_ACTION_SET_LINEIN_VOLUME,
    CALF_ACTION_SET_USB_MIC_VOLUME,
    CALF_ACTION_SET_SPEAKER_VOLUME,
    CALF_ACTION_SET_TIMEZONE,
    CALF_ACTION_SET_AUTO_TIME,
    CALF_ACTION_SET_DATETIME,
    CALF_ACTION_SET_CAPTURE_MODE,
    CALF_ACTION_SET_RESOLUTION,
    CALF_ACTION_SET_PHOTO_FORMAT,
    CALF_ACTION_SET_DRIVE_MODE,
    CALF_ACTION_SET_ENCODING_CODEC,
    CALF_ACTION_SET_IMAGE_QUALITY,
    CALF_ACTION_SET_ENCODING_COLOR_RANGE,
    CALF_ACTION_SET_RECORDING_CODEC,
    CALF_ACTION_SET_RECORDING_BITRATE,
    CALF_ACTION_SET_RECORDING_GOP,
    CALF_ACTION_SET_RECORDING_COLOR_RANGE,
    CALF_ACTION_GALLERY_ENTER,
    CALF_ACTION_GALLERY_EXIT,
    CALF_ACTION_GALLERY_PREV,
    CALF_ACTION_GALLERY_NEXT,
    CALF_ACTION_GALLERY_PLAY_TOGGLE,
    CALF_ACTION_GALLERY_DELETE,
    CALF_ACTION_WIFI_SCAN,
    CALF_ACTION_WIFI_CONNECT_SAVED,
    CALF_ACTION_WIFI_CONNECT_PASSWORD,
    CALF_ACTION_SET_WIFI_ENABLED,
    CALF_ACTION_FIRMWARE_CHECK,
    CALF_ACTION_FIRMWARE_INSTALL,
    CALF_ACTION_LOAD_STOCK_UI,
} calf_action_kind_t;

typedef enum {
    CALF_CAPTURE_PHOTO = 0,
    CALF_CAPTURE_NIGHT,
    CALF_CAPTURE_VIDEO,
} calf_capture_mode_t;

typedef enum {
    CALF_LANGUAGE_ENGLISH = 0,
    CALF_LANGUAGE_COUNT,
} calf_language_t;

typedef enum {
    CALF_KEY_UP = 0,
    CALF_KEY_DOWN,
    CALF_KEY_LEFT,
    CALF_KEY_RIGHT,
    CALF_KEY_MENU,
    CALF_KEY_BACK,
    CALF_KEY_SHUTTER,
    CALF_KEY_FILE,
    CALF_KEY_F1,
    CALF_KEY_F2,
    CALF_KEY_POWER,
} calf_key_t;

typedef struct {
    calf_action_kind_t kind;
    const char *value;
    int selection;
} calf_action_t;

typedef struct {
    int online;
    int recording;
    int battery_percent;
    int storage_free_mb;
    int system_temp;
    int core_temp;
    int streaming;
    int playback;
    int usb_power;
    int recording_seconds;
} calf_backend_status_t;

/* Battery current and power are positive while the battery supplies the
 * camera and negative while the battery is charging. */
typedef struct {
    int usb_mv;
    int usb_ma;
    int usb_mw;
    int battery_mv;
    int battery_ma;
    int battery_mw;
    int device_mw;
    int recording;
    int valid;
} calf_power_sample_t;

typedef struct {
    char ssid[CALF_WIFI_SSID_CAPACITY];
    int quality;
    int level;
} calf_wifi_network_t;

typedef struct {
    calf_screen_t screen;
    int exposure_index;
    int iso_index;
    int lens_index;
    int white_balance_index;
    int ev_index;
    int antiflicker_index;
    int backlight_index;
    int image_level_index[CALF_IMAGE_LEVEL_COUNT];
    int effect_index;
    int display_off_index;
    int display_off_seconds;
    int language_index;
    int indicator_led_index;
    int audio_input_index;
    int audio_input_volume_index[CALF_AUDIO_INPUT_VOLUME_COUNT];
    int speaker_volume_index;
    int timezone_index;
    int auto_time_index;
    int datetime_year;
    int datetime_month;
    int datetime_day;
    int datetime_hour;
    int datetime_minute;
    int datetime_second;
    calf_capture_mode_t capture_mode;
    int resolution_index;
    int photo_format_index;
    int drive_mode_index;
    int encoding_codec_index;
    int image_quality_index;
    int encoding_color_range_index;
    int recording_codec_index;
    int recording_bitrate_index;
    int recording_gop_index;
    int recording_color_range_index;
    int exposure_known;
    int iso_known;
    int lens_known;
    int white_balance_known;
    int ev_known;
    int antiflicker_known;
    int backlight_known;
    int image_level_known[CALF_IMAGE_LEVEL_COUNT];
    int effect_known;
    int display_off_known;
    int language_known;
    int indicator_led_known;
    int audio_input_known;
    int audio_input_volume_known[CALF_AUDIO_INPUT_VOLUME_COUNT];
    int speaker_volume_known;
    int timezone_known;
    int auto_time_known;
    int datetime_known;
    int resolution_known;
    int photo_format_known;
    int drive_mode_known;
    int encoding_codec_known;
    int image_quality_known;
    int encoding_color_range_known;
    int recording_codec_known;
    int recording_bitrate_known;
    int recording_gop_known;
    int recording_color_range_known;
    int focus_index;
    int focus_visible;
    int return_to_main;
    int lcd_on;
    int pending_selection;
    calf_action_kind_t pending_action;
    calf_backend_status_t status;
    char message[64];
    char datetime_action_value[20];
    char gallery_filename[64];
    int gallery_has_item;
    int gallery_is_video;
    int gallery_index;
    int gallery_count;
    int gallery_playing;
    int gallery_volume_visible;
    int gallery_position_seconds;
    int gallery_duration_seconds;
    int gallery_timing_known;
    int gallery_histogram_visible;
    int gallery_histogram_valid;
    int gallery_zoom_right;
    uint32_t gallery_histogram[CALF_HISTOGRAM_BIN_COUNT];
    const uint32_t *gallery_preview_pixels;
    int gallery_preview_width;
    int gallery_preview_height;
    int live_histogram_visible;
    int live_histogram_valid;
    int live_histogram_error;
    int night_preview_iso;
    int night_preview_clipped;
    int capture_sequence_active;
    int capture_sequence_interval;
    int capture_sequence_sleeping;
    int capture_sequence_remaining_seconds;
    unsigned capture_sequence_shot_count;
    int motion_valid;
    int motion_x;
    int motion_y;
    int motion_z;
    int motion_score;
    int motion_stable_samples;
    int motion_calibration_samples;
    int motion_bias_x;
    int motion_bias_y;
    int motion_bias_z;
    int level_valid;
    int level_x;
    int level_y;
    uint32_t live_histogram[CALF_HISTOGRAM_BIN_COUNT];
    calf_wifi_network_t wifi_networks[CALF_WIFI_MAX_NETWORKS];
    int wifi_network_count;
    int wifi_selected_index;
    int wifi_list_offset;
    int wifi_keyboard_mode;
    char wifi_current_ssid[CALF_WIFI_SSID_CAPACITY];
    char wifi_ip_address[16];
    char wifi_password[CALF_WIFI_PASSWORD_CAPACITY];
    int wifi_enabled;
    int wifi_enabled_known;
    int update_size_mb;
    int update_ready;
    calf_power_sample_t power;
    calf_power_sample_t power_history[CALF_POWER_HISTORY_COUNT];
    int power_history_count;
    int power_history_next;
    int message_is_error;
    uint32_t revision;
} calf_ui_t;

void calf_ui_init(calf_ui_t *ui);
calf_action_t calf_ui_tap(calf_ui_t *ui, int x, int y);
calf_action_t calf_ui_key_press(calf_ui_t *ui, calf_key_t key);
void calf_ui_complete_action(calf_ui_t *ui, calf_action_t action, int success,
                             const char *message);
void calf_ui_set_status(calf_ui_t *ui, const calf_backend_status_t *status);
void calf_ui_set_display_off(calf_ui_t *ui, int selection);
void calf_ui_set_language(calf_ui_t *ui, int selection);
void calf_ui_set_indicator_led(calf_ui_t *ui, int selection);
void calf_ui_set_capture_mode(calf_ui_t *ui, calf_capture_mode_t mode);
int calf_ui_sync_resolution(calf_ui_t *ui, calf_capture_mode_t mode,
                            const char *profile);
void calf_ui_sync_photo_format(calf_ui_t *ui, int raw_enabled);
void calf_ui_set_drive_mode(calf_ui_t *ui, int selection);
void calf_ui_set_capture_sequence(calf_ui_t *ui, int active,
                                  int interval, int sleeping,
                                  int remaining_seconds,
                                  unsigned shot_count);
int calf_ui_sync_encoder_value(calf_ui_t *ui, calf_action_kind_t kind,
                               const char *value);
void calf_ui_set_gallery(calf_ui_t *ui, const char *filename, int is_video,
                         int index, int count, int playing);
void calf_ui_set_gallery_index(calf_ui_t *ui, int index);
void calf_ui_set_gallery_playback(calf_ui_t *ui, int playing,
                                  int position_seconds,
                                  int duration_seconds, int timing_known);
void calf_ui_set_gallery_volume_visible(calf_ui_t *ui, int visible);
void calf_ui_set_gallery_histogram(calf_ui_t *ui,
                                   const uint32_t *bins, int valid);
void calf_ui_set_gallery_preview(calf_ui_t *ui, const uint32_t *pixels,
                                 int width, int height);
void calf_ui_set_live_histogram(calf_ui_t *ui,
                                const uint32_t *bins, int valid);
void calf_ui_set_night_preview(calf_ui_t *ui, int preview_iso, int clipped);
void calf_ui_set_motion(calf_ui_t *ui, int gyro_x, int gyro_y, int gyro_z,
                        int valid);
void calf_ui_set_level(calf_ui_t *ui, int accel_y, int accel_z, int valid);
size_t calf_language_count(void);
const char *calf_language_label(size_t index);
const char *calf_language_value(size_t index);
int calf_language_index_from_value(const char *value);
const char *calf_ui_translate(calf_language_t language, const char *english);
void calf_ui_set_wifi_networks(calf_ui_t *ui,
                               const calf_wifi_network_t *networks, int count,
                               const char *current_ssid,
                               const char *ip_address);
void calf_ui_wifi_require_password(calf_ui_t *ui, int selection);
void calf_ui_set_wifi_enabled(calf_ui_t *ui, int enabled);
void calf_ui_set_wifi_connection(calf_ui_t *ui, const char *current_ssid,
                                 const char *ip_address);
void calf_ui_set_update_ready(calf_ui_t *ui, int size_mb);
int calf_power_decode_bq25703(unsigned adc_vbus_psys,
                             unsigned adc_ibat,
                             unsigned adc_iin_cmpin,
                             unsigned adc_vsys_vbat,
                             unsigned charge_option_1,
                             unsigned adc_option,
                             int recording,
                             calf_power_sample_t *sample);
int calf_power_average_samples(const calf_power_sample_t *samples,
                               size_t count,
                               calf_power_sample_t *average);
void calf_ui_add_power_sample(calf_ui_t *ui,
                              const calf_power_sample_t *sample);
int calf_ui_action_requires_primary(const calf_ui_t *ui,
                                    calf_action_t action);
int calf_ui_sync_image_value(calf_ui_t *ui, calf_action_kind_t kind,
                             const char *value);
int calf_ui_sync_audio_input(calf_ui_t *ui, int automatic, int input_type);
int calf_ui_sync_audio_volume(calf_ui_t *ui, calf_action_kind_t kind,
                              int value);
int calf_ui_sync_speaker_volume(calf_ui_t *ui, int value);
int calf_ui_sync_timezone(calf_ui_t *ui, const char *value);
void calf_ui_sync_auto_time(calf_ui_t *ui, int enabled);
void calf_ui_sync_datetime(calf_ui_t *ui, int year, int month, int day,
                           int hour, int minute, int second);
void calf_ui_notice(calf_ui_t *ui, const char *message, int is_error);
void calf_ui_render(const calf_ui_t *ui, uint32_t *argb, int stride_pixels);

size_t calf_exposure_count(void);
const char *calf_exposure_label(size_t index);
const char *calf_exposure_value(size_t index);
size_t calf_iso_count(void);
const char *calf_iso_label(size_t index);
const char *calf_iso_value(size_t index);
int calf_iso_index_for_value(const char *value);
int calf_ui_exposure_index_for_value(const char *value);
int calf_exposure_allowed(calf_capture_mode_t mode, const char *value);
int calf_iso_allowed(calf_capture_mode_t mode, const char *value);
size_t calf_display_off_count(void);
const char *calf_display_off_label(size_t index);
int calf_display_off_seconds(size_t index);
int calf_display_off_index_from_seconds(int seconds);
size_t calf_drive_mode_count(void);
const char *calf_drive_mode_label(size_t index);
const char *calf_drive_mode_value(size_t index);
int calf_drive_mode_index_from_value(const char *value);
int calf_drive_mode_delay_seconds(size_t index);
int calf_drive_mode_is_interval(size_t index);
int calf_drive_mode_is_burst(size_t index);
unsigned calf_drive_mode_shot_limit(size_t index);

#endif
