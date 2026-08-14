#ifndef CALF_TARGET_INTERNAL_H
#define CALF_TARGET_INTERNAL_H

#include "calf_ui.h"
#include "target_abi.h"

#define MAX_EVENT_INPUTS 16
#define HTTP_BUFFER_SIZE 16384
#define PROFILE_BUFFER_SIZE 32768
#define GALLERY_PATH_SIZE 256
#define GALLERY_INITIAL_CAPACITY 64
#define INPUT_DEBOUNCE_MS 180u
#define PHYSICAL_BUTTON_QUIET_MS 300u
#define GALLERY_PLAY_DEBOUNCE_MS 750u
#define GALLERY_NAVIGATION_DEBOUNCE_MS 750u
#define GALLERY_VOLUME_OVERLAY_MS 1500u
#define POWER_ADC_POLL_INTERVAL_MS 1000u
#define POWER_SAMPLES_PER_HISTORY_POINT 5u
#define WIFI_STATUS_INTERVAL_MS 15000u
#define WIFI_START_RETRY_INTERVAL_MS 5000u
#define WIFI_START_MAX_ATTEMPTS 3u
#define CAPTURE_SEQUENCE_WAKE_MARGIN_MS 2000u
#define CAPTURE_SEQUENCE_MIN_SLEEP_MS 2000u
#define POWER_REGMAP_PATH "/sys/kernel/debug/regmap/6-006b/registers"

#define DISPLAY_OFF_CONFIG "/local/calf-ui-display-off"
#define DISPLAY_OFF_CONFIG_TEMP "/local/calf-ui-display-off.tmp"
#define LANGUAGE_CONFIG "/local/calf-ui-language"
#define LANGUAGE_CONFIG_TEMP "/local/calf-ui-language.tmp"
#define INDICATOR_LED_CONFIG "/local/calf-ui-indicator-led"
#define INDICATOR_LED_CONFIG_TEMP "/local/calf-ui-indicator-led.tmp"
#define BLUE_LED_TRIGGER "/sys/class/leds/led-blue/trigger"
#define BLUE_LED_BRIGHTNESS "/sys/class/leds/led-blue/brightness"
#define INDICATOR_LED_ENFORCE_INTERVAL_MS 100u
#define AUDIO_INPUT_CONFIG "/local/calf-ui-audio-input"
#define AUDIO_INPUT_CONFIG_TEMP "/local/calf-ui-audio-input.tmp"
#define SPEAKER_VOLUME_CONFIG "/local/calf-ui-speaker-volume"
#define SPEAKER_VOLUME_CONFIG_TEMP "/local/calf-ui-speaker-volume.tmp"
#define RAW_CAPTURE_CONFIG "/local/calf-raw-enabled"
#define RAW_CAPTURE_CONFIG_TEMP "/local/calf-raw-enabled.tmp"
#define DRIVE_MODE_CONFIG "/local/calf-ui-drive-mode"
#define DRIVE_MODE_CONFIG_TEMP "/local/calf-ui-drive-mode.tmp"
#define CAPTURE_MODE_CONFIG "/local/calf-ui-capture-mode"
#define CAPTURE_MODE_CONFIG_TEMP "/local/calf-ui-capture-mode.tmp"
#define PHOTO_EXPOSURE_CONFIG "/local/calf-ui-photo-exposure"
#define PHOTO_EXPOSURE_CONFIG_TEMP "/local/calf-ui-photo-exposure.tmp"
#define PHOTO_ISO_CONFIG "/local/calf-ui-photo-iso"
#define PHOTO_ISO_CONFIG_TEMP "/local/calf-ui-photo-iso.tmp"
#define NIGHT_EXPOSURE_CONFIG "/local/calf-ui-night-exposure"
#define NIGHT_EXPOSURE_CONFIG_TEMP "/local/calf-ui-night-exposure.tmp"
#define NIGHT_ISO_CONFIG "/local/calf-ui-night-iso"
#define NIGHT_ISO_CONFIG_TEMP "/local/calf-ui-night-iso.tmp"
#define TIME_FLOOR_CONFIG "/local/calf-ui-time-floor"
#define TIME_FLOOR_CONFIG_TEMP "/local/calf-ui-time-floor.tmp"
#define STOCK_PROFILE "/local/ngui-profile.yaml"
#define STOCK_PROFILE_TEMP "/local/ngui-profile.yaml.calf-ui.tmp"
#define FIRMWARE_UPDATE_PATH "/mnt/mmcblk1p1/vpupdate.bin"
#define FIRMWARE_UPDATE_IDENTITY_PATH "/mnt/mmcblk1p1/vpupdate.bin.sha256"
#define STOCK_UI_SESSION_MARKER "/tmp/calf-ui-stock-session"
#define NIGHT_PREVIEW_FPS_STATE "/tmp/calf-capture-fps"
#define SUPERVISOR_LOG_PATH "/local/calf-supervisor.log"
#define WIFI_CONNECT_NO_PROFILE (-2)

typedef struct {
    volatile uint32_t *control;
    uint32_t *pixels;
    size_t pixel_bytes;
    int control_fd;
    int pixel_fd;
} display_t;

typedef struct {
    int fd;
    int x;
    int y;
    int pressed;
    int dirty;
    int wake_consumed;
    int tap_armed;
    int last_tap_valid;
    struct timeval last_tap;
} touch_t;

typedef struct {
    int descriptors[MAX_EVENT_INPUTS];
    int count;
    int power_pressed;
    struct timeval power_down;
    uint32_t pressed_keys;
    uint32_t timed_keys;
    struct timeval last_key_press[CALF_KEY_POWER + 1];
    int button_release_valid;
    struct timeval last_button_release;
} keys_t;

typedef struct {
    int automatic;
    int input_type;
    int volume[CALF_AUDIO_INPUT_VOLUME_COUNT];
    int valid;
} audio_input_state_t;

typedef struct {
    char timezone[16];
    int automatic;
} time_settings_t;

typedef struct {
    char photo[32];
    char video[32];
    calf_capture_mode_t mode;
} camera_profiles_t;

typedef struct {
    int active;
    int interval;
    int burst;
    int sleeping;
    int deep_idle_enabled;
    int overrun;
    unsigned interval_seconds;
    unsigned shot_limit;
    unsigned shot_count;
    uint64_t next_capture_ms;
} capture_sequence_t;

typedef struct {
    char **paths;
    int count;
    int capacity;
    int index;
    int active;
    int graph_suspended;
    int playback_started;
    int playing;
    uint32_t *preview_pixels;
    int preview_width;
    int preview_height;
    uint64_t play_ready_ms;
} gallery_state_t;

size_t string_length(const char *text);
void string_copy(char *destination, size_t capacity, const char *source);
int string_equal(const char *left, const char *right);
const char *find_text(const char *text, const char *needle);
void buffer_append(char *buffer, size_t capacity, size_t *used,
                   const char *text);
void buffer_append_uint(char *buffer, size_t capacity, size_t *used,
                        unsigned value);
int http_request(const char *method, const char *path, const char *body,
                 char *response, size_t response_capacity);
int parse_integer_after(const char *text, const char *key, int *value);
int parse_milliseconds_after(const char *text, const char *key, int *value);
int parse_scalar_after(const char *text, const char *key, char *value,
                       size_t capacity);
int response_code_ok(const char *response);
int read_exact(int descriptor, unsigned char *buffer, size_t count);
int target_write_atomic_file(const char *destination, const char *temporary,
                             const char *text, size_t length,
                             unsigned int mode, int owner, int group);
int target_power_parse_registers(const char *registers, int recording,
                                 calf_power_sample_t *sample);
int target_read_power_sample(calf_power_sample_t *sample, int recording);
int firmware_update_validate(int *size_mb, char digest[65]);
int firmware_update_validate_paths(const char *archive_path,
                                   const char *identity_path,
                                   int *size_mb, char digest[65]);
int api_post_action(const char *path, const char *body);
int api_stop_camera_graph(void);

void capture_sequence_init(capture_sequence_t *sequence);
int capture_sequence_start(capture_sequence_t *sequence,
                           int drive_mode_index, uint64_t now_ms);
void capture_sequence_cancel(capture_sequence_t *sequence);
int capture_sequence_capture_due(const capture_sequence_t *sequence,
                                 uint64_t now_ms);
int capture_sequence_should_sleep(const capture_sequence_t *sequence,
                                  uint64_t now_ms);
int capture_sequence_should_wake(const capture_sequence_t *sequence,
                                 uint64_t now_ms);
void capture_sequence_set_sleeping(capture_sequence_t *sequence,
                                   int sleeping);
void capture_sequence_complete_capture(capture_sequence_t *sequence,
                                       int success, uint64_t completed_ms);
int capture_sequence_remaining_seconds(const capture_sequence_t *sequence,
                                       uint64_t now_ms);
int night_preview_fps_for_exposure(const char *exposure);
const char *night_preview_exposure_for_fps(int fps);
int night_image_action_is_transient(calf_capture_mode_t mode,
                                    calf_action_kind_t kind);

void gallery_init(gallery_state_t *gallery);
void gallery_destroy(gallery_state_t *gallery);
int gallery_add_path(gallery_state_t *gallery, const char *path);
int gallery_sort_paths(gallery_state_t *gallery);
void gallery_remove_path(gallery_state_t *gallery, int index);
int gallery_path_is_video(const char *path);
void gallery_clear_preview(calf_ui_t *ui, gallery_state_t *gallery);
void gallery_sync_ui(calf_ui_t *ui, const gallery_state_t *gallery);
void api_poll_gallery_info(calf_ui_t *ui, gallery_state_t *gallery);
int gallery_enter_backend(gallery_state_t *gallery);
int gallery_close_backend(gallery_state_t *gallery);
int gallery_offset_index(const gallery_state_t *gallery, int offset);
int gallery_move(gallery_state_t *gallery, int offset);
int gallery_toggle_playback(gallery_state_t *gallery);
int gallery_delete_current(gallery_state_t *gallery);

extern volatile int g_running;
int display_open(display_t *display);
void display_present(display_t *display, const calf_ui_t *ui);
void display_clear(display_t *display);
void display_close(display_t *display);
int touch_open(touch_t *touch);
int touch_read_action(touch_t *touch, calf_ui_t *ui,
                      calf_action_t *action, int suppress_actions,
                      int allow_wake, int *activity);
int keys_open(keys_t *keys);
int keys_read_action(keys_t *keys, int descriptor, calf_ui_t *ui,
                     calf_action_t *action, int suppress_actions,
                     int allow_wake, int *short_power, int *long_power,
                     int *activity);

#endif
