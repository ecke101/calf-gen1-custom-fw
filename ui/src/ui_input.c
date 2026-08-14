#include "ui_internal.h"

static int contains(rect_t rectangle, int x, int y)
{
    return x >= rectangle.x && y >= rectangle.y &&
           x < rectangle.x + rectangle.w && y < rectangle.y + rectangle.h;
}

static calf_action_t no_action(void)
{
    calf_action_t action;
    action.kind = CALF_ACTION_NONE;
    action.value = (const char *)0;
    action.selection = -1;
    return action;
}

static calf_action_t begin_action(calf_ui_t *ui, calf_action_kind_t kind,
                                  const char *value, int selection)
{
    calf_action_t action;
    action.kind = kind;
    action.value = value;
    action.selection = selection;
    ui->pending_action = kind;
    ui->pending_selection = selection;
    ui->message_is_error = 0;
    text_copy(ui->message, sizeof(ui->message), "APPLYING");
    ++ui->revision;
    return action;
}

static calf_action_t begin_quiet_action(calf_ui_t *ui,
                                        calf_action_kind_t kind,
                                        const char *value, int selection)
{
    calf_action_t action = begin_action(ui, kind, value, selection);
    /* The target renders pending actions before making the backend request. */
    text_copy(ui->message, sizeof(ui->message), "");
    return action;
}

static calf_action_t record_toggle_action(calf_ui_t *ui)
{
    int stopping = ui->status.recording != 0;
    calf_action_t action = begin_action(
        ui, CALF_ACTION_RECORD_TOGGLE, stopping ? "stop" : "start", -1);
    if(stopping)
        text_copy(ui->message, sizeof(ui->message), "SAVING");
    return action;
}

static calf_action_t guarded_setting_action(calf_ui_t *ui,
                                            calf_action_kind_t kind,
                                            const char *value,
                                            int selection);

rect_t grid_cell(int index, int columns, int top, int height)
{
    const int gap = 12;
    const int margin = 12;
    rect_t rectangle;
    rectangle.w = (CALF_UI_WIDTH - margin * 2 - gap * (columns - 1)) / columns;
    rectangle.h = height;
    rectangle.x = margin + (index % columns) * (rectangle.w + gap);
    rectangle.y = top + (index / columns) * (height + gap);
    return rectangle;
}

rect_t drive_mode_cell(int index)
{
    const int option_left = 184;
    const int option_width = 604;
    const int gap = 8;
    int row;
    int column;
    int columns;
    rect_t rectangle;
    if(index <= 0) {
        row = 0;
        column = 0;
        columns = 1;
    }
    else if(index <= 3) {
        row = 1;
        column = index - 1;
        columns = 3;
    }
    else if(index <= 6) {
        row = 2;
        column = index - 4;
        columns = 3;
    }
    else {
        row = 3;
        column = index - 7;
        columns = 6;
    }
    rectangle.w = (option_width - gap * (columns - 1)) / columns;
    rectangle.h = 80;
    rectangle.x = option_left + column * (rectangle.w + gap);
    rectangle.y = 84 + row * 92;
    return rectangle;
}

static int drive_mode_move_focus(int current, calf_key_t key)
{
    static const int row_starts[] = {0, 1, 4, 7};
    static const int row_counts[] = {1, 3, 3, 6};
    int row = current == 0 ? 0 : current <= 3 ? 1 : current <= 6 ? 2 : 3;
    int column = current - row_starts[row];
    int target_row;
    int target_column;
    if(key == CALF_KEY_LEFT && column > 0) return current - 1;
    if(key == CALF_KEY_RIGHT && column + 1 < row_counts[row])
        return current + 1;
    if(key != CALF_KEY_UP && key != CALF_KEY_DOWN) return current;
    target_row = row + (key == CALF_KEY_UP ? -1 : 1);
    if(target_row < 0 || target_row >= (int)ARRAY_SIZE(row_starts))
        return current;
    target_column = ((column * 2 + 1) * row_counts[target_row]) /
                    (row_counts[row] * 2);
    if(target_column >= row_counts[target_row])
        target_column = row_counts[target_row] - 1;
    return row_starts[target_row] + target_column;
}

rect_t main_button_cell(int index)
{
    rect_t rectangle;
    rectangle.x = 8 + index * 158 + (index > 2 ? 2 : 0);
    rectangle.y = 414;
    rectangle.w = index == 2 ? 152 : 150;
    rectangle.h = 58;
    return rectangle;
}

rect_t wifi_key_cell(int index)
{
    const int gap = 4;
    const int margin = 8;
    rect_t rectangle;
    rectangle.w = (CALF_UI_WIDTH - margin * 2 - gap * 9) / 10;
    rectangle.h = 48;
    rectangle.x = margin + (index % 10) * (rectangle.w + gap);
    rectangle.y = 166 + (index / 10) * 54;
    return rectangle;
}

rect_t wifi_special_cell(int index)
{
    static const int widths[] = {150, 226, 150, 226};
    rect_t rectangle = {8, 390, widths[index], 78};
    int item;
    for(item = 0; item < index; ++item)
        rectangle.x += widths[item] + 8;
    return rectangle;
}

void wifi_clear_password(calf_ui_t *ui)
{
    bytes_zero(ui->wifi_password, sizeof(ui->wifi_password));
}

static void wifi_password_character(calf_ui_t *ui, char character)
{
    size_t length = text_length(ui->wifi_password);
    if(length + 1 >= sizeof(ui->wifi_password)) return;
    ui->wifi_password[length] = character;
    ui->wifi_password[length + 1] = '\0';
    ++ui->revision;
}

static void wifi_password_backspace(calf_ui_t *ui)
{
    size_t length = text_length(ui->wifi_password);
    if(length == 0) return;
    ui->wifi_password[length - 1] = '\0';
    ++ui->revision;
}

static calf_action_t wifi_password_connect(calf_ui_t *ui)
{
    size_t length = text_length(ui->wifi_password);
    if(ui->wifi_selected_index < 0 ||
       ui->wifi_selected_index >= ui->wifi_network_count) {
        calf_ui_notice(ui, "SELECT NETWORK AGAIN", 1);
        return no_action();
    }
    if(length != 0 && length < 8) {
        calf_ui_notice(ui, "PASSWORD: 8 TO 63 CHARS", 1);
        return no_action();
    }
    return begin_action(
        ui, CALF_ACTION_WIFI_CONNECT_PASSWORD,
        ui->wifi_networks[ui->wifi_selected_index].ssid,
        ui->wifi_selected_index);
}

static calf_screen_t parent_screen(calf_screen_t screen)
{
    if(screen == CALF_SCREEN_CAPTURE_MODE ||
       screen == CALF_SCREEN_CAMERA_RESOLUTION ||
       screen == CALF_SCREEN_PHOTO_FORMAT ||
       screen == CALF_SCREEN_DRIVE_MODE)
        return CALF_SCREEN_SETTINGS_CAMERA;
    if(screen >= CALF_SCREEN_ENCODING_CODEC &&
       screen <= CALF_SCREEN_ENCODING_COLOR_RANGE)
        return CALF_SCREEN_SETTINGS_ENCODING;
    if(screen >= CALF_SCREEN_RECORDING_CODEC &&
       screen <= CALF_SCREEN_RECORDING_COLOR_RANGE)
        return CALF_SCREEN_SETTINGS_RECORDING;
    if(screen == CALF_SCREEN_GALLERY ||
       screen == CALF_SCREEN_DELETE_CONFIRM)
        return CALF_SCREEN_MAIN;
    if(screen == CALF_SCREEN_WIFI_LIST)
        return CALF_SCREEN_SETTINGS_NETWORK;
    if(screen == CALF_SCREEN_WIFI_PASSWORD)
        return CALF_SCREEN_WIFI_LIST;
    if(screen == CALF_SCREEN_WIFI_OFF_CONFIRM)
        return CALF_SCREEN_SETTINGS_NETWORK;
    if(screen == CALF_SCREEN_UPDATE_CONFIRM)
        return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_STOCK_UI_CONFIRM)
        return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_POWER_HISTORY)
        return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_WHITE_BALANCE || screen == CALF_SCREEN_EV ||
       screen == CALF_SCREEN_ANTIFLICKER ||
       screen == CALF_SCREEN_IMAGE_BRIGHTNESS ||
       screen == CALF_SCREEN_IMAGE_CONTRAST ||
       screen == CALF_SCREEN_IMAGE_SATURATION ||
       screen == CALF_SCREEN_IMAGE_SHARPNESS ||
       screen == CALF_SCREEN_IMAGE_DNR || screen == CALF_SCREEN_IMAGE_EFFECT)
        return CALF_SCREEN_SETTINGS_IMAGE;
    if(screen == CALF_SCREEN_DISPLAY) return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_DISPLAY_OFF) return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_LANGUAGE) return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_INDICATOR_LED)
        return CALF_SCREEN_SETTINGS_GENERAL;
    if(screen == CALF_SCREEN_AUDIO_INPUT ||
       (screen >= CALF_SCREEN_AUDIO_BUILTIN_VOLUME &&
        screen <= CALF_SCREEN_AUDIO_SPEAKER_VOLUME))
        return CALF_SCREEN_SETTINGS_AUDIO;
    if(screen == CALF_SCREEN_TIMEZONE || screen == CALF_SCREEN_AUTO_TIME ||
       screen == CALF_SCREEN_ADJUST_DATETIME)
        return CALF_SCREEN_SETTINGS_DATETIME;
    if(screen >= CALF_SCREEN_SETTINGS_CAMERA &&
       screen <= CALF_SCREEN_SETTINGS_GENERAL)
        return CALF_SCREEN_SETTINGS;
    return CALF_SCREEN_MAIN;
}

static int screen_grid(const calf_ui_t *ui, calf_screen_t screen,
                       int *count, int *columns,
                       int *top, int *height)
{
    if(screen == CALF_SCREEN_EXPOSURE) {
        size_t exposure_count;
        (void)calf_ui_exposure_choices(ui, &exposure_count);
        *count = (int)exposure_count;
        *columns = ui->capture_mode == CALF_CAPTURE_NIGHT ? 3 : 4;
        *top = 88; *height = 108;
    }
    else if(screen == CALF_SCREEN_ISO) {
        size_t iso_count;
        (void)calf_ui_iso_choices(ui, &iso_count);
        *count = (int)iso_count;
        *columns = ui->capture_mode == CALF_CAPTURE_NIGHT ? 4 : 3;
        *top = 88; *height = 108;
    }
    else if(screen == CALF_SCREEN_SETTINGS) {
        *count = (int)ARRAY_SIZE(k_settings_categories); *columns = 2;
        *top = 80; *height = 70;
    }
    else if(screen == CALF_SCREEN_SETTINGS_CAMERA) {
        *count = (int)ARRAY_SIZE(k_camera_setting_labels); *columns = 2;
        *top = 112; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_IMAGE) {
        *count = (int)ARRAY_SIZE(k_image_setting_labels); *columns = 3;
        *top = 80; *height = 116;
    }
    else if(screen == CALF_SCREEN_SETTINGS_ENCODING) {
        *count = (int)ARRAY_SIZE(k_encoding_setting_labels); *columns = 2;
        *top = 112; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_RECORDING) {
        *count = (int)ARRAY_SIZE(k_recording_setting_labels); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_LIVE) {
        *count = (int)ARRAY_SIZE(k_live_setting_labels); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_NETWORK) {
        *count = (int)ARRAY_SIZE(k_network_setting_labels); *columns = 2;
        *top = 112; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_AUDIO) {
        *count = (int)ARRAY_SIZE(k_audio_setting_labels); *columns = 2;
        *top = 82; *height = 116;
    }
    else if(screen == CALF_SCREEN_SETTINGS_STORAGE) {
        *count = (int)ARRAY_SIZE(k_storage_setting_labels); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_DATETIME) {
        *count = (int)ARRAY_SIZE(k_datetime_setting_labels); *columns = 2;
        *top = 112; *height = 142;
    }
    else if(screen == CALF_SCREEN_SETTINGS_GENERAL) {
        *count = (int)ARRAY_SIZE(k_general_setting_labels); *columns = 2;
        *top = 80; *height = 82;
    }
    else if(screen == CALF_SCREEN_WHITE_BALANCE) {
        *count = (int)ARRAY_SIZE(k_white_balances); *columns = 3;
        *top = 88; *height = 156;
    }
    else if(screen == CALF_SCREEN_EV) {
        *count = (int)ARRAY_SIZE(k_ev_values); *columns = 4;
        *top = 88; *height = 156;
    }
    else if(screen == CALF_SCREEN_ANTIFLICKER) {
        *count = (int)ARRAY_SIZE(k_antiflicker_values); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen >= CALF_SCREEN_IMAGE_BRIGHTNESS &&
            screen <= CALF_SCREEN_IMAGE_DNR) {
        *count = (int)ARRAY_SIZE(k_image_levels); *columns = 7;
        *top = 80; *height = 108;
    }
    else if(screen == CALF_SCREEN_IMAGE_EFFECT) {
        *count = (int)ARRAY_SIZE(k_image_effects); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_DISPLAY) {
        *count = (int)ARRAY_SIZE(k_backlight_values); *columns = 7;
        *top = 80; *height = 82;
    }
    else if(screen == CALF_SCREEN_DISPLAY_OFF) {
        *count = (int)ARRAY_SIZE(k_display_off_values); *columns = 2;
        *top = 80; *height = 82;
    }
    else if(screen == CALF_SCREEN_LANGUAGE) {
        *count = (int)ARRAY_SIZE(k_languages); *columns = 1;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_INDICATOR_LED) {
        *count = (int)ARRAY_SIZE(k_indicator_led_values); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_AUDIO_INPUT) {
        *count = (int)ARRAY_SIZE(k_audio_inputs); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen >= CALF_SCREEN_AUDIO_BUILTIN_VOLUME &&
            screen <= CALF_SCREEN_AUDIO_USB_VOLUME) {
        *count = (int)ARRAY_SIZE(k_audio_input_volumes); *columns = 4;
        *top = 88; *height = 108;
    }
    else if(screen == CALF_SCREEN_AUDIO_SPEAKER_VOLUME) {
        *count = (int)ARRAY_SIZE(k_speaker_volumes); *columns = 4;
        *top = 80; *height = 82;
    }
    else if(screen == CALF_SCREEN_TIMEZONE) {
        *count = (int)ARRAY_SIZE(k_timezones); *columns = 5;
        *top = 80; *height = 68;
    }
    else if(screen == CALF_SCREEN_AUTO_TIME) {
        *count = (int)ARRAY_SIZE(k_auto_time_values); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_CAPTURE_MODE) {
        *count = (int)ARRAY_SIZE(k_capture_modes); *columns = 3;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_CAMERA_RESOLUTION) {
        *count = ui->capture_mode == CALF_CAPTURE_VIDEO
                     ? (int)ARRAY_SIZE(k_video_resolutions)
                     : (int)ARRAY_SIZE(k_photo_resolutions);
        *columns = 2;
        *top = ui->capture_mode == CALF_CAPTURE_VIDEO ? 80 : 126;
        *height = ui->capture_mode == CALF_CAPTURE_VIDEO ? 70 : 218;
    }
    else if(screen == CALF_SCREEN_PHOTO_FORMAT) {
        *count = (int)ARRAY_SIZE(k_photo_formats); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_DRIVE_MODE) {
        *count = (int)ARRAY_SIZE(k_drive_modes); *columns = 3;
        *top = 80; *height = 82;
    }
    else if(screen == CALF_SCREEN_ENCODING_CODEC) {
        *count = (int)ARRAY_SIZE(k_encoding_codecs); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_ENCODING_IMAGE_QUALITY) {
        *count = (int)ARRAY_SIZE(k_image_qualities); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen == CALF_SCREEN_ENCODING_COLOR_RANGE ||
            screen == CALF_SCREEN_RECORDING_COLOR_RANGE) {
        *count = (int)ARRAY_SIZE(k_color_ranges); *columns = 2;
        *top = 126; *height = 218;
    }
    else if(screen == CALF_SCREEN_RECORDING_CODEC) {
        *count = (int)ARRAY_SIZE(k_recording_codecs); *columns = 2;
        *top = 108; *height = 142;
    }
    else if(screen == CALF_SCREEN_RECORDING_BITRATE) {
        *count = (int)ARRAY_SIZE(k_recording_bitrates); *columns = 2;
        *top = 80; *height = 70;
    }
    else if(screen == CALF_SCREEN_RECORDING_GOP) {
        *count = (int)ARRAY_SIZE(k_recording_gops); *columns = 3;
        *top = 108; *height = 142;
    }
    else return 0;
    return 1;
}

static int selected_focus_index(const calf_ui_t *ui)
{
    if(ui->screen == CALF_SCREEN_EXPOSURE && ui->exposure_known)
        return calf_ui_exposure_visible_selection(ui);
    if(ui->screen == CALF_SCREEN_ISO && ui->iso_known)
        return ui->iso_index;
    if(ui->screen == CALF_SCREEN_EV && ui->ev_known)
        return ui->ev_index;
    if(ui->screen == CALF_SCREEN_WHITE_BALANCE &&
       ui->white_balance_known)
        return ui->white_balance_index;
    if(ui->screen == CALF_SCREEN_ANTIFLICKER && ui->antiflicker_known)
        return ui->antiflicker_index;
    if(ui->screen == CALF_SCREEN_IMAGE_EFFECT && ui->effect_known)
        return ui->effect_index;
    if(ui->screen == CALF_SCREEN_DISPLAY && ui->backlight_known)
        return ui->backlight_index;
    if(ui->screen == CALF_SCREEN_DISPLAY_OFF && ui->display_off_known)
        return ui->display_off_index;
    if(ui->screen == CALF_SCREEN_LANGUAGE && ui->language_known)
        return ui->language_index;
    if(ui->screen == CALF_SCREEN_INDICATOR_LED && ui->indicator_led_known)
        return ui->indicator_led_index;
    if(ui->screen >= CALF_SCREEN_IMAGE_BRIGHTNESS &&
       ui->screen <= CALF_SCREEN_IMAGE_DNR) {
        int level = (int)ui->screen - (int)CALF_SCREEN_IMAGE_BRIGHTNESS;
        if(ui->image_level_known[level])
            return ui->image_level_index[level];
    }
    if(ui->screen == CALF_SCREEN_AUDIO_INPUT && ui->audio_input_known)
        return ui->audio_input_index;
    if(ui->screen == CALF_SCREEN_TIMEZONE && ui->timezone_known)
        return ui->timezone_index;
    if(ui->screen == CALF_SCREEN_AUTO_TIME && ui->auto_time_known)
        return ui->auto_time_index;
    if(ui->screen == CALF_SCREEN_CAPTURE_MODE)
        return (int)ui->capture_mode;
    if(ui->screen == CALF_SCREEN_CAMERA_RESOLUTION && ui->resolution_known)
        return ui->resolution_index;
    if(ui->screen == CALF_SCREEN_PHOTO_FORMAT && ui->photo_format_known)
        return ui->photo_format_index;
    if(ui->screen == CALF_SCREEN_DRIVE_MODE && ui->drive_mode_known)
        return ui->drive_mode_index;
    if(ui->screen == CALF_SCREEN_ENCODING_CODEC && ui->encoding_codec_known)
        return ui->encoding_codec_index;
    if(ui->screen == CALF_SCREEN_ENCODING_IMAGE_QUALITY &&
       ui->image_quality_known)
        return ui->image_quality_index;
    if(ui->screen == CALF_SCREEN_ENCODING_COLOR_RANGE &&
       ui->encoding_color_range_known)
        return ui->encoding_color_range_index;
    if(ui->screen == CALF_SCREEN_RECORDING_CODEC &&
       ui->recording_codec_known)
        return ui->recording_codec_index;
    if(ui->screen == CALF_SCREEN_RECORDING_BITRATE &&
       ui->recording_bitrate_known)
        return ui->recording_bitrate_index;
    if(ui->screen == CALF_SCREEN_RECORDING_GOP && ui->recording_gop_known)
        return ui->recording_gop_index;
    if(ui->screen == CALF_SCREEN_RECORDING_COLOR_RANGE &&
       ui->recording_color_range_known)
        return ui->recording_color_range_index;
    if(ui->screen >= CALF_SCREEN_AUDIO_BUILTIN_VOLUME &&
       ui->screen <= CALF_SCREEN_AUDIO_USB_VOLUME) {
        int input = (int)ui->screen -
                    (int)CALF_SCREEN_AUDIO_BUILTIN_VOLUME;
        if(ui->audio_input_volume_known[input])
            return ui->audio_input_volume_index[input];
    }
    if(ui->screen == CALF_SCREEN_AUDIO_SPEAKER_VOLUME &&
       ui->speaker_volume_known)
        return ui->speaker_volume_index;
    return 0;
}

void calf_ui_focus_default(calf_ui_t *ui)
{
    int count;
    int columns;
    int top;
    int height;
    int focus;
    int index;

    ui->focus_index = 0;
    ui->focus_visible = 0;

    if(ui->screen == CALF_SCREEN_LENS) {
        ui->focus_index = ui->lens_known ? ui->lens_index : 0;
        ui->focus_visible = 1;
        return;
    }
    if(ui->screen == CALF_SCREEN_ADJUST_DATETIME ||
       ui->screen == CALF_SCREEN_WIFI_PASSWORD ||
       ui->screen == CALF_SCREEN_DELETE_CONFIRM ||
       ui->screen == CALF_SCREEN_WIFI_OFF_CONFIRM ||
       ui->screen == CALF_SCREEN_UPDATE_CONFIRM) {
        ui->focus_visible = 1;
        return;
    }
    if(ui->screen == CALF_SCREEN_WIFI_LIST) {
        for(index = 0; index < ui->wifi_network_count; ++index) {
            if(text_equal(ui->wifi_networks[index].ssid,
                          ui->wifi_current_ssid)) {
                ui->focus_index = index;
                if(index >= ui->wifi_list_offset + 5)
                    ui->wifi_list_offset = index - 4;
                break;
            }
        }
        ui->focus_visible = 1;
        return;
    }
    if(!screen_grid(ui, ui->screen, &count, &columns, &top, &height))
        return;
    (void)columns;
    (void)top;
    (void)height;
    focus = selected_focus_index(ui);
    if(focus < 0 || focus >= count) focus = 0;
    ui->focus_index = focus;
    ui->focus_visible = 1;
}

static void change_screen(calf_ui_t *ui, calf_screen_t screen)
{
    ui->screen = screen;
    calf_ui_focus_default(ui);
    text_copy(ui->message, sizeof(ui->message), "");
    ++ui->revision;
}

int datetime_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
    int result;
    if(month < 1 || month > 12) return 31;
    result = days[month - 1];
    if(month == 2 && (year % 4 == 0) &&
       (year % 100 != 0 || year % 400 == 0))
        result = 29;
    return result;
}

static void datetime_adjust(calf_ui_t *ui, int field, int direction)
{
    int limit;
    if(field == 0) {
        ui->datetime_year += direction;
        if(ui->datetime_year < 2020) ui->datetime_year = 2049;
        if(ui->datetime_year > 2049) ui->datetime_year = 2020;
    }
    else if(field == 1) {
        ui->datetime_month += direction;
        if(ui->datetime_month < 1) ui->datetime_month = 12;
        if(ui->datetime_month > 12) ui->datetime_month = 1;
    }
    else if(field == 2) {
        limit = datetime_days_in_month(ui->datetime_year, ui->datetime_month);
        ui->datetime_day += direction;
        if(ui->datetime_day < 1) ui->datetime_day = limit;
        if(ui->datetime_day > limit) ui->datetime_day = 1;
    }
    else if(field == 3) {
        ui->datetime_hour += direction;
        if(ui->datetime_hour < 0) ui->datetime_hour = 23;
        if(ui->datetime_hour > 23) ui->datetime_hour = 0;
    }
    else if(field == 4) {
        ui->datetime_minute += direction;
        if(ui->datetime_minute < 0) ui->datetime_minute = 59;
        if(ui->datetime_minute > 59) ui->datetime_minute = 0;
    }
    else if(field == 5) {
        ui->datetime_second += direction;
        if(ui->datetime_second < 0) ui->datetime_second = 59;
        if(ui->datetime_second > 59) ui->datetime_second = 0;
    }
    limit = datetime_days_in_month(ui->datetime_year, ui->datetime_month);
    if(ui->datetime_day > limit) ui->datetime_day = limit;
    ++ui->revision;
}

void append_padded_uint(char *buffer, size_t capacity, size_t *used,
                        unsigned value, unsigned digits)
{
    unsigned divisor = 1;
    unsigned index;
    for(index = 1; index < digits; ++index) divisor *= 10;
    for(index = 0; index < digits; ++index) {
        char one[2];
        one[0] = (char)('0' + (value / divisor) % 10);
        one[1] = '\0';
        append_text(buffer, capacity, one);
        ++*used;
        if(divisor > 1) divisor /= 10;
    }
}

static calf_action_t datetime_apply(calf_ui_t *ui)
{
    size_t used = 0;
    ui->datetime_action_value[0] = '\0';
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_year, 4);
    append_text(ui->datetime_action_value,
                sizeof(ui->datetime_action_value), "-"); ++used;
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_month, 2);
    append_text(ui->datetime_action_value,
                sizeof(ui->datetime_action_value), "-"); ++used;
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_day, 2);
    append_text(ui->datetime_action_value,
                sizeof(ui->datetime_action_value), "T"); ++used;
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_hour, 2);
    append_text(ui->datetime_action_value,
                sizeof(ui->datetime_action_value), ":"); ++used;
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_minute, 2);
    append_text(ui->datetime_action_value,
                sizeof(ui->datetime_action_value), ":"); ++used;
    append_padded_uint(ui->datetime_action_value,
                       sizeof(ui->datetime_action_value), &used,
                       (unsigned)ui->datetime_second, 2);
    return begin_action(ui, CALF_ACTION_SET_DATETIME,
                        ui->datetime_action_value, -1);
}

static calf_action_t activate_focus(calf_ui_t *ui)
{
    int count;
    int columns;
    int top;
    int height;
    rect_t rectangle;
    int focus = ui->focus_index;
    calf_screen_t old_screen = ui->screen;
    calf_action_t action;
    if(ui->screen == CALF_SCREEN_LENS) {
        if(focus < 0 || focus >= (int)ARRAY_SIZE(k_lenses)) focus = 0;
        rectangle = (rect_t){16 + focus * 262, 126, 244, 218};
    }
    else if(ui->screen == CALF_SCREEN_DRIVE_MODE) {
        if(focus < 0 || focus >= (int)ARRAY_SIZE(k_drive_modes)) focus = 0;
        rectangle = drive_mode_cell(focus);
    }
    else {
        if(!screen_grid(ui, ui->screen, &count, &columns, &top, &height))
            return no_action();
        if(focus < 0 || focus >= count) focus = 0;
        rectangle = grid_cell(focus, columns, top, height);
    }
    action = calf_ui_tap(ui, rectangle.x + rectangle.w / 2,
                         rectangle.y + rectangle.h / 2);
    if(ui->screen == old_screen) {
        ui->focus_index = focus;
        ui->focus_visible = 1;
    }
    else calf_ui_focus_default(ui);
    return action;
}

static calf_action_t gallery_enter_action(calf_ui_t *ui)
{
    if(ui->status.recording) {
        calf_ui_notice(ui, "STOP RECORDING FIRST", 1);
        return no_action();
    }
    if(!ui->status.online) {
        calf_ui_notice(ui, "STATUS UNKNOWN", 1);
        return no_action();
    }
    if(ui->status.streaming != 0) {
        calf_ui_notice(ui, ui->status.streaming > 0
                               ? "STOP LIVE FIRST" : "LIVE STATUS UNKNOWN",
                       1);
        return no_action();
    }
    if(ui->status.playback != 0) {
        calf_ui_notice(ui, ui->status.playback > 0
                               ? "STOP PLAYBACK FIRST"
                               : "PLAY STATUS UNKNOWN",
                       1);
        return no_action();
    }
    return begin_action(ui, CALF_ACTION_GALLERY_ENTER,
                        (const char *)0, -1);
}

calf_action_t calf_ui_key_press(calf_ui_t *ui, calf_key_t key)
{
    int count;
    int columns;
    int top;
    int height;
    int next;
    if(ui->pending_action != CALF_ACTION_NONE) return no_action();

    if(key == CALF_KEY_POWER)
        return begin_action(ui, CALF_ACTION_SET_LCD_POWER,
                            ui->lcd_on ? "turn_off" : "turn_on",
                            ui->lcd_on ? 0 : 1);

    if(ui->capture_sequence_active) {
        if(key == CALF_KEY_SHUTTER || key == CALF_KEY_BACK)
            return begin_quiet_action(
                ui, CALF_ACTION_CAPTURE_SEQUENCE_CANCEL,
                (const char *)0, -1);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_DELETE_CONFIRM) {
        if(key == CALF_KEY_BACK) {
            change_screen(ui, CALF_SCREEN_GALLERY);
            return no_action();
        }
        if(key == CALF_KEY_LEFT || key == CALF_KEY_RIGHT) {
            ui->focus_index = key == CALF_KEY_RIGHT ? 1 : 0;
            ui->focus_visible = 1;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_MENU || key == CALF_KEY_SHUTTER) {
            if(!ui->focus_visible || ui->focus_index == 0) {
                change_screen(ui, CALF_SCREEN_GALLERY);
                return no_action();
            }
            return begin_action(ui, CALF_ACTION_GALLERY_DELETE,
                                (const char *)0, -1);
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_GALLERY) {
        if(key == CALF_KEY_BACK)
            return begin_action(ui, CALF_ACTION_GALLERY_EXIT,
                                (const char *)0, -1);
        if(!ui->gallery_has_item) return no_action();
        if(ui->gallery_is_video && ui->gallery_playing &&
           (key == CALF_KEY_UP || key == CALF_KEY_DOWN)) {
            int selection;
            if(!ui->speaker_volume_known) {
                calf_ui_notice(ui, "VOLUME UNKNOWN", 1);
                return no_action();
            }
            selection = ui->speaker_volume_index +
                        (key == CALF_KEY_UP ? 1 : -1);
            if(selection < 0) selection = 0;
            if(selection >= (int)ARRAY_SIZE(k_speaker_volumes))
                selection = (int)ARRAY_SIZE(k_speaker_volumes) - 1;
            if(selection == ui->speaker_volume_index) return no_action();
            return begin_quiet_action(ui, CALF_ACTION_SET_SPEAKER_VOLUME,
                                      k_speaker_volumes[selection].value,
                                      selection);
        }
        if(key == CALF_KEY_UP) {
            ui->gallery_histogram_visible =
                !ui->gallery_histogram_visible;
            if(ui->gallery_histogram_visible)
                ui->gallery_histogram_valid = 0;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_DOWN) {
            ui->gallery_zoom_right = !ui->gallery_zoom_right;
            if(ui->gallery_histogram_visible)
                ui->gallery_histogram_valid = 0;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_LEFT)
            return begin_action(ui, CALF_ACTION_GALLERY_PREV,
                                (const char *)0, -1);
        if(key == CALF_KEY_RIGHT)
            return begin_action(ui, CALF_ACTION_GALLERY_NEXT,
                                (const char *)0, -1);
        if((key == CALF_KEY_MENU || key == CALF_KEY_SHUTTER) &&
           ui->gallery_is_video)
            return begin_quiet_action(ui,
                                      CALF_ACTION_GALLERY_PLAY_TOGGLE,
                                      (const char *)0, -1);
        if(key == CALF_KEY_FILE) {
            change_screen(ui, CALF_SCREEN_DELETE_CONFIRM);
            ui->focus_index = 0;
            ui->focus_visible = 1;
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_UPDATE_CONFIRM) {
        if(key == CALF_KEY_BACK) {
            change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
            return no_action();
        }
        if(key == CALF_KEY_LEFT || key == CALF_KEY_RIGHT) {
            ui->focus_index = key == CALF_KEY_RIGHT ? 1 : 0;
            ui->focus_visible = 1;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_MENU) {
            if(!ui->focus_visible || ui->focus_index == 0) {
                change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
                return no_action();
            }
            return begin_action(ui, CALF_ACTION_FIRMWARE_INSTALL,
                                (const char *)0, -1);
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_STOCK_UI_CONFIRM) {
        if(key == CALF_KEY_BACK) {
            change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
            return no_action();
        }
        if(key == CALF_KEY_LEFT || key == CALF_KEY_RIGHT) {
            ui->focus_index = key == CALF_KEY_RIGHT ? 1 : 0;
            ui->focus_visible = 1;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_MENU) {
            if(!ui->focus_visible || ui->focus_index == 0) {
                change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
                return no_action();
            }
            return guarded_setting_action(
                ui, CALF_ACTION_LOAD_STOCK_UI, (const char *)0, -1);
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_OFF_CONFIRM) {
        if(key == CALF_KEY_BACK) {
            change_screen(ui, CALF_SCREEN_SETTINGS_NETWORK);
            return no_action();
        }
        if(key == CALF_KEY_LEFT || key == CALF_KEY_RIGHT) {
            ui->focus_index = key == CALF_KEY_RIGHT ? 1 : 0;
            ui->focus_visible = 1;
            ++ui->revision;
            return no_action();
        }
        if(key == CALF_KEY_MENU) {
            if(!ui->focus_visible || ui->focus_index == 0) {
                change_screen(ui, CALF_SCREEN_SETTINGS_NETWORK);
                return no_action();
            }
            return begin_action(ui, CALF_ACTION_SET_WIFI_ENABLED, "0", 0);
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_LIST) {
        int count = ui->wifi_network_count + 1;
        int focus = ui->focus_visible ? ui->focus_index : 0;
        if(key == CALF_KEY_BACK) {
            change_screen(ui, CALF_SCREEN_SETTINGS_NETWORK);
            return no_action();
        }
        if(key == CALF_KEY_UP || key == CALF_KEY_LEFT) {
            if(focus > 0) --focus;
        }
        else if(key == CALF_KEY_DOWN || key == CALF_KEY_RIGHT) {
            if(focus + 1 < count) ++focus;
        }
        else if(key == CALF_KEY_MENU) {
            if(focus == ui->wifi_network_count)
                return begin_action(ui, CALF_ACTION_WIFI_SCAN,
                                    (const char *)0, -1);
            if(focus >= 0 && focus < ui->wifi_network_count) {
                ui->wifi_selected_index = focus;
                return begin_action(
                    ui, CALF_ACTION_WIFI_CONNECT_SAVED,
                    ui->wifi_networks[focus].ssid, focus);
            }
            return no_action();
        }
        else return no_action();
        ui->focus_index = focus;
        ui->focus_visible = 1;
        if(focus < ui->wifi_list_offset) ui->wifi_list_offset = focus;
        if(focus >= ui->wifi_list_offset + 5)
            ui->wifi_list_offset = focus - 4;
        ++ui->revision;
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_PASSWORD) {
        int focus = ui->focus_visible ? ui->focus_index : 0;
        if(key == CALF_KEY_BACK) {
            wifi_clear_password(ui);
            change_screen(ui, CALF_SCREEN_WIFI_LIST);
            return no_action();
        }
        if(key == CALF_KEY_LEFT) {
            if(focus > 0) --focus;
        }
        else if(key == CALF_KEY_RIGHT) {
            if(focus < 43) ++focus;
        }
        else if(key == CALF_KEY_UP) {
            if(focus >= 40)
                focus = 30 + (focus - 40) * 3;
            else if(focus >= 10) focus -= 10;
        }
        else if(key == CALF_KEY_DOWN) {
            if(focus < 30) focus += 10;
            else if(focus < 40) focus = 40 + (focus - 30) * 4 / 10;
        }
        else if(key == CALF_KEY_MENU) {
            if(focus < 40) {
                wifi_password_character(
                    ui, k_wifi_keyboard_rows[ui->wifi_keyboard_mode][focus]);
                return no_action();
            }
            if(focus == 40) {
                ui->wifi_keyboard_mode = (ui->wifi_keyboard_mode + 1) % 3;
                ++ui->revision;
                return no_action();
            }
            if(focus == 41) {
                wifi_password_character(ui, ' ');
                return no_action();
            }
            if(focus == 42) {
                wifi_password_backspace(ui);
                return no_action();
            }
            return wifi_password_connect(ui);
        }
        else return no_action();
        ui->focus_index = focus;
        ui->focus_visible = 1;
        ++ui->revision;
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_MAIN) {
        if(key == CALF_KEY_UP) {
            change_screen(ui, CALF_SCREEN_ISO); ui->return_to_main = 1;
        }
        else if(key == CALF_KEY_DOWN) {
            change_screen(ui, CALF_SCREEN_EXPOSURE); ui->return_to_main = 1;
        }
        else if(key == CALF_KEY_LEFT) {
            change_screen(ui, CALF_SCREEN_EV); ui->return_to_main = 1;
        }
        else if(key == CALF_KEY_RIGHT) {
            change_screen(ui, CALF_SCREEN_WHITE_BALANCE); ui->return_to_main = 1;
        }
        else if(key == CALF_KEY_MENU) change_screen(ui, CALF_SCREEN_SETTINGS);
        else if(key == CALF_KEY_FILE)
            return gallery_enter_action(ui);
        else if(key == CALF_KEY_F1) {
            ui->live_histogram_visible = !ui->live_histogram_visible;
            if(ui->live_histogram_visible) {
                ui->live_histogram_valid = 0;
                ui->live_histogram_error = 0;
            }
            ++ui->revision;
        }
        else if(key == CALF_KEY_SHUTTER)
            return ui->capture_mode == CALF_CAPTURE_VIDEO
                       ? record_toggle_action(ui)
                       : begin_action(
                             ui,
                             ui->drive_mode_known && ui->drive_mode_index != 0
                                 ? CALF_ACTION_CAPTURE_SEQUENCE_START
                                 : CALF_ACTION_SNAPSHOT,
                             (const char *)0, -1);
        return no_action();
    }

    if(key == CALF_KEY_BACK) {
        change_screen(ui, ui->return_to_main ? CALF_SCREEN_MAIN
                                             : parent_screen(ui->screen));
        ui->return_to_main = 0;
        return no_action();
    }
    if(ui->screen == CALF_SCREEN_ADJUST_DATETIME) {
        int focus = ui->focus_visible ? ui->focus_index : 0;
        if(key == CALF_KEY_UP && focus > 0) --focus;
        else if(key == CALF_KEY_DOWN && focus < 6) ++focus;
        else if(key == CALF_KEY_LEFT && focus < 6)
            datetime_adjust(ui, focus, -1);
        else if(key == CALF_KEY_RIGHT && focus < 6)
            datetime_adjust(ui, focus, 1);
        else if(key == CALF_KEY_MENU) {
            if(focus == 6) return datetime_apply(ui);
            datetime_adjust(ui, focus, 1);
        }
        ui->focus_index = focus;
        ui->focus_visible = 1;
        ++ui->revision;
        return no_action();
    }
    if(key == CALF_KEY_MENU) return activate_focus(ui);
    if(key != CALF_KEY_UP && key != CALF_KEY_DOWN &&
       key != CALF_KEY_LEFT && key != CALF_KEY_RIGHT)
        return no_action();

    if(ui->screen == CALF_SCREEN_DRIVE_MODE) {
        next = ui->focus_visible ? ui->focus_index
                                 : selected_focus_index(ui);
        if(next < 0 || next >= (int)ARRAY_SIZE(k_drive_modes)) next = 0;
        ui->focus_index = drive_mode_move_focus(next, key);
        ui->focus_visible = 1;
        ++ui->revision;
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_LENS) {
        count = (int)ARRAY_SIZE(k_lenses);
        columns = 3;
    }
    else if(!screen_grid(ui, ui->screen, &count, &columns, &top, &height))
        return no_action();
    (void)top;
    (void)height;
    next = ui->focus_visible ? ui->focus_index : selected_focus_index(ui);
    if(next < 0 || next >= count) next = 0;
    if(key == CALF_KEY_LEFT && next > 0) --next;
    else if(key == CALF_KEY_RIGHT && next + 1 < count) ++next;
    else if(key == CALF_KEY_UP && next >= columns) next -= columns;
    else if(key == CALF_KEY_DOWN && next + columns < count) next += columns;
    ui->focus_index = next;
    ui->focus_visible = 1;
    ++ui->revision;
    return no_action();
}

static calf_action_t camera_mode_action(calf_ui_t *ui, const char *value,
                                        int selection)
{
    if(!ui->status.online) {
        calf_ui_notice(ui, "STATUS UNKNOWN", 1);
        return no_action();
    }
    if(ui->status.recording) {
        calf_ui_notice(ui, "STOP RECORDING FIRST", 1);
        return no_action();
    }
    if(ui->status.streaming != 0) {
        calf_ui_notice(ui, ui->status.streaming > 0
                               ? "STOP LIVE FIRST" : "LIVE STATUS UNKNOWN",
                       1);
        return no_action();
    }
    if(ui->status.playback != 0) {
        calf_ui_notice(ui, ui->status.playback > 0
                               ? "STOP PLAYBACK FIRST"
                               : "PLAY STATUS UNKNOWN",
                       1);
        return no_action();
    }
    return begin_action(ui, CALF_ACTION_SET_CAMERA_MODE, value, selection);
}

static calf_action_t guarded_setting_action(calf_ui_t *ui,
                                            calf_action_kind_t kind,
                                            const char *value,
                                            int selection)
{
    if(!ui->status.online) {
        calf_ui_notice(ui, "STATUS UNKNOWN", 1);
        return no_action();
    }
    if(ui->status.recording) {
        calf_ui_notice(ui, "STOP RECORDING FIRST", 1);
        return no_action();
    }
    if(ui->status.streaming != 0) {
        calf_ui_notice(ui, ui->status.streaming > 0
                               ? "STOP LIVE FIRST" : "LIVE STATUS UNKNOWN",
                       1);
        return no_action();
    }
    if(ui->status.playback != 0) {
        calf_ui_notice(ui, ui->status.playback > 0
                               ? "STOP PLAYBACK FIRST"
                               : "PLAY STATUS UNKNOWN",
                       1);
        return no_action();
    }
    return begin_action(ui, kind, value, selection);
}

calf_action_t calf_ui_tap(calf_ui_t *ui, int x, int y)
{
    int i;
    if(ui->pending_action != CALF_ACTION_NONE) return no_action();
    ui->focus_visible = 0;

    if(ui->capture_sequence_active) {
        if(ui->screen == CALF_SCREEN_MAIN &&
           contains(main_button_cell(3), x, y))
            return begin_quiet_action(
                ui, CALF_ACTION_CAPTURE_SEQUENCE_CANCEL,
                (const char *)0, -1);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_GALLERY) {
        if(contains((rect_t){12, 10, 112, 58}, x, y))
            return begin_action(ui, CALF_ACTION_GALLERY_EXIT,
                                (const char *)0, -1);
        if(!ui->gallery_has_item) return no_action();
        if(contains((rect_t){8, 414, 190, 58}, x, y))
            return begin_action(ui, CALF_ACTION_GALLERY_PREV,
                                (const char *)0, -1);
        if(contains((rect_t){206, 414, 190, 58}, x, y) &&
           ui->gallery_is_video)
            return begin_quiet_action(ui,
                                      CALF_ACTION_GALLERY_PLAY_TOGGLE,
                                      (const char *)0, -1);
        if(contains((rect_t){404, 414, 190, 58}, x, y))
            return begin_action(ui, CALF_ACTION_GALLERY_NEXT,
                                (const char *)0, -1);
        if(contains((rect_t){602, 414, 190, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_DELETE_CONFIRM);
            ui->focus_index = 0;
            ui->focus_visible = 1;
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_DELETE_CONFIRM) {
        if(contains((rect_t){148, 300, 236, 92}, x, y) ||
           contains((rect_t){12, 10, 112, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_GALLERY);
            return no_action();
        }
        if(contains((rect_t){416, 300, 236, 92}, x, y))
            return begin_action(ui, CALF_ACTION_GALLERY_DELETE,
                                (const char *)0, -1);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_UPDATE_CONFIRM) {
        if(contains((rect_t){148, 316, 236, 92}, x, y) ||
           contains((rect_t){12, 10, 112, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
            return no_action();
        }
        if(contains((rect_t){416, 316, 236, 92}, x, y))
            return begin_action(ui, CALF_ACTION_FIRMWARE_INSTALL,
                                (const char *)0, -1);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_STOCK_UI_CONFIRM) {
        if(contains((rect_t){148, 316, 236, 92}, x, y) ||
           contains((rect_t){12, 10, 112, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_SETTINGS_GENERAL);
            return no_action();
        }
        if(contains((rect_t){416, 316, 236, 92}, x, y))
            return guarded_setting_action(
                ui, CALF_ACTION_LOAD_STOCK_UI, (const char *)0, -1);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_OFF_CONFIRM) {
        if(contains((rect_t){148, 316, 236, 92}, x, y) ||
           contains((rect_t){12, 10, 112, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_SETTINGS_NETWORK);
            return no_action();
        }
        if(contains((rect_t){416, 316, 236, 92}, x, y))
            return begin_action(ui, CALF_ACTION_SET_WIFI_ENABLED, "0", 0);
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_LIST) {
        if(contains((rect_t){12, 10, 112, 58}, x, y)) {
            change_screen(ui, CALF_SCREEN_SETTINGS_NETWORK);
            return no_action();
        }
        for(i = 0; i < 5; ++i) {
            int index = ui->wifi_list_offset + i;
            if(index < ui->wifi_network_count &&
               contains((rect_t){12, 126 + i * 56, 776, 50}, x, y)) {
                ui->wifi_selected_index = index;
                return begin_action(
                    ui, CALF_ACTION_WIFI_CONNECT_SAVED,
                    ui->wifi_networks[index].ssid, index);
            }
        }
        if(contains((rect_t){12, 414, 238, 54}, x, y)) {
            ui->wifi_list_offset = ui->wifi_list_offset >= 5
                                       ? ui->wifi_list_offset - 5 : 0;
            ++ui->revision;
            return no_action();
        }
        if(contains((rect_t){270, 414, 260, 54}, x, y))
            return begin_action(ui, CALF_ACTION_WIFI_SCAN,
                                (const char *)0, -1);
        if(contains((rect_t){550, 414, 238, 54}, x, y)) {
            int maximum = ui->wifi_network_count > 5
                              ? ui->wifi_network_count - 5 : 0;
            ui->wifi_list_offset += 5;
            if(ui->wifi_list_offset > maximum)
                ui->wifi_list_offset = maximum;
            ++ui->revision;
            return no_action();
        }
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_WIFI_PASSWORD) {
        if(contains((rect_t){12, 10, 112, 58}, x, y)) {
            wifi_clear_password(ui);
            change_screen(ui, CALF_SCREEN_WIFI_LIST);
            return no_action();
        }
        for(i = 0; i < 40; ++i) {
            if(contains(wifi_key_cell(i), x, y)) {
                wifi_password_character(
                    ui, k_wifi_keyboard_rows[ui->wifi_keyboard_mode][i]);
                return no_action();
            }
        }
        for(i = 0; i < 4; ++i) {
            if(!contains(wifi_special_cell(i), x, y)) continue;
            if(i == 0) {
                ui->wifi_keyboard_mode = (ui->wifi_keyboard_mode + 1) % 3;
                ++ui->revision;
            }
            else if(i == 1) wifi_password_character(ui, ' ');
            else if(i == 2) wifi_password_backspace(ui);
            else return wifi_password_connect(ui);
            return no_action();
        }
        return no_action();
    }

    if(ui->screen != CALF_SCREEN_MAIN && contains((rect_t){12, 10, 112, 58}, x, y)) {
        change_screen(ui, ui->return_to_main ? CALF_SCREEN_MAIN
                                             : parent_screen(ui->screen));
        ui->return_to_main = 0;
        return no_action();
    }

    if(ui->screen == CALF_SCREEN_MAIN) {
        const rect_t settings = main_button_cell(0);
        const rect_t zoom = main_button_cell(1);
        const rect_t histogram = main_button_cell(2);
        const rect_t mode_or_record = main_button_cell(3);
        const rect_t record = main_button_cell(4);
        if(contains(settings, x, y)) {
            change_screen(ui, CALF_SCREEN_SETTINGS);
            return no_action();
        }
        if(contains((rect_t){0, 0, 133, 72}, x, y)) {
            change_screen(ui, CALF_SCREEN_WHITE_BALANCE);
            ui->return_to_main = 1;
            return no_action();
        }
        if(contains((rect_t){133, 0, 133, 72}, x, y)) {
            change_screen(ui, CALF_SCREEN_EV);
            ui->return_to_main = 1;
            return no_action();
        }
        if(contains((rect_t){266, 0, 133, 72}, x, y)) {
            change_screen(ui, CALF_SCREEN_EXPOSURE);
            return no_action();
        }
        if(contains((rect_t){399, 0, 133, 72}, x, y)) {
            change_screen(ui, CALF_SCREEN_ISO);
            return no_action();
        }
        if(contains(zoom, x, y))
            return camera_mode_action(
                ui, ui->lens_known && ui->lens_index == 0
                        ? k_lenses[1].value : k_lenses[0].value,
                ui->lens_known && ui->lens_index == 0 ? 1 : 0);
        if(contains(histogram, x, y)) {
            ui->live_histogram_visible = !ui->live_histogram_visible;
            if(ui->live_histogram_visible) {
                ui->live_histogram_valid = 0;
                ui->live_histogram_error = 0;
            }
            ++ui->revision;
            return no_action();
        }
        if(contains(mode_or_record, x, y)) {
            if(ui->capture_mode == CALF_CAPTURE_VIDEO)
                return record_toggle_action(ui);
            change_screen(ui, CALF_SCREEN_DRIVE_MODE);
            ui->return_to_main = 1;
            return no_action();
        }
        if(contains(record, x, y)) {
            change_screen(ui, CALF_SCREEN_CAPTURE_MODE);
            ui->return_to_main = 1;
            return no_action();
        }
    }
    else if(ui->screen == CALF_SCREEN_EXPOSURE) {
        const choice_t *choices;
        size_t count;
        int columns = ui->capture_mode == CALF_CAPTURE_NIGHT ? 3 : 4;
        choices = calf_ui_exposure_choices(ui, &count);
        for(i = 0; i < (int)count; ++i) {
            if(contains(grid_cell(i, columns, 88, 108), x, y))
                return begin_action(ui, CALF_ACTION_SET_EXPOSURE,
                                    choices[i].value,
                                    calf_ui_exposure_index_for_value(
                                        choices[i].value));
        }
    }
    else if(ui->screen == CALF_SCREEN_ISO) {
        const choice_t *choices;
        size_t count;
        int columns = ui->capture_mode == CALF_CAPTURE_NIGHT ? 4 : 3;
        choices = calf_ui_iso_choices(ui, &count);
        for(i = 0; i < (int)count; ++i) {
            if(contains(grid_cell(i, columns, 88, 108), x, y))
                return begin_action(ui, CALF_ACTION_SET_ISO,
                                    choices[i].value,
                                    calf_iso_index_for_value(choices[i].value));
        }
    }
    else if(ui->screen == CALF_SCREEN_LENS) {
        for(i = 0; i < (int)ARRAY_SIZE(k_lenses); ++i) {
            rect_t rectangle = {16 + i * 262, 126, 244, 218};
            if(contains(rectangle, x, y))
                return camera_mode_action(ui, k_lenses[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS) {
        for(i = 0; i < (int)ARRAY_SIZE(k_settings_categories); ++i) {
            if(contains(grid_cell(i, 2, 80, 70), x, y)) {
                change_screen(ui, k_settings_categories[i].screen);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_CAMERA) {
        if(contains(grid_cell(0, 2, 112, 142), x, y)) {
            change_screen(ui, CALF_SCREEN_CAPTURE_MODE);
            return no_action();
        }
        if(contains(grid_cell(1, 2, 112, 142), x, y))
            change_screen(ui, CALF_SCREEN_CAMERA_RESOLUTION);
        if(contains(grid_cell(2, 2, 112, 142), x, y))
            change_screen(ui, CALF_SCREEN_PHOTO_FORMAT);
        if(contains(grid_cell(3, 2, 112, 142), x, y))
            change_screen(ui, CALF_SCREEN_DRIVE_MODE);
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_IMAGE) {
        static const calf_screen_t screens[] = {
            CALF_SCREEN_WHITE_BALANCE, CALF_SCREEN_EV,
            CALF_SCREEN_IMAGE_BRIGHTNESS, CALF_SCREEN_IMAGE_CONTRAST,
            CALF_SCREEN_IMAGE_SATURATION, CALF_SCREEN_IMAGE_SHARPNESS,
            CALF_SCREEN_IMAGE_DNR, CALF_SCREEN_ANTIFLICKER,
            CALF_SCREEN_IMAGE_EFFECT,
        };
        for(i = 0; i < (int)ARRAY_SIZE(screens); ++i) {
            if(contains(grid_cell(i, 3, 80, 116), x, y)) {
                change_screen(ui, screens[i]);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_ENCODING) {
        static const calf_screen_t screens[] = {
            CALF_SCREEN_ENCODING_CODEC,
            CALF_SCREEN_ENCODING_IMAGE_QUALITY,
            CALF_SCREEN_ENCODING_COLOR_RANGE,
        };
        for(i = 0; i < (int)ARRAY_SIZE(screens); ++i) {
            if(contains(grid_cell(i, 2, 112, 142), x, y)) {
                change_screen(ui, screens[i]);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_RECORDING) {
        static const calf_screen_t screens[] = {
            CALF_SCREEN_RECORDING_CODEC, CALF_SCREEN_RECORDING_BITRATE,
            CALF_SCREEN_RECORDING_GOP, CALF_SCREEN_RECORDING_COLOR_RANGE,
        };
        for(i = 0; i < (int)ARRAY_SIZE(screens); ++i) {
            if(contains(grid_cell(i, 2, 108, 142), x, y)) {
                change_screen(ui, screens[i]);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_NETWORK) {
        if(contains(grid_cell(0, 2, 112, 142), x, y)) {
            if(ui->wifi_enabled_known && !ui->wifi_enabled) {
                calf_ui_notice(ui, "TURN WI-FI ON FIRST", 0);
                return no_action();
            }
            return begin_action(ui, CALF_ACTION_WIFI_SCAN,
                                (const char *)0, -1);
        }
        if(contains(grid_cell(1, 2, 112, 142), x, y)) {
            if(ui->wifi_enabled_known && ui->wifi_enabled) {
                change_screen(ui, CALF_SCREEN_WIFI_OFF_CONFIRM);
                ui->focus_index = 0;
                ui->focus_visible = 1;
                return no_action();
            }
            return begin_action(ui, CALF_ACTION_SET_WIFI_ENABLED, "1", 1);
        }
        if(y >= 80) calf_ui_notice(ui, "COMING NEXT", 0);
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_GENERAL) {
        if(contains(grid_cell(0, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_DISPLAY);
            return no_action();
        }
        if(contains(grid_cell(1, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_DISPLAY_OFF);
            return no_action();
        }
        if(contains(grid_cell(2, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_LANGUAGE);
            return no_action();
        }
        if(contains(grid_cell(3, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_INDICATOR_LED);
            return no_action();
        }
        if(contains(grid_cell(4, 2, 80, 82), x, y))
            return begin_action(ui, CALF_ACTION_FIRMWARE_CHECK,
                                (const char *)0, -1);
        if(contains(grid_cell(6, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_POWER_HISTORY);
            return no_action();
        }
        if(contains(grid_cell(7, 2, 80, 82), x, y)) {
            change_screen(ui, CALF_SCREEN_STOCK_UI_CONFIRM);
            ui->focus_index = 0;
            ui->focus_visible = 1;
            return no_action();
        }
        for(i = 5; i < 8; ++i) {
            if(i == 6 || i == 7) continue;
            if(contains(grid_cell(i, 2, 80, 82), x, y)) {
                calf_ui_notice(ui, "COMING NEXT", 0);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_LANGUAGE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_languages); ++i) {
            if(contains(grid_cell(i, 1, 126, 218), x, y))
                return begin_action(ui, CALF_ACTION_SET_LANGUAGE,
                                    k_languages[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_INDICATOR_LED) {
        for(i = 0; i < (int)ARRAY_SIZE(k_indicator_led_values); ++i) {
            if(contains(grid_cell(i, 2, 126, 218), x, y))
                return begin_action(ui, CALF_ACTION_SET_INDICATOR_LED,
                                    k_indicator_led_values[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_AUDIO) {
        static const calf_screen_t screens[] = {
            CALF_SCREEN_AUDIO_INPUT,
            CALF_SCREEN_AUDIO_BUILTIN_VOLUME,
            CALF_SCREEN_AUDIO_LINEIN_VOLUME,
            CALF_SCREEN_AUDIO_USB_VOLUME,
            CALF_SCREEN_AUDIO_SPEAKER_VOLUME,
        };
        for(i = 0; i < (int)ARRAY_SIZE(screens); ++i) {
            if(contains(grid_cell(i, 2, 82, 116), x, y)) {
                change_screen(ui, screens[i]);
                return no_action();
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_SETTINGS_DATETIME) {
        if(contains(grid_cell(0, 2, 112, 142), x, y)) {
            change_screen(ui, CALF_SCREEN_TIMEZONE);
            return no_action();
        }
        if(contains(grid_cell(1, 2, 112, 142), x, y)) {
            change_screen(ui, CALF_SCREEN_AUTO_TIME);
            return no_action();
        }
        if(contains(grid_cell(2, 2, 112, 142), x, y)) {
            if(ui->auto_time_known && ui->auto_time_index != 0) {
                calf_ui_notice(ui, "TURN AUTO SET OFF", 0);
                return no_action();
            }
            change_screen(ui, CALF_SCREEN_ADJUST_DATETIME);
            return no_action();
        }
    }
    else if(ui->screen >= CALF_SCREEN_SETTINGS_CAMERA &&
            ui->screen <= CALF_SCREEN_SETTINGS_DATETIME) {
        if(y >= 80) calf_ui_notice(ui, "COMING NEXT", 0);
    }
    else if(ui->screen == CALF_SCREEN_WHITE_BALANCE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_white_balances); ++i) {
            if(contains(grid_cell(i, 3, 88, 156), x, y))
                return begin_action(ui, CALF_ACTION_SET_WHITE_BALANCE,
                                    k_white_balances[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_EV) {
        for(i = 0; i < (int)ARRAY_SIZE(k_ev_values); ++i) {
            if(contains(grid_cell(i, 4, 88, 156), x, y))
                return begin_action(ui, CALF_ACTION_SET_EV,
                                    k_ev_values[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_ANTIFLICKER) {
        for(i = 0; i < (int)ARRAY_SIZE(k_antiflicker_values); ++i) {
            if(contains(grid_cell(i, 2, 108, 142), x, y))
                return begin_action(ui, CALF_ACTION_SET_ANTIFLICKER,
                                    k_antiflicker_values[i].value, i);
        }
    }
    else if(ui->screen >= CALF_SCREEN_IMAGE_BRIGHTNESS &&
            ui->screen <= CALF_SCREEN_IMAGE_DNR) {
        static const calf_action_kind_t actions[] = {
            CALF_ACTION_SET_BRIGHTNESS, CALF_ACTION_SET_CONTRAST,
            CALF_ACTION_SET_SATURATION, CALF_ACTION_SET_SHARPNESS,
            CALF_ACTION_SET_DNR,
        };
        int level = (int)ui->screen - (int)CALF_SCREEN_IMAGE_BRIGHTNESS;
        for(i = 0; i < (int)ARRAY_SIZE(k_image_levels); ++i) {
            if(contains(grid_cell(i, 7, 80, 108), x, y))
                return begin_action(ui, actions[level], k_image_levels[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_IMAGE_EFFECT) {
        for(i = 0; i < (int)ARRAY_SIZE(k_image_effects); ++i) {
            if(contains(grid_cell(i, 2, 126, 218), x, y))
                return begin_action(ui, CALF_ACTION_SET_EFFECT,
                                    k_image_effects[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_DISPLAY) {
        for(i = 0; i < (int)ARRAY_SIZE(k_backlight_values); ++i) {
            if(contains(grid_cell(i, 7, 80, 82), x, y))
                return begin_action(ui, CALF_ACTION_SET_BACKLIGHT,
                                    k_backlight_values[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_DISPLAY_OFF) {
        for(i = 0; i < (int)ARRAY_SIZE(k_display_off_values); ++i) {
            if(contains(grid_cell(i, 2, 80, 82), x, y))
                return begin_action(ui, CALF_ACTION_SET_DISPLAY_OFF,
                                    k_display_off_values[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_AUDIO_INPUT) {
        for(i = 0; i < (int)ARRAY_SIZE(k_audio_inputs); ++i) {
            if(contains(grid_cell(i, 2, 108, 142), x, y))
                return begin_action(ui, CALF_ACTION_SET_AUDIO_INPUT,
                                    k_audio_inputs[i].value, i);
        }
    }
    else if(ui->screen >= CALF_SCREEN_AUDIO_BUILTIN_VOLUME &&
            ui->screen <= CALF_SCREEN_AUDIO_USB_VOLUME) {
        int input = (int)ui->screen -
                    (int)CALF_SCREEN_AUDIO_BUILTIN_VOLUME;
        calf_action_kind_t kind =
            (calf_action_kind_t)((int)CALF_ACTION_SET_BUILTIN_MIC_VOLUME +
                                 input);
        for(i = 0; i < (int)ARRAY_SIZE(k_audio_input_volumes); ++i) {
            if(contains(grid_cell(i, 4, 88, 108), x, y))
                return begin_action(ui, kind,
                                    k_audio_input_volumes[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_AUDIO_SPEAKER_VOLUME) {
        for(i = 0; i < (int)ARRAY_SIZE(k_speaker_volumes); ++i) {
            if(contains(grid_cell(i, 4, 80, 82), x, y))
                return begin_action(ui, CALF_ACTION_SET_SPEAKER_VOLUME,
                                    k_speaker_volumes[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_TIMEZONE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_timezones); ++i) {
            if(contains(grid_cell(i, 5, 80, 68), x, y))
                return begin_action(ui, CALF_ACTION_SET_TIMEZONE,
                                    k_timezones[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_AUTO_TIME) {
        for(i = 0; i < (int)ARRAY_SIZE(k_auto_time_values); ++i) {
            if(contains(grid_cell(i, 2, 126, 218), x, y))
                return begin_action(ui, CALF_ACTION_SET_AUTO_TIME,
                                    k_auto_time_values[i].value, i);
        }
    }
    else if(ui->screen == CALF_SCREEN_CAMERA_RESOLUTION) {
        const choice_t *choices = ui->capture_mode == CALF_CAPTURE_VIDEO
                                      ? k_video_resolutions
                                      : k_photo_resolutions;
        int count = ui->capture_mode == CALF_CAPTURE_VIDEO
                        ? (int)ARRAY_SIZE(k_video_resolutions)
                        : (int)ARRAY_SIZE(k_photo_resolutions);
        int top = ui->capture_mode == CALF_CAPTURE_VIDEO ? 80 : 126;
        int height = ui->capture_mode == CALF_CAPTURE_VIDEO ? 70 : 218;
        for(i = 0; i < count; ++i) {
            if(contains(grid_cell(i, 2, top, height), x, y)) {
                if(ui->resolution_known && ui->resolution_index == i) {
                    change_screen(ui, CALF_SCREEN_SETTINGS_CAMERA);
                    return no_action();
                }
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_RESOLUTION,
                    choices[i].value, i);
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_PHOTO_FORMAT) {
        for(i = 0; i < (int)ARRAY_SIZE(k_photo_formats); ++i) {
            if(contains(grid_cell(i, 2, 126, 218), x, y)) {
                if(ui->photo_format_known && ui->photo_format_index == i) {
                    change_screen(ui, CALF_SCREEN_SETTINGS_CAMERA);
                    return no_action();
                }
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_PHOTO_FORMAT,
                    k_photo_formats[i].value, i);
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_DRIVE_MODE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_drive_modes); ++i) {
            if(contains(drive_mode_cell(i), x, y)) {
                if(ui->drive_mode_known && ui->drive_mode_index == i) {
                    change_screen(ui, ui->return_to_main
                                          ? CALF_SCREEN_MAIN
                                          : CALF_SCREEN_SETTINGS_CAMERA);
                    if(ui->screen == CALF_SCREEN_MAIN)
                        ui->return_to_main = 0;
                    return no_action();
                }
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_DRIVE_MODE,
                    k_drive_modes[i].value, i);
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_ENCODING_CODEC) {
        for(i = 0; i < (int)ARRAY_SIZE(k_encoding_codecs); ++i)
            if(contains(grid_cell(i, 2, 126, 218), x, y))
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_ENCODING_CODEC,
                    k_encoding_codecs[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_ENCODING_IMAGE_QUALITY) {
        for(i = 0; i < (int)ARRAY_SIZE(k_image_qualities); ++i)
            if(contains(grid_cell(i, 2, 108, 142), x, y))
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_IMAGE_QUALITY,
                    k_image_qualities[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_ENCODING_COLOR_RANGE ||
            ui->screen == CALF_SCREEN_RECORDING_COLOR_RANGE) {
        calf_action_kind_t kind =
            ui->screen == CALF_SCREEN_ENCODING_COLOR_RANGE
                ? CALF_ACTION_SET_ENCODING_COLOR_RANGE
                : CALF_ACTION_SET_RECORDING_COLOR_RANGE;
        for(i = 0; i < (int)ARRAY_SIZE(k_color_ranges); ++i)
            if(contains(grid_cell(i, 2, 126, 218), x, y))
                return guarded_setting_action(
                    ui, kind, k_color_ranges[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_RECORDING_CODEC) {
        for(i = 0; i < (int)ARRAY_SIZE(k_recording_codecs); ++i)
            if(contains(grid_cell(i, 2, 108, 142), x, y))
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_RECORDING_CODEC,
                    k_recording_codecs[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_RECORDING_BITRATE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_recording_bitrates); ++i)
            if(contains(grid_cell(i, 2, 80, 70), x, y))
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_RECORDING_BITRATE,
                    k_recording_bitrates[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_RECORDING_GOP) {
        for(i = 0; i < (int)ARRAY_SIZE(k_recording_gops); ++i)
            if(contains(grid_cell(i, 3, 108, 142), x, y))
                return guarded_setting_action(
                    ui, CALF_ACTION_SET_RECORDING_GOP,
                    k_recording_gops[i].value, i);
    }
    else if(ui->screen == CALF_SCREEN_CAPTURE_MODE) {
        for(i = 0; i < (int)ARRAY_SIZE(k_capture_modes); ++i) {
            if(contains(grid_cell(i, 3, 126, 218), x, y)) {
                if(i == (int)ui->capture_mode) {
                    change_screen(ui, CALF_SCREEN_MAIN);
                    return no_action();
                }
                if(!ui->status.online || ui->status.recording ||
                   ui->status.streaming != 0 || ui->status.playback != 0) {
                    calf_ui_notice(ui,
                        ui->status.recording ? "STOP RECORDING FIRST"
                        : !ui->status.online ? "STATUS UNKNOWN"
                        : ui->status.streaming > 0 ? "STOP LIVE FIRST"
                        : ui->status.playback > 0 ? "STOP PLAYBACK FIRST"
                        : "STATUS UNKNOWN", 1);
                    return no_action();
                }
                return begin_action(ui, CALF_ACTION_SET_CAPTURE_MODE,
                                    k_capture_modes[i].value, i);
            }
        }
    }
    else if(ui->screen == CALF_SCREEN_ADJUST_DATETIME) {
        for(i = 0; i < 6; ++i) {
            rect_t decrement = {170, 78 + i * 52, 116, 46};
            rect_t increment = {514, 78 + i * 52, 116, 46};
            if(contains(decrement, x, y)) {
                datetime_adjust(ui, i, -1);
                ui->focus_index = i;
                return no_action();
            }
            if(contains(increment, x, y)) {
                datetime_adjust(ui, i, 1);
                ui->focus_index = i;
                return no_action();
            }
        }
        if(contains((rect_t){280, 398, 240, 66}, x, y))
            return datetime_apply(ui);
    }
    return no_action();
}
