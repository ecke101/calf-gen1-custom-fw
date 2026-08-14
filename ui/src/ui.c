#include "ui_internal.h"

_Static_assert(CALF_ACTION_SET_DNR - CALF_ACTION_SET_BRIGHTNESS ==
                   CALF_IMAGE_LEVEL_COUNT - 1,
               "image actions must remain contiguous");
_Static_assert(CALF_SCREEN_IMAGE_DNR - CALF_SCREEN_IMAGE_BRIGHTNESS ==
                   CALF_IMAGE_LEVEL_COUNT - 1,
               "image screens must remain contiguous");
_Static_assert(CALF_ACTION_SET_USB_MIC_VOLUME -
                   CALF_ACTION_SET_BUILTIN_MIC_VOLUME ==
                   CALF_AUDIO_INPUT_VOLUME_COUNT - 1,
               "audio volume actions must remain contiguous");
_Static_assert(CALF_SCREEN_AUDIO_USB_VOLUME -
                   CALF_SCREEN_AUDIO_BUILTIN_VOLUME ==
                   CALF_AUDIO_INPUT_VOLUME_COUNT - 1,
               "audio volume screens must remain contiguous");

const choice_t k_exposures[] = {
    {"1/2", "0.5"},       {"1/4", "0.25"},
    {"1/8", "0.125"},     {"1/15", "0.0666667"},
    {"AUTO", "-1"},       {"1/30", "0.0333333"},
    {"1/60", "0.0166667"}, {"1/125", "0.008"},
    {"1/250", "0.004"},   {"1/1000", "0.001"},
    {"1/4000", "0.00025"}, {"1/50", "0.02"},
    {"1/100", "0.01"},    {"1/500", "0.002"},
    {"1/2000", "0.0005"}, {"1 SEC", "1"},
    {"2 SEC", "2"},      {"4 SEC", "4"},
    {"8 SEC", "8"},      {"12 SEC", "12"},
};

static const choice_t k_night_exposures[] = {
    {"1/15", "0.0666667"}, {"1/8", "0.125"},
    {"1/4", "0.25"},      {"1/2", "0.5"},
    {"1 SEC", "1"},       {"2 SEC", "2"},
    {"4 SEC", "4"},       {"8 SEC", "8"},
    {"12 SEC", "12"},
};

/* Photo keeps the original long-exposure grid. Video choices are ordered
 * from the slowest shutter the active sensor rate can actually apply. */
static const choice_t k_video_exposures_30fps[] = {
    {"AUTO", "-1"},       {"1/30", "0.0333333"},
    {"1/50", "0.02"},    {"1/60", "0.0166667"},
    {"1/100", "0.01"},   {"1/125", "0.008"},
    {"1/250", "0.004"},  {"1/500", "0.002"},
    {"1/1000", "0.001"}, {"1/2000", "0.0005"},
    {"1/4000", "0.00025"},
};

static const choice_t k_video_exposures_50fps[] = {
    {"AUTO", "-1"},       {"1/50", "0.02"},
    {"1/60", "0.0166667"}, {"1/100", "0.01"},
    {"1/125", "0.008"},  {"1/250", "0.004"},
    {"1/500", "0.002"},  {"1/1000", "0.001"},
    {"1/2000", "0.0005"}, {"1/4000", "0.00025"},
};

static const choice_t k_video_exposures_60fps[] = {
    {"AUTO", "-1"},       {"1/60", "0.0166667"},
    {"1/100", "0.01"},   {"1/125", "0.008"},
    {"1/250", "0.004"},  {"1/500", "0.002"},
    {"1/1000", "0.001"}, {"1/2000", "0.0005"},
    {"1/4000", "0.00025"},
};

const choice_t *calf_ui_exposure_choices(const calf_ui_t *ui, size_t *count)
{
    const choice_t *choices = k_exposures;
    size_t choice_count = 11;
    if(ui != (const calf_ui_t *)0 &&
       ui->capture_mode == CALF_CAPTURE_NIGHT) {
        choices = k_night_exposures;
        choice_count = ARRAY_SIZE(k_night_exposures);
    }
    else if(ui != (const calf_ui_t *)0 &&
            ui->capture_mode == CALF_CAPTURE_VIDEO) {
        if(ui->resolution_index <= 1) {
            choices = k_video_exposures_30fps;
            choice_count = ARRAY_SIZE(k_video_exposures_30fps);
        }
        else if(ui->resolution_index <= 5) {
            choices = k_video_exposures_50fps;
            choice_count = ARRAY_SIZE(k_video_exposures_50fps);
        }
        else {
            choices = k_video_exposures_60fps;
            choice_count = ARRAY_SIZE(k_video_exposures_60fps);
        }
    }
    if(count != (size_t *)0) *count = choice_count;
    return choices;
}

int calf_ui_exposure_index_for_value(const char *value)
{
    size_t index;
    for(index = 0; index < ARRAY_SIZE(k_exposures); ++index)
        if(text_equal(k_exposures[index].value, value)) return (int)index;
    return -1;
}

int calf_ui_exposure_visible_selection(const calf_ui_t *ui)
{
    const choice_t *choices;
    const char *value;
    size_t count;
    size_t index;
    if(ui == (const calf_ui_t *)0 || !ui->exposure_known ||
       ui->exposure_index < 0 ||
       ui->exposure_index >= (int)ARRAY_SIZE(k_exposures)) return -1;
    value = k_exposures[ui->exposure_index].value;
    choices = calf_ui_exposure_choices(ui, &count);
    for(index = 0; index < count; ++index)
        if(text_equal(choices[index].value, value)) return (int)index;
    return -1;
}

const choice_t k_isos[] = {
    {"AUTO", "auto"},     {"100", "iso100"},
    {"200", "iso200"},    {"400", "iso400"},
    {"800", "iso800"},    {"1600", "iso1600"},
    {"3200", "iso3200"},  {"6400", "iso6400"},
    {"12800", "iso12800"},
};

static const choice_t k_night_isos[] = {
    {"100", "iso100"}, {"200", "iso200"},
    {"400", "iso400"}, {"800", "iso800"},
};

const choice_t *calf_ui_iso_choices(const calf_ui_t *ui, size_t *count)
{
    const choice_t *choices = k_isos;
    size_t choice_count = ARRAY_SIZE(k_isos);
    if(ui != (const calf_ui_t *)0 &&
       ui->capture_mode == CALF_CAPTURE_NIGHT) {
        choices = k_night_isos;
        choice_count = ARRAY_SIZE(k_night_isos);
    }
    if(count != (size_t *)0) *count = choice_count;
    return choices;
}

int calf_iso_index_for_value(const char *value)
{
    size_t index;
    for(index = 0; index < ARRAY_SIZE(k_isos); ++index)
        if(text_equal(k_isos[index].value, value)) return (int)index;
    return -1;
}

int calf_ui_iso_visible_selection(const calf_ui_t *ui)
{
    const choice_t *choices;
    const char *value;
    size_t count;
    size_t index;
    if(ui == (const calf_ui_t *)0 || !ui->iso_known || ui->iso_index < 0 ||
       ui->iso_index >= (int)ARRAY_SIZE(k_isos)) return -1;
    value = k_isos[ui->iso_index].value;
    choices = calf_ui_iso_choices(ui, &count);
    for(index = 0; index < count; ++index)
        if(text_equal(choices[index].value, value)) return (int)index;
    return -1;
}

int calf_exposure_allowed(calf_capture_mode_t mode, const char *value)
{
    calf_ui_t ui;
    const choice_t *choices;
    size_t count;
    size_t index;
    calf_ui_init(&ui);
    ui.capture_mode = mode;
    choices = calf_ui_exposure_choices(&ui, &count);
    for(index = 0; index < count; ++index)
        if(text_equal(choices[index].value, value)) return 1;
    return 0;
}

int calf_iso_allowed(calf_capture_mode_t mode, const char *value)
{
    calf_ui_t ui;
    const choice_t *choices;
    size_t count;
    size_t index;
    calf_ui_init(&ui);
    ui.capture_mode = mode;
    choices = calf_ui_iso_choices(&ui, &count);
    for(index = 0; index < count; ++index)
        if(text_equal(choices[index].value, value)) return 1;
    return 0;
}

const choice_t k_lenses[] = {
    {"LEFT", "SENSOR0_4K"},
    {"STEREO", "PRIMARY"},
    {"RIGHT", "SENSOR1_4K"},
};

const choice_t k_capture_modes[] = {
    {"PHOTO", "photo"}, {"NIGHT", "night"}, {"VIDEO", "recording"},
};

const choice_t k_video_resolutions[] = {
    {"VR180 8K30", "VR180_8K"},
    {"8K30 MASK", "VR180_8K_MASK"},
    {"VR180 6K50", "VR180_6K"},
    {"6K50 MASK", "VR180_6K_MASK"},
    {"VR180 4K50", "VR180_4K"},
    {"4K50 MASK", "VR180_4K_MASK"},
    {"VR180 5K60", "VR180_5K60"},
    {"3D 4K60", "3D_4K"},
    {"3D 1080P60", "3D_1080P"},
};

const choice_t k_photo_resolutions[] = {
    {"VR180 8K", "VR180_PIC"}, {"3D 4K", "3D_4K"},
};

const choice_t k_encoding_codecs[] = {
    {"H.264", "H264"}, {"H.265", "H265"},
};

const choice_t k_image_qualities[] = {
    {"HIGHER 150M", "higher"},
    {"HIGH 100M", "high"},
    {"MEDIUM 60M", "medium"},
    {"LOW 20M", "low"},
};

const choice_t k_color_ranges[] = {
    {"FULL (PC)", "0"}, {"LIMITED (TV)", "1"},
};

const choice_t k_recording_codecs[] = {
    {"H.264 HIGH", "H264_HIGH"},
    {"H.264 MAIN", "H264_MAIN"},
    {"H.264 BASE", "H264_BASE"},
    {"H.265 MAIN", "H265_MAIN"},
};

const choice_t k_recording_bitrates[] = {
    {"10 MBPS", "10000"}, {"20 MBPS", "20000"},
    {"30 MBPS", "30000"}, {"40 MBPS", "40000"},
    {"50 MBPS", "50000"}, {"60 MBPS", "60000"},
    {"70 MBPS", "70000"}, {"80 MBPS", "80000"},
    {"90 MBPS", "90000"}, {"100 MBPS", "100000"},
};

const choice_t k_recording_gops[] = {
    {"10", "10"}, {"20", "20"}, {"30", "30"},
    {"40", "40"}, {"50", "50"}, {"60", "60"},
};

const choice_t k_white_balances[] = {
    {"AUTO", "auto"},           {"DAYLIGHT", "daylight"},
    {"CLOUDY", "cloudy"},       {"SHADOW", "shadow"},
    {"FLUORESCENT", "fluorescent"}, {"TUNGSTEN", "tungsten"},
};

const choice_t k_ev_values[] = {
    {"-3", "-3"}, {"-2", "-2"}, {"-1", "-1"}, {"0", "0"},
    {"+1", "1"},  {"+2", "2"},  {"+3", "3"},
};

const choice_t k_antiflicker_values[] = {
    {"OFF", "off"}, {"AUTO", "auto"}, {"50HZ", "50hz"}, {"60HZ", "60hz"},
};

/* The stock UI stores 0..25 and sends level * 10 + 1. */
const choice_t k_backlight_values[] = {
    {"0", "1"}, {"1", "11"}, {"2", "21"}, {"3", "31"},
    {"4", "41"}, {"5", "51"}, {"6", "61"}, {"7", "71"},
    {"8", "81"}, {"9", "91"}, {"10", "101"}, {"11", "111"},
    {"12", "121"}, {"13", "131"}, {"14", "141"}, {"15", "151"},
    {"16", "161"}, {"17", "171"}, {"18", "181"}, {"19", "191"},
    {"20", "201"}, {"21", "211"}, {"22", "221"}, {"23", "231"},
    {"24", "241"}, {"25", "251"},
};

const choice_t k_image_levels[] = {
    {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"},
    {"4", "4"}, {"5", "5"}, {"6", "6"}, {"7", "7"},
    {"8", "8"}, {"9", "9"}, {"10", "10"}, {"11", "11"},
    {"12", "12"}, {"13", "13"}, {"14", "14"}, {"15", "15"},
    {"16", "16"}, {"17", "17"}, {"18", "18"}, {"19", "19"},
    {"20", "20"},
};

const choice_t k_image_effects[] = {
    {"NONE", "none"}, {"BLACK WHITE", "blackwhite"},
};

const choice_t k_audio_inputs[] = {
    {"AUTO", "auto"}, {"BUILTIN MIC", "builtin_mic"},
    {"LINE IN", "35mm_linein"}, {"USB MIC", "usb_mic"},
};

/* The stock UI shows 0..10 and sends the selected level multiplied by 10. */
const choice_t k_audio_input_volumes[] = {
    {"0", "0"}, {"1", "10"}, {"2", "20"}, {"3", "30"},
    {"4", "40"}, {"5", "50"}, {"6", "60"}, {"7", "70"},
    {"8", "80"}, {"9", "90"}, {"10", "100"},
};

/* The stock speaker slider shows 0..14 and sends level * 10. */
const choice_t k_speaker_volumes[] = {
    {"0", "0"}, {"1", "10"}, {"2", "20"}, {"3", "30"},
    {"4", "40"}, {"5", "50"}, {"6", "60"}, {"7", "70"},
    {"8", "80"}, {"9", "90"}, {"10", "100"}, {"11", "110"},
    {"12", "120"}, {"13", "130"}, {"14", "140"},
};

/* Preserve the stock UI's POSIX TZ values; their sign is intentionally
 * opposite the human-readable UTC offset. */
const choice_t k_timezones[] = {
    {"UTC-12", "WST+12"}, {"UTC-11", "WST+11"},
    {"UTC-10", "WST+10"}, {"UTC-9", "WST+9"},
    {"UTC-8", "WST+8"},   {"UTC-7", "WST+7"},
    {"UTC-6", "WST+6"},   {"UTC-5", "WST+5"},
    {"UTC-4", "WST+4"},   {"UTC-3", "WST+3"},
    {"UTC-2", "WST+2"},   {"UTC-1", "WST+1"},
    {"UTC", "UTC"},       {"UTC+1", "EST-1"},
    {"UTC+2", "EST-2"},   {"UTC+3", "EST-3"},
    {"UTC+4", "EST-4"},   {"UTC+5", "EST-5"},
    {"UTC+6", "EST-6"},   {"UTC+7", "EST-7"},
    {"UTC+8", "EST-8"},   {"UTC+9", "EST-9"},
    {"UTC+10", "EST-10"}, {"UTC+11", "EST-11"},
    {"UTC+12", "EST-12"},
};

const choice_t k_auto_time_values[] = {
    {"OFF", "0"}, {"ON", "1"},
};

const choice_t k_photo_formats[] = {
    {"JPEG", "0"}, {"JPEG + RAW", "1"},
};

const choice_t k_drive_modes[] = {
    {"SINGLE", "single"},
    {"TIMER 2S", "timer-2"},
    {"TIMER 5S", "timer-5"},
    {"TIMER 10S", "timer-10"},
    {"BURST 3", "burst-3"},
    {"BURST 5", "burst-5"},
    {"BURST 10", "burst-10"},
    {"INTERVAL 1S", "interval-1"},
    {"INTERVAL 2S", "interval-2"},
    {"INTERVAL 5S", "interval-5"},
    {"INTERVAL 10S", "interval-10"},
    {"INTERVAL 30S", "interval-30"},
    {"INTERVAL 60S", "interval-60"},
};

static const int k_drive_mode_delays[] = {
    0, 2, 5, 10, 0, 0, 0, 1, 2, 5, 10, 30, 60,
};

const choice_t k_display_off_values[] = {
    {"ALWAYS ON", "-1"}, {"10 SECONDS", "10"},
    {"30 SECONDS", "30"}, {"1 MINUTE", "60"},
    {"5 MINUTES", "300"}, {"10 MINUTES", "600"},
    {"20 MINUTES", "1200"}, {"30 MINUTES", "1800"},
};

const choice_t k_languages[] = {
    {"ENGLISH", "en"},
};

const choice_t k_indicator_led_values[] = {
    {"NORMAL", "normal"},
    {"STEALTH", "stealth"},
};

static const int k_display_off_seconds[] = {
    -1, 10, 30, 60, 300, 600, 1200, 1800,
};

const nav_choice_t k_settings_categories[] = {
    {"CAMERA", CALF_SCREEN_SETTINGS_CAMERA},
    {"IMAGE", CALF_SCREEN_SETTINGS_IMAGE},
    /* Legacy internal names: ENCODING=channel 0, RECORDING=channel 1,
       LIVE=the not-yet-implemented channel 2 page. */
    {"VIDEO RECORDING", CALF_SCREEN_SETTINGS_ENCODING},
    {"LIVE STREAMING", CALF_SCREEN_SETTINGS_RECORDING},
    {"UVC", CALF_SCREEN_SETTINGS_LIVE},
    {"NETWORK", CALF_SCREEN_SETTINGS_NETWORK},
    {"AUDIO", CALF_SCREEN_SETTINGS_AUDIO},
    {"STORAGE", CALF_SCREEN_SETTINGS_STORAGE},
    {"DATE TIME", CALF_SCREEN_SETTINGS_DATETIME},
    {"GENERAL", CALF_SCREEN_SETTINGS_GENERAL},
};

const char *const k_camera_setting_labels[] = {
    "MODE", "RESOLUTION", "PHOTO FORMAT", "DRIVE MODE",
};
const char *const k_image_setting_labels[] = {
    "WHITE BAL", "EV", "BRIGHTNESS", "CONTRAST", "SATURATION",
    "SHARPNESS", "NOISE RED", "FLICKER", "EFFECT",
};
const char *const k_encoding_setting_labels[] = {
    "VIDEO CODEC", "IMAGE QUALITY", "COLOR RANGE",
};
const char *const k_recording_setting_labels[] = {
    "CODEC", "BITRATE", "GOP", "COLOR RANGE",
};
const char *const k_live_setting_labels[] = {
    "CODEC", "BITRATE", "GOP", "COLOR RANGE",
};
const char *const k_network_setting_labels[] = {
    "NETWORKS", "WI-FI POWER", "ETHERNET", "USB DIRECT",
};
const char *const k_audio_setting_labels[] = {
    "INPUT", "BUILTIN MIC", "LINE IN", "USB MIC", "SPEAKER",
};
const char *const k_storage_setting_labels[] = {
    "LOCATION", "USE AS USB", "FORMAT", "SPEED TEST",
};
const char *const k_datetime_setting_labels[] = {
    "TIME ZONE", "AUTO SET", "DATE TIME",
};
const char *const k_general_setting_labels[] = {
    "DISPLAY", "DISPLAY OFF", "LANGUAGE", "INDICATOR LED",
    "UPDATE", "RESET", "POWER STATS", "STOCK UI",
};

const char *const k_wifi_keyboard_rows[] = {
    "1234567890qwertyuiopasdfghjkl-zxcvbnm._@",
    "1234567890QWERTYUIOPASDFGHJKL-ZXCVBNM._@",
    "1234567890!#$%&*+=-()[]{}<>/\\:;,.\'\"_@^~",
};

void bytes_zero(void *destination, size_t length)
{
    unsigned char *bytes = (unsigned char *)destination;
    size_t i;
    for(i = 0; i < length; ++i) bytes[i] = 0;
}

void text_copy(char *destination, size_t capacity, const char *source)
{
    size_t i = 0;
    if(capacity == 0) return;
    if(source != (const char *)0) {
        while(i + 1 < capacity && source[i] != '\0') {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

size_t text_length(const char *text)
{
    size_t length = 0;
    if(text == (const char *)0) return 0;
    while(text[length] != '\0') ++length;
    return length;
}

int text_equal(const char *left, const char *right)
{
    size_t index = 0;
    if(left == (const char *)0 || right == (const char *)0) return 0;
    while(left[index] != '\0' && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static int choice_index_for_value(const choice_t *choices, size_t count,
                                  const char *value)
{
    size_t index;
    for(index = 0; index < count; ++index)
        if(text_equal(choices[index].value, value)) return (int)index;
    return -1;
}

void append_text(char *destination, size_t capacity, const char *text)
{
    size_t used = text_length(destination);
    size_t i = 0;
    if(used >= capacity) return;
    while(used + i + 1 < capacity && text[i] != '\0') {
        destination[used + i] = text[i];
        ++i;
    }
    destination[used + i] = '\0';
}

void append_uint(char *destination, size_t capacity, unsigned value)
{
    char reversed[16];
    size_t count = 0;
    if(value == 0) {
        append_text(destination, capacity, "0");
        return;
    }
    while(value != 0 && count < ARRAY_SIZE(reversed)) {
        reversed[count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while(count != 0) {
        char one[2];
        one[0] = reversed[--count];
        one[1] = '\0';
        append_text(destination, capacity, one);
    }
}

void append_int(char *destination, size_t capacity, int value)
{
    if(value < 0) {
        append_text(destination, capacity, "-");
        append_uint(destination, capacity, (unsigned)(-value));
    }
    else append_uint(destination, capacity, (unsigned)value);
}

size_t calf_exposure_count(void) { return ARRAY_SIZE(k_exposures); }

const char *calf_exposure_label(size_t index)
{
    return index < ARRAY_SIZE(k_exposures) ? k_exposures[index].label : "?";
}

const char *calf_exposure_value(size_t index)
{
    return index < ARRAY_SIZE(k_exposures) ? k_exposures[index].value : "-1";
}

size_t calf_iso_count(void) { return ARRAY_SIZE(k_isos); }

const char *calf_iso_label(size_t index)
{
    return index < ARRAY_SIZE(k_isos) ? k_isos[index].label : "?";
}

const char *calf_iso_value(size_t index)
{
    return index < ARRAY_SIZE(k_isos) ? k_isos[index].value : "auto";
}

size_t calf_display_off_count(void)
{
    return ARRAY_SIZE(k_display_off_values);
}

const char *calf_display_off_label(size_t index)
{
    return index < ARRAY_SIZE(k_display_off_values)
               ? k_display_off_values[index].label : "?";
}

int calf_display_off_seconds(size_t index)
{
    return index < ARRAY_SIZE(k_display_off_seconds)
               ? k_display_off_seconds[index] : -1;
}

int calf_display_off_index_from_seconds(int seconds)
{
    size_t index;
    for(index = 0; index < ARRAY_SIZE(k_display_off_seconds); ++index)
        if(k_display_off_seconds[index] == seconds) return (int)index;
    return -1;
}

size_t calf_language_count(void)
{
    return ARRAY_SIZE(k_languages);
}

const char *calf_language_label(size_t index)
{
    return index < ARRAY_SIZE(k_languages) ? k_languages[index].label : "?";
}

const char *calf_language_value(size_t index)
{
    return index < ARRAY_SIZE(k_languages) ? k_languages[index].value : "en";
}

int calf_language_index_from_value(const char *value)
{
    return choice_index_for_value(k_languages, ARRAY_SIZE(k_languages), value);
}

size_t calf_drive_mode_count(void)
{
    return ARRAY_SIZE(k_drive_modes);
}

const char *calf_drive_mode_label(size_t index)
{
    return index < ARRAY_SIZE(k_drive_modes)
               ? k_drive_modes[index].label : "?";
}

const char *calf_drive_mode_value(size_t index)
{
    return index < ARRAY_SIZE(k_drive_modes)
               ? k_drive_modes[index].value : "";
}

int calf_drive_mode_index_from_value(const char *value)
{
    return choice_index_for_value(k_drive_modes, ARRAY_SIZE(k_drive_modes),
                                  value);
}

int calf_drive_mode_delay_seconds(size_t index)
{
    return index < ARRAY_SIZE(k_drive_mode_delays)
               ? k_drive_mode_delays[index] : 0;
}

int calf_drive_mode_is_interval(size_t index)
{
    return index >= 7u && index < ARRAY_SIZE(k_drive_modes);
}

int calf_drive_mode_is_burst(size_t index)
{
    return index >= 4u && index <= 6u;
}

unsigned calf_drive_mode_shot_limit(size_t index)
{
    static const unsigned limits[] = {3u, 5u, 10u};
    return calf_drive_mode_is_burst(index) ? limits[index - 4u] : 0u;
}

static int power_milliwatts(int millivolts, int milliamps)
{
    int product = millivolts * milliamps;
    if(product < 0) return -((-product + 500) / 1000);
    return (product + 500) / 1000;
}

int calf_power_decode_bq25703(unsigned adc_vbus_psys,
                             unsigned adc_ibat,
                             unsigned adc_iin_cmpin,
                             unsigned adc_vsys_vbat,
                             unsigned charge_option_1,
                             unsigned adc_option,
                             int recording,
                             calf_power_sample_t *sample)
{
    const unsigned required_channels = 0x005du;
    unsigned vbus_code;
    unsigned input_code;
    unsigned charge_code;
    unsigned discharge_code;
    unsigned battery_code;
    int charge_ma;
    int discharge_ma;
    if(sample == (calf_power_sample_t *)0) return -1;
    bytes_zero(sample, sizeof(*sample));
    sample->recording = recording != 0;
    if((adc_option & required_channels) != required_channels) return -1;

    vbus_code = (adc_vbus_psys >> 8) & 0xffu;
    input_code = (adc_iin_cmpin >> 8) & 0xffu;
    charge_code = (adc_ibat >> 8) & 0x7fu;
    discharge_code = adc_ibat & 0x7fu;
    battery_code = adc_vsys_vbat & 0x7fu;
    if(battery_code == 0u) return -1;

    sample->usb_mv = vbus_code == 0u
                         ? 0 : 3200 + (int)vbus_code * 64;
    sample->usb_ma = (int)input_code * 50;
    if((charge_option_1 & (1u << 11)) != 0u)
        sample->usb_ma /= 2;
    if(sample->usb_mv == 0) sample->usb_ma = 0;

    charge_ma = (int)charge_code * 64;
    discharge_ma = (int)discharge_code * 256;
    if((charge_option_1 & (1u << 10)) != 0u) {
        charge_ma /= 2;
        discharge_ma /= 2;
    }
    sample->battery_mv = 2880 + (int)battery_code * 64;
    sample->battery_ma = discharge_ma - charge_ma;
    sample->usb_mw = power_milliwatts(sample->usb_mv, sample->usb_ma);
    sample->battery_mw = power_milliwatts(sample->battery_mv,
                                          sample->battery_ma);
    sample->device_mw = sample->usb_mw + sample->battery_mw;
    sample->valid = 1;
    return 0;
}

static int rounded_average(int64_t total, int count)
{
    if(total < 0)
        return -(int)((-total + count / 2) / count);
    return (int)((total + count / 2) / count);
}

int calf_power_average_samples(const calf_power_sample_t *samples,
                               size_t count,
                               calf_power_sample_t *average)
{
    int64_t usb_mv = 0;
    int64_t usb_ma = 0;
    int64_t usb_mw = 0;
    int64_t battery_mv = 0;
    int64_t battery_ma = 0;
    int64_t battery_mw = 0;
    int64_t device_mw = 0;
    int valid_count = 0;
    int recording = 0;
    size_t index;
    if(samples == (const calf_power_sample_t *)0 ||
       average == (calf_power_sample_t *)0 || count == 0u)
        return -1;
    bytes_zero(average, sizeof(*average));
    for(index = 0; index < count; ++index) {
        if(samples[index].recording) recording = 1;
        if(!samples[index].valid) continue;
        usb_mv += samples[index].usb_mv;
        usb_ma += samples[index].usb_ma;
        usb_mw += samples[index].usb_mw;
        battery_mv += samples[index].battery_mv;
        battery_ma += samples[index].battery_ma;
        battery_mw += samples[index].battery_mw;
        device_mw += samples[index].device_mw;
        ++valid_count;
    }
    average->recording = recording;
    if(valid_count == 0) return -1;
    average->usb_mv = rounded_average(usb_mv, valid_count);
    average->usb_ma = rounded_average(usb_ma, valid_count);
    average->usb_mw = rounded_average(usb_mw, valid_count);
    average->battery_mv = rounded_average(battery_mv, valid_count);
    average->battery_ma = rounded_average(battery_ma, valid_count);
    average->battery_mw = rounded_average(battery_mw, valid_count);
    average->device_mw = rounded_average(device_mw, valid_count);
    average->valid = 1;
    return 0;
}

void calf_ui_add_power_sample(calf_ui_t *ui,
                              const calf_power_sample_t *sample)
{
    int index;
    if(sample == (const calf_power_sample_t *)0) return;
    ui->power = *sample;
    index = ui->power_history_next;
    ui->power_history[index] = *sample;
    ui->power_history_next = (index + 1) % CALF_POWER_HISTORY_COUNT;
    if(ui->power_history_count < CALF_POWER_HISTORY_COUNT)
        ++ui->power_history_count;
    if(ui->screen == CALF_SCREEN_POWER_HISTORY) ++ui->revision;
}

void calf_ui_init(calf_ui_t *ui)
{
    int i;
    bytes_zero(ui, sizeof(*ui));
    ui->screen = CALF_SCREEN_MAIN;
    ui->exposure_index = 4;
    ui->iso_index = 0;
    ui->lens_index = 1;
    ui->white_balance_index = 0;
    ui->ev_index = 3;
    ui->antiflicker_index = 1;
    ui->backlight_index = 15;
    for(i = 0; i < CALF_IMAGE_LEVEL_COUNT; ++i) ui->image_level_index[i] = 10;
    ui->effect_index = 0;
    ui->display_off_index = 0;
    ui->display_off_seconds = -1;
    ui->language_index = CALF_LANGUAGE_ENGLISH;
    ui->indicator_led_index = 0;
    ui->audio_input_index = 0;
    for(i = 0; i < CALF_AUDIO_INPUT_VOLUME_COUNT; ++i)
        ui->audio_input_volume_index[i] = 8;
    ui->speaker_volume_index = 5;
    ui->timezone_index = 12;
    ui->auto_time_index = 0;
    ui->datetime_year = 2026;
    ui->datetime_month = 1;
    ui->datetime_day = 1;
    ui->capture_mode = CALF_CAPTURE_PHOTO;
    ui->resolution_index = 0;
    ui->photo_format_index = 0;
    ui->drive_mode_index = 0;
    ui->encoding_codec_index = 0;
    ui->image_quality_index = 0;
    ui->encoding_color_range_index = 0;
    ui->recording_codec_index = 0;
    ui->recording_bitrate_index = 2;
    ui->recording_gop_index = 1;
    ui->recording_color_range_index = 0;
    ui->display_off_known = 1;
    ui->language_known = 1;
    ui->indicator_led_known = 1;
    ui->drive_mode_known = 1;
    ui->focus_index = 0;
    ui->focus_visible = 0;
    ui->return_to_main = 0;
    ui->lcd_on = 1;
    ui->pending_selection = -1;
    ui->wifi_selected_index = -1;
    ui->wifi_enabled = 1;
    ui->wifi_enabled_known = 0;
    ui->status.battery_percent = -1;
    ui->status.storage_free_mb = -1;
    ui->status.system_temp = -1;
    ui->status.core_temp = -1;
    ui->status.streaming = -1;
    ui->status.playback = -1;
    ui->status.usb_power = -1;
    text_copy(ui->message, sizeof(ui->message), "BOOTING");
    ui->revision = 1;
}

void calf_ui_complete_action(calf_ui_t *ui, calf_action_t action, int success,
                             const char *message)
{
    calf_screen_t old_screen = ui->screen;
    if(action.kind == CALF_ACTION_NONE) return;
    if(success) {
        if(action.kind == CALF_ACTION_SET_EXPOSURE) {
            ui->exposure_index = action.selection;
            ui->exposure_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_ISO) {
            ui->iso_index = action.selection;
            ui->iso_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_CAMERA_MODE) {
            ui->lens_index = action.selection;
            ui->lens_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_WHITE_BALANCE) {
            ui->white_balance_index = action.selection;
            ui->white_balance_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_EV) {
            ui->ev_index = action.selection;
            ui->ev_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_ANTIFLICKER) {
            ui->antiflicker_index = action.selection;
            ui->antiflicker_known = 1;
        }
        else if(action.kind >= CALF_ACTION_SET_BRIGHTNESS &&
                action.kind <= CALF_ACTION_SET_DNR) {
            int level = (int)action.kind - (int)CALF_ACTION_SET_BRIGHTNESS;
            ui->image_level_index[level] = action.selection;
            ui->image_level_known[level] = 1;
        }
        else if(action.kind == CALF_ACTION_SET_EFFECT) {
            ui->effect_index = action.selection;
            ui->effect_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_BACKLIGHT) {
            ui->backlight_index = action.selection;
            ui->backlight_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_LCD_POWER)
            ui->lcd_on = action.selection != 0;
        else if(action.kind == CALF_ACTION_SET_DISPLAY_OFF) {
            ui->display_off_index = action.selection;
            ui->display_off_seconds = calf_display_off_seconds(
                (size_t)action.selection);
            ui->display_off_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_LANGUAGE) {
            ui->language_index = action.selection;
            ui->language_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_INDICATOR_LED) {
            ui->indicator_led_index = action.selection;
            ui->indicator_led_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_AUDIO_INPUT) {
            ui->audio_input_index = action.selection;
            ui->audio_input_known = 1;
        }
        else if(action.kind >= CALF_ACTION_SET_BUILTIN_MIC_VOLUME &&
                action.kind <= CALF_ACTION_SET_USB_MIC_VOLUME) {
            int input = (int)action.kind -
                        (int)CALF_ACTION_SET_BUILTIN_MIC_VOLUME;
            ui->audio_input_volume_index[input] = action.selection;
            ui->audio_input_volume_known[input] = 1;
        }
        else if(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME) {
            ui->speaker_volume_index = action.selection;
            ui->speaker_volume_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_TIMEZONE) {
            ui->timezone_index = action.selection;
            ui->timezone_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_AUTO_TIME) {
            ui->auto_time_index = action.selection;
            ui->auto_time_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_DATETIME)
            ui->datetime_known = 1;
        else if(action.kind == CALF_ACTION_SET_WIFI_ENABLED) {
            ui->wifi_enabled = action.selection != 0;
            ui->wifi_enabled_known = 1;
            if(!ui->wifi_enabled) {
                ui->wifi_current_ssid[0] = '\0';
                ui->wifi_ip_address[0] = '\0';
                ui->wifi_network_count = 0;
            }
        }
        else if(action.kind == CALF_ACTION_SET_CAPTURE_MODE) {
            ui->capture_mode = (calf_capture_mode_t)action.selection;
            ui->resolution_known = 0;
        }
        else if(action.kind == CALF_ACTION_SET_RESOLUTION) {
            ui->resolution_index = action.selection;
            ui->resolution_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_PHOTO_FORMAT) {
            ui->photo_format_index = action.selection;
            ui->photo_format_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_DRIVE_MODE) {
            ui->drive_mode_index = action.selection;
            ui->drive_mode_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_ENCODING_CODEC) {
            ui->encoding_codec_index = action.selection;
            ui->encoding_codec_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_IMAGE_QUALITY) {
            ui->image_quality_index = action.selection;
            ui->image_quality_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_ENCODING_COLOR_RANGE) {
            ui->encoding_color_range_index = action.selection;
            ui->encoding_color_range_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_RECORDING_CODEC) {
            ui->recording_codec_index = action.selection;
            ui->recording_codec_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_RECORDING_BITRATE) {
            ui->recording_bitrate_index = action.selection;
            ui->recording_bitrate_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_RECORDING_GOP) {
            ui->recording_gop_index = action.selection;
            ui->recording_gop_known = 1;
        }
        else if(action.kind == CALF_ACTION_SET_RECORDING_COLOR_RANGE) {
            ui->recording_color_range_index = action.selection;
            ui->recording_color_range_known = 1;
        }
        else if(action.kind == CALF_ACTION_RECORD_TOGGLE)
            ui->status.recording = !ui->status.recording;
        else if(action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE)
            ui->gallery_playing = !ui->gallery_playing;
        text_copy(ui->message, sizeof(ui->message), message != (const char *)0 ? message : "APPLIED");
        ui->message_is_error = 0;
        if(action.kind == CALF_ACTION_SET_EXPOSURE ||
           action.kind == CALF_ACTION_SET_ISO ||
           action.kind == CALF_ACTION_SET_CAMERA_MODE ||
           action.kind == CALF_ACTION_SET_CAPTURE_MODE ||
           action.kind == CALF_ACTION_CAPTURE_SEQUENCE_START ||
           action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL ||
           action.kind == CALF_ACTION_GALLERY_EXIT)
            ui->screen = CALF_SCREEN_MAIN;
        else if(action.kind == CALF_ACTION_GALLERY_ENTER ||
                action.kind == CALF_ACTION_GALLERY_PREV ||
                action.kind == CALF_ACTION_GALLERY_NEXT ||
                action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE ||
                action.kind == CALF_ACTION_GALLERY_DELETE)
            ui->screen = CALF_SCREEN_GALLERY;
        else if(action.kind == CALF_ACTION_SET_WHITE_BALANCE ||
                action.kind == CALF_ACTION_SET_EV ||
                action.kind == CALF_ACTION_SET_ANTIFLICKER ||
                (action.kind >= CALF_ACTION_SET_BRIGHTNESS &&
                 action.kind <= CALF_ACTION_SET_EFFECT))
            ui->screen = ui->return_to_main ? CALF_SCREEN_MAIN
                                             : CALF_SCREEN_SETTINGS_IMAGE;
        else if(action.kind == CALF_ACTION_SET_BACKLIGHT ||
                action.kind == CALF_ACTION_SET_DISPLAY_OFF ||
                action.kind == CALF_ACTION_SET_LANGUAGE ||
                action.kind == CALF_ACTION_SET_INDICATOR_LED ||
                action.kind == CALF_ACTION_LOAD_STOCK_UI)
            ui->screen = CALF_SCREEN_SETTINGS_GENERAL;
        else if(action.kind == CALF_ACTION_SET_AUDIO_INPUT ||
                (action.kind >= CALF_ACTION_SET_BUILTIN_MIC_VOLUME &&
                 action.kind <= CALF_ACTION_SET_USB_MIC_VOLUME))
            ui->screen = CALF_SCREEN_SETTINGS_AUDIO;
        else if(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME)
            ui->screen = old_screen == CALF_SCREEN_GALLERY
                             ? CALF_SCREEN_GALLERY
                             : CALF_SCREEN_SETTINGS_AUDIO;
        else if(action.kind == CALF_ACTION_SET_TIMEZONE ||
                action.kind == CALF_ACTION_SET_AUTO_TIME ||
                action.kind == CALF_ACTION_SET_DATETIME)
            ui->screen = CALF_SCREEN_SETTINGS_DATETIME;
        else if(action.kind == CALF_ACTION_SET_RESOLUTION ||
                action.kind == CALF_ACTION_SET_PHOTO_FORMAT)
            ui->screen = CALF_SCREEN_SETTINGS_CAMERA;
        else if(action.kind == CALF_ACTION_SET_DRIVE_MODE)
            ui->screen = ui->return_to_main ? CALF_SCREEN_MAIN
                                             : CALF_SCREEN_SETTINGS_CAMERA;
        else if(action.kind >= CALF_ACTION_SET_ENCODING_CODEC &&
                action.kind <= CALF_ACTION_SET_ENCODING_COLOR_RANGE)
            ui->screen = CALF_SCREEN_SETTINGS_ENCODING;
        else if(action.kind >= CALF_ACTION_SET_RECORDING_CODEC &&
                action.kind <= CALF_ACTION_SET_RECORDING_COLOR_RANGE)
            ui->screen = CALF_SCREEN_SETTINGS_RECORDING;
        else if(action.kind == CALF_ACTION_WIFI_SCAN ||
                action.kind == CALF_ACTION_WIFI_CONNECT_SAVED ||
                action.kind == CALF_ACTION_WIFI_CONNECT_PASSWORD)
            ui->screen = CALF_SCREEN_WIFI_LIST;
        else if(action.kind == CALF_ACTION_SET_WIFI_ENABLED)
            ui->screen = CALF_SCREEN_SETTINGS_NETWORK;
        else if(action.kind == CALF_ACTION_FIRMWARE_CHECK)
            ui->screen = CALF_SCREEN_UPDATE_CONFIRM;
        if(ui->screen == CALF_SCREEN_MAIN) ui->return_to_main = 0;
        if(ui->screen != old_screen) calf_ui_focus_default(ui);
    }
    else {
        if(action.kind == CALF_ACTION_FIRMWARE_CHECK)
            ui->update_ready = 0;
        text_copy(ui->message, sizeof(ui->message), message != (const char *)0 ? message : "REQUEST FAILED");
        ui->message_is_error = 1;
    }
    ui->pending_action = CALF_ACTION_NONE;
    ui->pending_selection = -1;
    ++ui->revision;
}

void calf_ui_set_status(calf_ui_t *ui, const calf_backend_status_t *status)
{
    if(ui->status.online != status->online ||
       ui->status.recording != status->recording ||
       ui->status.recording_seconds != status->recording_seconds ||
       ui->status.battery_percent != status->battery_percent ||
       ui->status.storage_free_mb != status->storage_free_mb ||
       ui->status.system_temp != status->system_temp ||
       ui->status.core_temp != status->core_temp ||
       ui->status.streaming != status->streaming ||
       ui->status.playback != status->playback ||
       ui->status.usb_power != status->usb_power) {
        ui->status = *status;
        ++ui->revision;
    }
}

void calf_ui_set_display_off(calf_ui_t *ui, int selection)
{
    if(selection < 0 || selection >= (int)ARRAY_SIZE(k_display_off_values))
        return;
    if(!ui->display_off_known || ui->display_off_index != selection) {
        ui->display_off_index = selection;
        ui->display_off_seconds = calf_display_off_seconds((size_t)selection);
        ui->display_off_known = 1;
        ++ui->revision;
    }
}

void calf_ui_set_language(calf_ui_t *ui, int selection)
{
    if(selection < 0 || selection >= (int)ARRAY_SIZE(k_languages)) return;
    if(!ui->language_known || ui->language_index != selection) {
        ui->language_index = selection;
        ui->language_known = 1;
        ++ui->revision;
    }
}

void calf_ui_set_indicator_led(calf_ui_t *ui, int selection)
{
    if(selection < 0 ||
       selection >= (int)ARRAY_SIZE(k_indicator_led_values)) return;
    if(!ui->indicator_led_known || ui->indicator_led_index != selection) {
        ui->indicator_led_index = selection;
        ui->indicator_led_known = 1;
        ++ui->revision;
    }
}

void calf_ui_set_capture_mode(calf_ui_t *ui, calf_capture_mode_t mode)
{
    if(mode != CALF_CAPTURE_PHOTO && mode != CALF_CAPTURE_NIGHT &&
       mode != CALF_CAPTURE_VIDEO) return;
    if(ui->capture_mode != mode) {
        ui->capture_mode = mode;
        ++ui->revision;
    }
}

void calf_ui_set_night_preview(calf_ui_t *ui, int preview_iso, int clipped)
{
    if(preview_iso < 0) preview_iso = 0;
    clipped = clipped != 0;
    if(ui->night_preview_iso != preview_iso ||
       ui->night_preview_clipped != clipped) {
        ui->night_preview_iso = preview_iso;
        ui->night_preview_clipped = clipped;
        ++ui->revision;
    }
}

int calf_ui_sync_resolution(calf_ui_t *ui, calf_capture_mode_t mode,
                            const char *profile)
{
    const choice_t *choices = mode == CALF_CAPTURE_VIDEO
                                  ? k_video_resolutions : k_photo_resolutions;
    size_t count = mode == CALF_CAPTURE_VIDEO
                       ? ARRAY_SIZE(k_video_resolutions)
                       : ARRAY_SIZE(k_photo_resolutions);
    int index = choice_index_for_value(choices, count, profile);
    if(mode != ui->capture_mode) return -1;
    if(index < 0) {
        if(ui->resolution_known) {
            ui->resolution_known = 0;
            ++ui->revision;
        }
        return -1;
    }
    if(!ui->resolution_known || ui->resolution_index != index) {
        ui->resolution_index = index;
        ui->resolution_known = 1;
        ++ui->revision;
    }
    return 0;
}

void calf_ui_sync_photo_format(calf_ui_t *ui, int raw_enabled)
{
    int selection = raw_enabled != 0;
    if(!ui->photo_format_known || ui->photo_format_index != selection) {
        ui->photo_format_index = selection;
        ui->photo_format_known = 1;
        ++ui->revision;
    }
}

void calf_ui_set_drive_mode(calf_ui_t *ui, int selection)
{
    if(selection < 0 || selection >= (int)ARRAY_SIZE(k_drive_modes))
        return;
    if(!ui->drive_mode_known || ui->drive_mode_index != selection) {
        ui->drive_mode_index = selection;
        ui->drive_mode_known = 1;
        ++ui->revision;
    }
}

void calf_ui_set_capture_sequence(calf_ui_t *ui, int active,
                                  int interval, int sleeping,
                                  int remaining_seconds,
                                  unsigned shot_count)
{
    active = active != 0;
    interval = interval != 0;
    sleeping = sleeping != 0;
    if(remaining_seconds < 0) remaining_seconds = 0;
    if(ui->capture_sequence_active == active &&
       ui->capture_sequence_interval == interval &&
       ui->capture_sequence_sleeping == sleeping &&
       ui->capture_sequence_remaining_seconds == remaining_seconds &&
       ui->capture_sequence_shot_count == shot_count)
        return;
    ui->capture_sequence_active = active;
    ui->capture_sequence_interval = interval;
    ui->capture_sequence_sleeping = sleeping;
    ui->capture_sequence_remaining_seconds = remaining_seconds;
    ui->capture_sequence_shot_count = shot_count;
    ++ui->revision;
}

int calf_ui_sync_encoder_value(calf_ui_t *ui, calf_action_kind_t kind,
                               const char *value)
{
    const choice_t *choices = (const choice_t *)0;
    size_t count = 0;
    int *selection = (int *)0;
    int *known = (int *)0;
    int index;
    if(kind == CALF_ACTION_SET_ENCODING_CODEC) {
        choices = k_encoding_codecs; count = ARRAY_SIZE(k_encoding_codecs);
        selection = &ui->encoding_codec_index;
        known = &ui->encoding_codec_known;
    }
    else if(kind == CALF_ACTION_SET_IMAGE_QUALITY) {
        choices = k_image_qualities; count = ARRAY_SIZE(k_image_qualities);
        selection = &ui->image_quality_index;
        known = &ui->image_quality_known;
    }
    else if(kind == CALF_ACTION_SET_ENCODING_COLOR_RANGE ||
            kind == CALF_ACTION_SET_RECORDING_COLOR_RANGE) {
        choices = k_color_ranges; count = ARRAY_SIZE(k_color_ranges);
        if(kind == CALF_ACTION_SET_ENCODING_COLOR_RANGE) {
            selection = &ui->encoding_color_range_index;
            known = &ui->encoding_color_range_known;
        }
        else {
            selection = &ui->recording_color_range_index;
            known = &ui->recording_color_range_known;
        }
    }
    else if(kind == CALF_ACTION_SET_RECORDING_CODEC) {
        choices = k_recording_codecs; count = ARRAY_SIZE(k_recording_codecs);
        selection = &ui->recording_codec_index;
        known = &ui->recording_codec_known;
    }
    else if(kind == CALF_ACTION_SET_RECORDING_BITRATE) {
        choices = k_recording_bitrates;
        count = ARRAY_SIZE(k_recording_bitrates);
        selection = &ui->recording_bitrate_index;
        known = &ui->recording_bitrate_known;
    }
    else if(kind == CALF_ACTION_SET_RECORDING_GOP) {
        choices = k_recording_gops; count = ARRAY_SIZE(k_recording_gops);
        selection = &ui->recording_gop_index;
        known = &ui->recording_gop_known;
    }
    else return -1;
    index = choice_index_for_value(choices, count, value);
    if(index < 0) {
        if(*known) {
            *known = 0;
            ++ui->revision;
        }
        return -1;
    }
    if(!*known || *selection != index) {
        *selection = index;
        *known = 1;
        ++ui->revision;
    }
    return 0;
}

void calf_ui_set_gallery(calf_ui_t *ui, const char *filename, int is_video,
                         int index, int count, int playing)
{
    char previous[sizeof(ui->gallery_filename)];
    int item_changed;
    text_copy(previous, sizeof(previous), ui->gallery_filename);
    text_copy(ui->gallery_filename, sizeof(ui->gallery_filename),
              filename != (const char *)0 ? filename : "");
    item_changed = !text_equal(previous, ui->gallery_filename);
    if(item_changed ||
       ui->gallery_has_item != (count > 0) ||
       ui->gallery_is_video != (is_video != 0) ||
       ui->gallery_index != index || ui->gallery_count != count ||
       ui->gallery_playing != (playing != 0)) {
        ui->gallery_has_item = count > 0;
        ui->gallery_is_video = is_video != 0;
        ui->gallery_index = index;
        ui->gallery_count = count;
        ui->gallery_playing = playing != 0;
        if(item_changed) {
            ui->gallery_position_seconds = 0;
            ui->gallery_duration_seconds = 0;
            ui->gallery_timing_known = 0;
            ui->gallery_histogram_valid = 0;
        }
        ++ui->revision;
    }
}

void calf_ui_set_gallery_index(calf_ui_t *ui, int index)
{
    if(!ui->gallery_has_item || index < 0 || index >= ui->gallery_count)
        return;
    if(ui->gallery_index != index) {
        ui->gallery_index = index;
        ++ui->revision;
    }
}

void calf_ui_set_gallery_playback(calf_ui_t *ui, int playing,
                                  int position_seconds,
                                  int duration_seconds, int timing_known)
{
    if(position_seconds < 0) position_seconds = 0;
    if(duration_seconds < 0) duration_seconds = 0;
    if(position_seconds > duration_seconds && timing_known)
        position_seconds = duration_seconds;
    if(ui->gallery_playing != (playing != 0) ||
       ui->gallery_position_seconds != position_seconds ||
       ui->gallery_duration_seconds != duration_seconds ||
       ui->gallery_timing_known != (timing_known != 0)) {
        ui->gallery_playing = playing != 0;
        ui->gallery_position_seconds = position_seconds;
        ui->gallery_duration_seconds = duration_seconds;
        ui->gallery_timing_known = timing_known != 0;
        ++ui->revision;
    }
}

void calf_ui_set_gallery_volume_visible(calf_ui_t *ui, int visible)
{
    if(ui->gallery_volume_visible != (visible != 0)) {
        ui->gallery_volume_visible = visible != 0;
        ++ui->revision;
    }
}

void calf_ui_set_gallery_histogram(calf_ui_t *ui,
                                   const uint32_t *bins, int valid)
{
    int changed = ui->gallery_histogram_valid != (valid != 0);
    int index;
    if(valid && bins != (const uint32_t *)0) {
        for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index) {
            if(ui->gallery_histogram[index] != bins[index]) changed = 1;
            ui->gallery_histogram[index] = bins[index];
        }
    }
    if(changed) {
        ui->gallery_histogram_valid = valid != 0;
        ++ui->revision;
    }
}

void calf_ui_set_live_histogram(calf_ui_t *ui,
                                const uint32_t *bins, int valid)
{
    int is_valid = valid > 0;
    int is_error = valid < 0;
    int changed = ui->live_histogram_valid != is_valid ||
                  ui->live_histogram_error != is_error;
    int index;
    if(is_valid && bins != (const uint32_t *)0) {
        for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index) {
            if(ui->live_histogram[index] != bins[index]) changed = 1;
            ui->live_histogram[index] = bins[index];
        }
    }
    if(changed) {
        ui->live_histogram_valid = is_valid;
        ui->live_histogram_error = is_error;
        ++ui->revision;
    }
}

void calf_ui_set_motion(calf_ui_t *ui, int gyro_x, int gyro_y, int gyro_z,
                        int valid)
{
    const int calibration_sample_count = 5;
    const int dead_zone = 120;
    const int full_scale = 1600;
    int absolute_x;
    int absolute_y;
    int absolute_z;
    int magnitude;
    int instant_score;
    int score;
    int stable_samples;
    int changed;
    if(!valid) {
        if(ui->motion_valid) {
            ui->motion_valid = 0;
            ui->motion_score = 100;
            ui->motion_stable_samples = 0;
            ++ui->revision;
        }
        return;
    }
    if(gyro_x < -1000) gyro_x = -1000;
    if(gyro_x > 1000) gyro_x = 1000;
    if(gyro_y < -1000) gyro_y = -1000;
    if(gyro_y > 1000) gyro_y = 1000;
    if(gyro_z < -1000) gyro_z = -1000;
    if(gyro_z > 1000) gyro_z = 1000;
    if(ui->motion_calibration_samples < calibration_sample_count) {
        int samples = ui->motion_calibration_samples;
        int difference_x = gyro_x - ui->motion_bias_x;
        int difference_y = gyro_y - ui->motion_bias_y;
        int difference_z = gyro_z - ui->motion_bias_z;
        if(difference_x < 0) difference_x = -difference_x;
        if(difference_y < 0) difference_y = -difference_y;
        if(difference_z < 0) difference_z = -difference_z;
        if(samples == 0 || difference_x > 120 || difference_y > 120 ||
           difference_z > 120) {
            samples = 1;
            ui->motion_bias_x = gyro_x;
            ui->motion_bias_y = gyro_y;
            ui->motion_bias_z = gyro_z;
        }
        else {
            ui->motion_bias_x =
                (ui->motion_bias_x * samples + gyro_x) / (samples + 1);
            ui->motion_bias_y =
                (ui->motion_bias_y * samples + gyro_y) / (samples + 1);
            ui->motion_bias_z =
                (ui->motion_bias_z * samples + gyro_z) / (samples + 1);
            ++samples;
        }
        ui->motion_calibration_samples = samples;
        ui->motion_valid = 1;
        ui->motion_x = 0;
        ui->motion_y = 0;
        ui->motion_z = 0;
        ui->motion_score = 0;
        ui->motion_stable_samples = 0;
        ++ui->revision;
        return;
    }
    gyro_x -= ui->motion_bias_x;
    gyro_y -= ui->motion_bias_y;
    gyro_z -= ui->motion_bias_z;
    absolute_x = gyro_x < 0 ? -gyro_x : gyro_x;
    absolute_y = gyro_y < 0 ? -gyro_y : gyro_y;
    absolute_z = gyro_z < 0 ? -gyro_z : gyro_z;
    magnitude = absolute_x > absolute_y ? absolute_x : absolute_y;
    if(absolute_z > magnitude) magnitude = absolute_z;
    instant_score = magnitude <= dead_zone
                        ? 0
                        : (magnitude - dead_zone) * 100 /
                              (full_scale - dead_zone);
    if(instant_score > 100) instant_score = 100;
    /* The backend supplies the newest sample from its 208 Hz stream, while
     * the UI renders at about 15 Hz. Give old and new readings equal weight
     * so high-frequency hand motion does not make the ring snap between
     * radii, without changing the CAMM data or level bubble. */
    score = ui->motion_valid
                ? (ui->motion_score + instant_score + 1) / 2
                : instant_score;
    if(score < 3) score = 0;
    stable_samples = instant_score == 0
                         ? ui->motion_stable_samples + 1 : 0;
    if(stable_samples > 3) stable_samples = 3;
    changed = !ui->motion_valid || ui->motion_x != gyro_x ||
              ui->motion_y != gyro_y || ui->motion_z != gyro_z ||
              ui->motion_score != score ||
              ui->motion_stable_samples != stable_samples;
    ui->motion_valid = 1;
    ui->motion_x = gyro_x;
    ui->motion_y = gyro_y;
    ui->motion_z = gyro_z;
    ui->motion_score = score;
    ui->motion_stable_samples = stable_samples;
    if(changed) ++ui->revision;
}

void calf_ui_set_level(calf_ui_t *ui, int accel_y, int accel_z, int valid)
{
    int level_x;
    int level_y;
    int changed;
    if(!valid) {
        if(ui->level_valid) {
            ui->level_valid = 0;
            ++ui->revision;
        }
        return;
    }
    if(accel_y < -2000) accel_y = -2000;
    if(accel_y > 2000) accel_y = 2000;
    if(accel_z < -2000) accel_z = -2000;
    if(accel_z > 2000) accel_z = 2000;
    level_x = ui->level_valid ? (ui->level_x * 2 + accel_y) / 3 : accel_y;
    level_y = ui->level_valid ? (ui->level_y * 2 + accel_z) / 3 : accel_z;
    changed = !ui->level_valid || ui->level_x != level_x ||
              ui->level_y != level_y;
    ui->level_valid = 1;
    ui->level_x = level_x;
    ui->level_y = level_y;
    if(changed) ++ui->revision;
}

void calf_ui_set_gallery_preview(calf_ui_t *ui, const uint32_t *pixels,
                                 int width, int height)
{
    if(pixels == (const uint32_t *)0 || width <= 0 || height <= 0) {
        pixels = (const uint32_t *)0;
        width = 0;
        height = 0;
    }
    if(ui->gallery_preview_pixels != pixels ||
       ui->gallery_preview_width != width ||
       ui->gallery_preview_height != height) {
        ui->gallery_preview_pixels = pixels;
        ui->gallery_preview_width = width;
        ui->gallery_preview_height = height;
        ++ui->revision;
    }
}

void calf_ui_set_wifi_networks(calf_ui_t *ui,
                               const calf_wifi_network_t *networks, int count,
                               const char *current_ssid,
                               const char *ip_address)
{
    int index;
    if(count < 0) count = 0;
    if(count > CALF_WIFI_MAX_NETWORKS) count = CALF_WIFI_MAX_NETWORKS;
    for(index = 0; index < count; ++index) {
        text_copy(ui->wifi_networks[index].ssid,
                  sizeof(ui->wifi_networks[index].ssid),
                  networks[index].ssid);
        ui->wifi_networks[index].quality = networks[index].quality;
        ui->wifi_networks[index].level = networks[index].level;
    }
    for(; index < CALF_WIFI_MAX_NETWORKS; ++index)
        bytes_zero(&ui->wifi_networks[index], sizeof(ui->wifi_networks[index]));
    ui->wifi_network_count = count;
    ui->wifi_enabled = 1;
    ui->wifi_enabled_known = 1;
    ui->wifi_selected_index = -1;
    ui->wifi_list_offset = 0;
    text_copy(ui->wifi_current_ssid, sizeof(ui->wifi_current_ssid),
              current_ssid != (const char *)0 ? current_ssid : "");
    text_copy(ui->wifi_ip_address, sizeof(ui->wifi_ip_address),
              ip_address != (const char *)0 ? ip_address : "");
    ui->screen = CALF_SCREEN_WIFI_LIST;
    calf_ui_focus_default(ui);
    ++ui->revision;
}

void calf_ui_wifi_require_password(calf_ui_t *ui, int selection)
{
    if(selection < 0 || selection >= ui->wifi_network_count) return;
    wifi_clear_password(ui);
    ui->wifi_selected_index = selection;
    ui->wifi_keyboard_mode = 0;
    ui->pending_action = CALF_ACTION_NONE;
    ui->pending_selection = -1;
    ui->screen = CALF_SCREEN_WIFI_PASSWORD;
    calf_ui_focus_default(ui);
    text_copy(ui->message, sizeof(ui->message), "");
    ui->message_is_error = 0;
    ++ui->revision;
}

void calf_ui_set_wifi_enabled(calf_ui_t *ui, int enabled)
{
    enabled = enabled != 0;
    if(!ui->wifi_enabled_known || ui->wifi_enabled != enabled) {
        ui->wifi_enabled = enabled;
        ui->wifi_enabled_known = 1;
        if(!enabled) {
            ui->wifi_current_ssid[0] = '\0';
            ui->wifi_ip_address[0] = '\0';
            ui->wifi_network_count = 0;
        }
        ++ui->revision;
    }
}

void calf_ui_set_wifi_connection(calf_ui_t *ui, const char *current_ssid,
                                 const char *ip_address)
{
    const char *ssid = current_ssid != (const char *)0 ? current_ssid : "";
    const char *ip = ip_address != (const char *)0 ? ip_address : "";
    if(text_equal(ui->wifi_current_ssid, ssid) &&
       text_equal(ui->wifi_ip_address, ip))
        return;
    text_copy(ui->wifi_current_ssid, sizeof(ui->wifi_current_ssid), ssid);
    text_copy(ui->wifi_ip_address, sizeof(ui->wifi_ip_address), ip);
    ++ui->revision;
}

void calf_ui_set_update_ready(calf_ui_t *ui, int size_mb)
{
    ui->update_size_mb = size_mb;
    ui->update_ready = 1;
    ui->focus_index = 0;
    ui->focus_visible = 1;
    ui->screen = CALF_SCREEN_UPDATE_CONFIRM;
    ++ui->revision;
}

int calf_ui_sync_image_value(calf_ui_t *ui, calf_action_kind_t kind,
                             const char *value)
{
    const choice_t *choices = (const choice_t *)0;
    size_t count = 0;
    int *selection = (int *)0;
    int *known = (int *)0;
    int index;
    if(kind == CALF_ACTION_SET_EXPOSURE) {
        choices = k_exposures; count = ARRAY_SIZE(k_exposures);
        selection = &ui->exposure_index; known = &ui->exposure_known;
    }
    else if(kind == CALF_ACTION_SET_ISO) {
        choices = k_isos; count = ARRAY_SIZE(k_isos);
        selection = &ui->iso_index; known = &ui->iso_known;
    }
    else if(kind == CALF_ACTION_SET_WHITE_BALANCE) {
        choices = k_white_balances; count = ARRAY_SIZE(k_white_balances);
        selection = &ui->white_balance_index; known = &ui->white_balance_known;
    }
    else if(kind == CALF_ACTION_SET_EV) {
        choices = k_ev_values; count = ARRAY_SIZE(k_ev_values);
        selection = &ui->ev_index; known = &ui->ev_known;
    }
    else if(kind == CALF_ACTION_SET_ANTIFLICKER) {
        choices = k_antiflicker_values;
        count = ARRAY_SIZE(k_antiflicker_values);
        selection = &ui->antiflicker_index; known = &ui->antiflicker_known;
    }
    else if(kind >= CALF_ACTION_SET_BRIGHTNESS &&
            kind <= CALF_ACTION_SET_DNR) {
        int level = (int)kind - (int)CALF_ACTION_SET_BRIGHTNESS;
        choices = k_image_levels; count = ARRAY_SIZE(k_image_levels);
        selection = &ui->image_level_index[level];
        known = &ui->image_level_known[level];
    }
    else if(kind == CALF_ACTION_SET_EFFECT) {
        choices = k_image_effects; count = ARRAY_SIZE(k_image_effects);
        selection = &ui->effect_index; known = &ui->effect_known;
    }
    else return -1;
    index = choice_index_for_value(choices, count, value);
    if(index < 0) return -1;
    if(!*known || *selection != index) {
        *selection = index;
        *known = 1;
        ++ui->revision;
    }
    return 0;
}

int calf_ui_sync_audio_input(calf_ui_t *ui, int automatic, int input_type)
{
    int selection;
    if(automatic) selection = 0;
    else if(input_type >= 0 && input_type <= 2) selection = input_type + 1;
    else {
        if(ui->audio_input_known) {
            ui->audio_input_known = 0;
            ++ui->revision;
        }
        return -1;
    }
    if(!ui->audio_input_known || ui->audio_input_index != selection) {
        ui->audio_input_index = selection;
        ui->audio_input_known = 1;
        ++ui->revision;
    }
    return 0;
}

int calf_ui_sync_audio_volume(calf_ui_t *ui, calf_action_kind_t kind,
                              int value)
{
    int input;
    int selection;
    if(kind < CALF_ACTION_SET_BUILTIN_MIC_VOLUME ||
       kind > CALF_ACTION_SET_USB_MIC_VOLUME)
        return -1;
    input = (int)kind - (int)CALF_ACTION_SET_BUILTIN_MIC_VOLUME;
    if(value < 0 || value > 100 || value % 10 != 0) {
        if(ui->audio_input_volume_known[input]) {
            ui->audio_input_volume_known[input] = 0;
            ++ui->revision;
        }
        return -1;
    }
    selection = value / 10;
    if(!ui->audio_input_volume_known[input] ||
       ui->audio_input_volume_index[input] != selection) {
        ui->audio_input_volume_index[input] = selection;
        ui->audio_input_volume_known[input] = 1;
        ++ui->revision;
    }
    return 0;
}

int calf_ui_sync_speaker_volume(calf_ui_t *ui, int value)
{
    int selection;
    if(value < 0 || value > 140 || value % 10 != 0) {
        if(ui->speaker_volume_known) {
            ui->speaker_volume_known = 0;
            ++ui->revision;
        }
        return -1;
    }
    selection = value / 10;
    if(!ui->speaker_volume_known ||
       ui->speaker_volume_index != selection) {
        ui->speaker_volume_index = selection;
        ui->speaker_volume_known = 1;
        ++ui->revision;
    }
    return 0;
}

int calf_ui_sync_timezone(calf_ui_t *ui, const char *value)
{
    int selection = choice_index_for_value(
        k_timezones, ARRAY_SIZE(k_timezones), value);
    if(selection < 0) {
        if(ui->timezone_known) {
            ui->timezone_known = 0;
            ++ui->revision;
        }
        return -1;
    }
    if(!ui->timezone_known || ui->timezone_index != selection) {
        ui->timezone_index = selection;
        ui->timezone_known = 1;
        ++ui->revision;
    }
    return 0;
}

void calf_ui_sync_auto_time(calf_ui_t *ui, int enabled)
{
    int selection = enabled != 0;
    if(!ui->auto_time_known || ui->auto_time_index != selection) {
        ui->auto_time_index = selection;
        ui->auto_time_known = 1;
        ++ui->revision;
    }
}

void calf_ui_sync_datetime(calf_ui_t *ui, int year, int month, int day,
                           int hour, int minute, int second)
{
    int changed;
    if(year < 2020 || year > 2049 || month < 1 || month > 12 ||
       day < 1 || day > datetime_days_in_month(year, month) ||
       hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
       second < 0 || second > 59)
        return;
    if(ui->screen == CALF_SCREEN_ADJUST_DATETIME) return;
    changed = !ui->datetime_known || ui->datetime_year != year ||
              ui->datetime_month != month || ui->datetime_day != day ||
              ui->datetime_hour != hour || ui->datetime_minute != minute ||
              ui->datetime_second != second;
    if(changed) {
        ui->datetime_year = year;
        ui->datetime_month = month;
        ui->datetime_day = day;
        ui->datetime_hour = hour;
        ui->datetime_minute = minute;
        ui->datetime_second = second;
        ui->datetime_known = 1;
        ++ui->revision;
    }
}

int calf_ui_action_requires_primary(const calf_ui_t *ui,
                                    calf_action_t action)
{
    if(action.kind != CALF_ACTION_SNAPSHOT &&
       (action.kind != CALF_ACTION_RECORD_TOGGLE || ui->status.recording))
        return 0;
    return !ui->lens_known || ui->lens_index != 1;
}

void calf_ui_notice(calf_ui_t *ui, const char *message, int is_error)
{
    text_copy(ui->message, sizeof(ui->message), message);
    ui->message_is_error = is_error;
    ++ui->revision;
}
