#ifndef CALF_UI_INTERNAL_H
#define CALF_UI_INTERNAL_H

#include "calf_ui.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int x;
    int y;
    int w;
    int h;
} rect_t;

typedef struct {
    const char *label;
    const char *value;
} choice_t;

typedef struct {
    const char *label;
    calf_screen_t screen;
} nav_choice_t;

enum {
    IMAGE_LEVEL_BRIGHTNESS = 0,
    IMAGE_LEVEL_CONTRAST,
    IMAGE_LEVEL_SATURATION,
    IMAGE_LEVEL_SHARPNESS,
    IMAGE_LEVEL_DNR,
};

extern const choice_t k_exposures[20];
extern const choice_t k_isos[9];
extern const choice_t k_capture_modes[3];
extern const choice_t k_video_resolutions[9];
extern const choice_t k_photo_resolutions[2];
extern const choice_t k_photo_formats[2];
extern const choice_t k_drive_modes[CALF_DRIVE_MODE_COUNT];
extern const choice_t k_encoding_codecs[2];
extern const choice_t k_image_qualities[4];
extern const choice_t k_color_ranges[2];
extern const choice_t k_recording_codecs[4];
extern const choice_t k_recording_bitrates[10];
extern const choice_t k_recording_gops[6];
extern const choice_t k_white_balances[6];
extern const choice_t k_ev_values[7];
extern const choice_t k_antiflicker_values[4];
extern const choice_t k_backlight_values[26];
extern const choice_t k_image_levels[21];
extern const choice_t k_image_effects[2];
extern const choice_t k_audio_inputs[4];
extern const choice_t k_audio_input_volumes[11];
extern const choice_t k_speaker_volumes[15];
extern const choice_t k_timezones[25];
extern const choice_t k_auto_time_values[2];
extern const choice_t k_display_off_values[8];
extern const choice_t k_languages[CALF_LANGUAGE_COUNT];
extern const choice_t k_indicator_led_values[2];
extern const choice_t k_usb_ethernet_values[5];
extern const nav_choice_t k_settings_categories[10];
extern const char *const k_camera_setting_labels[4];
extern const char *const k_image_setting_labels[9];
extern const char *const k_encoding_setting_labels[3];
extern const char *const k_recording_setting_labels[4];
extern const char *const k_live_setting_labels[4];
extern const char *const k_network_setting_labels[4];
extern const char *const k_audio_setting_labels[5];
extern const char *const k_storage_setting_labels[4];
extern const char *const k_datetime_setting_labels[3];
extern const char *const k_general_setting_labels[8];
extern const char *const k_wifi_keyboard_rows[3];

void bytes_zero(void *destination, size_t length);
size_t text_length(const char *text);
int text_equal(const char *left, const char *right);
void text_copy(char *destination, size_t capacity, const char *source);
void append_text(char *destination, size_t capacity, const char *text);
void append_uint(char *destination, size_t capacity, unsigned value);
void append_int(char *destination, size_t capacity, int value);
void append_padded_uint(char *buffer, size_t capacity, size_t *used,
                        unsigned value, unsigned digits);
int calf_font_text_width(const char *text, int scale);
int calf_font_text_height(int scale);
int calf_font_has_codepoint(uint32_t codepoint);
void calf_font_draw(uint32_t *pixels, int stride, int x, int y,
                    const char *text, int scale, uint32_t color);
rect_t grid_cell(int index, int columns, int top, int height);
rect_t drive_mode_cell(int index);
rect_t main_button_cell(int index);
rect_t wifi_key_cell(int index);
rect_t wifi_special_cell(int index);
void wifi_clear_password(calf_ui_t *ui);
void calf_ui_focus_default(calf_ui_t *ui);
int datetime_days_in_month(int year, int month);
const choice_t *calf_ui_exposure_choices(const calf_ui_t *ui, size_t *count);
int calf_ui_exposure_visible_selection(const calf_ui_t *ui);
int calf_ui_exposure_index_for_value(const char *value);
const choice_t *calf_ui_iso_choices(const calf_ui_t *ui, size_t *count);
int calf_ui_iso_visible_selection(const calf_ui_t *ui);

#endif
