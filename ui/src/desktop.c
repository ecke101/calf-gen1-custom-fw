#include "calf_ui.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void make_preview(uint32_t *pixels)
{
    int x;
    int y;
    for(y = 0; y < CALF_UI_HEIGHT; ++y) {
        for(x = 0; x < CALF_UI_WIDTH; ++x) {
            int half_x = x < 400 ? x : x - 400;
            int dx = half_x - 200;
            int dy = y - 240;
            int radius2 = dx * dx + dy * dy;
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            if(radius2 > 205 * 205) {
                red = 12;
                green = 15;
                blue = 18;
            }
            else {
                red = (uint8_t)(38 + (x < 400 ? 65 : 15) + y / 8);
                green = (uint8_t)(55 + half_x / 4);
                blue = (uint8_t)(75 + (x < 400 ? 20 : 75) - y / 12);
            }
            pixels[y * CALF_UI_WIDTH + x] =
                0xff000000u | ((uint32_t)red << 16) |
                ((uint32_t)green << 8) | blue;
        }
    }
}

static void update_mock_live_histogram(calf_ui_t *ui, unsigned phase)
{
    uint32_t bins[CALF_HISTOGRAM_BIN_COUNT];
    int index;
    for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index) {
        int first = index - (24 + (int)(phase % 9u));
        int second = index - 67;
        unsigned first_peak = first * first < 400
                                  ? (unsigned)(400 - first * first) : 0u;
        unsigned second_peak = second * second < 256
                                   ? (unsigned)(256 - second * second) : 0u;
        bins[index] = 8u + first_peak * 3u + second_peak * 2u;
    }
    calf_ui_set_live_histogram(ui, bins, 1);
}

static void update_mock_motion(calf_ui_t *ui, unsigned phase)
{
    int horizontal = (int)(phase % 80u);
    int vertical = (int)((phase + 23u) % 120u);
    if(horizontal > 40) horizontal = 80 - horizontal;
    if(vertical > 60) vertical = 120 - vertical;
    calf_ui_set_motion(ui, horizontal / 2 - 10, vertical - 30,
                       horizontal - 20, 1);
    calf_ui_set_level(ui, (horizontal - 20) * 8, (vertical - 30) * 6, 1);
}

static void complete_mock_action(calf_ui_t *ui, calf_action_t action)
{
    static const calf_wifi_network_t networks[] = {
        {"CALF-LAB", 4, -42},
        {"Studio Guest", 3, -57},
        {"Workshop", 2, -71},
    };
    const char *message = "APPLIED";
    if(action.kind == CALF_ACTION_GALLERY_ENTER)
        calf_ui_set_gallery(ui, "V0000001.MP4", 1, 0, 3, 0);
    else if(action.kind == CALF_ACTION_GALLERY_PREV)
        calf_ui_set_gallery(ui, "V0000003.JPG", 0, 2, 3, 0);
    else if(action.kind == CALF_ACTION_GALLERY_NEXT)
        calf_ui_set_gallery(ui, "V0000002.JPG", 0, 1, 3, 0);
    else if(action.kind == CALF_ACTION_GALLERY_DELETE)
        calf_ui_set_gallery(ui, "V0000002.JPG", 0, 0, 2, 0);
    else if(action.kind == CALF_ACTION_WIFI_SCAN)
        calf_ui_set_wifi_networks(ui, networks, 3, "CALF-LAB",
                                  "192.168.1.67");
    else if(action.kind == CALF_ACTION_WIFI_CONNECT_SAVED &&
            action.selection == 1) {
        calf_ui_wifi_require_password(ui, action.selection);
        return;
    }
    else if(action.kind == CALF_ACTION_FIRMWARE_CHECK)
        calf_ui_set_update_ready(ui, 257);
    else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_START)
        calf_ui_set_capture_sequence(
            ui, 1,
            calf_drive_mode_is_interval((size_t)ui->drive_mode_index),
            0,
            calf_drive_mode_delay_seconds((size_t)ui->drive_mode_index),
            0);
    else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL)
        calf_ui_set_capture_sequence(ui, 0, 0, 0, 0, 0);
    if(action.kind == CALF_ACTION_SNAPSHOT) message = "";
    else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_START)
        message = "";
    else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL)
        message = "CAPTURE STOPPED";
    else if(action.kind == CALF_ACTION_RECORD_TOGGLE) message = "";
    else if(action.kind == CALF_ACTION_SET_CAMERA_MODE ||
            action.kind == CALF_ACTION_SET_CAPTURE_MODE) message = "MODE UPDATED";
    else if(action.kind == CALF_ACTION_SET_RESOLUTION)
        message = "RESOLUTION UPDATED";
    else if(action.kind == CALF_ACTION_SET_DRIVE_MODE)
        message = "DRIVE MODE UPDATED";
    else if(action.kind >= CALF_ACTION_SET_ENCODING_CODEC &&
            action.kind <= CALF_ACTION_SET_RECORDING_COLOR_RANGE)
        message = "ENCODER UPDATED";
    else if(action.kind >= CALF_ACTION_GALLERY_ENTER &&
            action.kind <= CALF_ACTION_GALLERY_PLAY_TOGGLE) message = "";
    else if(action.kind == CALF_ACTION_GALLERY_DELETE) message = "DELETED";
    else if(action.kind == CALF_ACTION_WIFI_SCAN) message = "NETWORKS UPDATED";
    else if(action.kind == CALF_ACTION_WIFI_CONNECT_SAVED ||
            action.kind == CALF_ACTION_WIFI_CONNECT_PASSWORD)
        message = "WI-FI CONNECTED";
    else if(action.kind == CALF_ACTION_SET_WIFI_ENABLED)
        message = "WI-FI POWER UPDATED";
    else if(action.kind == CALF_ACTION_FIRMWARE_CHECK) message = "";
    else if(action.kind == CALF_ACTION_FIRMWARE_INSTALL)
        message = "DESKTOP SIMULATION ONLY";
    else if(action.kind == CALF_ACTION_LOAD_STOCK_UI)
        message = "CAMERA BUILD ONLY";
    calf_ui_complete_action(ui, action, 1, message);
}

static void sync_desktop_clock(calf_ui_t *ui)
{
    time_t now = time(NULL);
    struct tm local;
    if(localtime_r(&now, &local) != NULL)
        calf_ui_sync_datetime(ui, local.tm_year + 1900, local.tm_mon + 1,
                              local.tm_mday, local.tm_hour, local.tm_min,
                              local.tm_sec);
}

static int desktop_key(SDL_Keycode code, calf_key_t *key)
{
    if(code == SDLK_UP) *key = CALF_KEY_UP;
    else if(code == SDLK_DOWN) *key = CALF_KEY_DOWN;
    else if(code == SDLK_LEFT) *key = CALF_KEY_LEFT;
    else if(code == SDLK_RIGHT) *key = CALF_KEY_RIGHT;
    else if(code == SDLK_m || code == SDLK_RETURN) *key = CALF_KEY_MENU;
    else if(code == SDLK_BACKSPACE) *key = CALF_KEY_BACK;
    else if(code == SDLK_SPACE) *key = CALF_KEY_SHUTTER;
    else if(code == SDLK_a) *key = CALF_KEY_FILE;
    else if(code == SDLK_F1) *key = CALF_KEY_F1;
    else if(code == SDLK_F2) *key = CALF_KEY_F2;
    else if(code == SDLK_p) *key = CALF_KEY_POWER;
    else return 0;
    return 1;
}

static int save_screenshot(const char *path, calf_ui_t *ui,
                           uint32_t *preview, uint32_t *overlay)
{
    SDL_Surface *preview_surface;
    SDL_Surface *overlay_surface;
    SDL_Surface *result;
    int saved;
    calf_ui_render(ui, overlay, CALF_UI_WIDTH);
    preview_surface = SDL_CreateRGBSurfaceWithFormatFrom(
        preview, CALF_UI_WIDTH, CALF_UI_HEIGHT, 32,
        CALF_UI_WIDTH * (int)sizeof(uint32_t), SDL_PIXELFORMAT_ARGB8888);
    overlay_surface = SDL_CreateRGBSurfaceWithFormatFrom(
        overlay, CALF_UI_WIDTH, CALF_UI_HEIGHT, 32,
        CALF_UI_WIDTH * (int)sizeof(uint32_t), SDL_PIXELFORMAT_ARGB8888);
    result = SDL_CreateRGBSurfaceWithFormat(0, CALF_UI_WIDTH, CALF_UI_HEIGHT,
                                            32, SDL_PIXELFORMAT_ARGB8888);
    if(preview_surface == NULL || overlay_surface == NULL || result == NULL) {
        fprintf(stderr, "SDL screenshot surface failed: %s\n", SDL_GetError());
        SDL_FreeSurface(result);
        SDL_FreeSurface(overlay_surface);
        SDL_FreeSurface(preview_surface);
        return 1;
    }
    SDL_SetSurfaceBlendMode(preview_surface, SDL_BLENDMODE_NONE);
    SDL_SetSurfaceBlendMode(overlay_surface, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(preview_surface, NULL, result, NULL);
    SDL_BlitSurface(overlay_surface, NULL, result, NULL);
    saved = SDL_SaveBMP(result, path) == 0 ? 0 : 1;
    if(saved != 0) fprintf(stderr, "SDL_SaveBMP failed: %s\n", SDL_GetError());
    SDL_FreeSurface(result);
    SDL_FreeSurface(overlay_surface);
    SDL_FreeSurface(preview_surface);
    return saved;
}

static void seed_mock_power_history(calf_ui_t *ui)
{
    int index;
    ui->status.battery_percent = 92;
    ui->status.usb_power = 1;
    ui->status.system_temp = 56;
    ui->status.core_temp = 59;
    for(index = 0; index < CALF_POWER_HISTORY_COUNT; ++index) {
        calf_power_sample_t sample;
        int wave = index % 48;
        sample.usb_mv = 4800;
        sample.usb_ma = 950 + (wave < 24 ? wave : 48 - wave) * 5;
        sample.usb_mw = sample.usb_mv * sample.usb_ma / 1000;
        sample.battery_mv = 8320 + (index % 3) * 64;
        sample.battery_ma = index < 70 ? 256 :
                            (index < 250 ? -256 : -192);
        sample.battery_mw = sample.battery_mv * sample.battery_ma / 1000;
        sample.device_mw = sample.usb_mw + sample.battery_mw;
        sample.recording = index >= 70 && index < 150;
        sample.valid = 1;
        calf_ui_add_power_sample(ui, &sample);
    }
}

static int select_screenshot_screen(calf_ui_t *ui, const char *name)
{
    if(strcmp(name, "main") == 0) ui->screen = CALF_SCREEN_MAIN;
    else if(strcmp(name, "main-motion") == 0) {
        int sample;
        ui->screen = CALF_SCREEN_MAIN;
        for(sample = 0; sample < 13; ++sample)
            calf_ui_set_motion(ui, 0, 0, 0, 1);
        calf_ui_set_level(ui, 0, 0, 1);
    }
    else if(strcmp(name, "main-histogram") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        ui->live_histogram_visible = 1;
        update_mock_live_histogram(ui, 0);
    }
    else if(strcmp(name, "settings") == 0) ui->screen = CALF_SCREEN_SETTINGS;
    else if(strcmp(name, "image") == 0) {
        ui->screen = CALF_SCREEN_SETTINGS_IMAGE;
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_WHITE_BALANCE, "daylight");
        (void)calf_ui_sync_image_value(ui, CALF_ACTION_SET_EV, "1");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_BRIGHTNESS, "10");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_CONTRAST, "8");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_SATURATION, "12");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_SHARPNESS, "9");
        (void)calf_ui_sync_image_value(ui, CALF_ACTION_SET_DNR, "5");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_ANTIFLICKER, "50hz");
        (void)calf_ui_sync_image_value(
            ui, CALF_ACTION_SET_EFFECT, "none");
    }
    else if(strcmp(name, "general") == 0) ui->screen = CALF_SCREEN_SETTINGS_GENERAL;
    else if(strcmp(name, "language") == 0) ui->screen = CALF_SCREEN_LANGUAGE;
    else if(strcmp(name, "indicator-led") == 0) {
        calf_ui_set_indicator_led(ui, 1);
        ui->screen = CALF_SCREEN_INDICATOR_LED;
    }
    else if(strcmp(name, "power-history") == 0) {
        ui->screen = CALF_SCREEN_POWER_HISTORY;
        seed_mock_power_history(ui);
    }
    else if(strcmp(name, "stock-ui-confirm") == 0) {
        ui->screen = CALF_SCREEN_STOCK_UI_CONFIRM;
        ui->focus_index = 0;
        ui->focus_visible = 1;
    }
    else if(strcmp(name, "camera") == 0) {
        calf_ui_set_capture_mode(ui, CALF_CAPTURE_VIDEO);
        (void)calf_ui_sync_resolution(ui, CALF_CAPTURE_VIDEO, "VR180_6K");
        ui->screen = CALF_SCREEN_SETTINGS_CAMERA;
    }
    else if(strcmp(name, "resolution") == 0) {
        calf_ui_set_capture_mode(ui, CALF_CAPTURE_VIDEO);
        (void)calf_ui_sync_resolution(ui, CALF_CAPTURE_VIDEO, "VR180_6K");
        ui->screen = CALF_SCREEN_CAMERA_RESOLUTION;
    }
    else if(strcmp(name, "drive-mode") == 0) {
        int selection = calf_drive_mode_index_from_value("interval-10");
        calf_ui_set_drive_mode(ui, selection);
        ui->screen = CALF_SCREEN_DRIVE_MODE;
        ui->focus_index = selection;
        ui->focus_visible = 1;
    }
    else if(strcmp(name, "video-recording") == 0 ||
            strcmp(name, "encoding") == 0) {
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_ENCODING_CODEC, "H264");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_IMAGE_QUALITY, "high");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_ENCODING_COLOR_RANGE, "0");
        ui->screen = CALF_SCREEN_SETTINGS_ENCODING;
    }
    else if(strcmp(name, "live-streaming") == 0 ||
            strcmp(name, "recording-settings") == 0) {
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_CODEC, "H264_HIGH");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_BITRATE, "30000");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_GOP, "20");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_COLOR_RANGE, "0");
        ui->screen = CALF_SCREEN_SETTINGS_RECORDING;
    }
    else if(strcmp(name, "live-codec") == 0 ||
            strcmp(name, "recording-codec") == 0) {
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_CODEC, "H264_HIGH");
        ui->screen = CALF_SCREEN_RECORDING_CODEC;
    }
    else if(strcmp(name, "uvc") == 0)
        ui->screen = CALF_SCREEN_SETTINGS_LIVE;
    else if(strcmp(name, "network") == 0) {
        ui->screen = CALF_SCREEN_SETTINGS_NETWORK;
        calf_ui_set_wifi_enabled(ui, 1);
    }
    else if(strcmp(name, "brightness") == 0)
        ui->screen = CALF_SCREEN_IMAGE_BRIGHTNESS;
    else if(strcmp(name, "display") == 0) ui->screen = CALF_SCREEN_DISPLAY;
    else if(strcmp(name, "display-off") == 0)
        ui->screen = CALF_SCREEN_DISPLAY_OFF;
    else if(strcmp(name, "audio") == 0)
        ui->screen = CALF_SCREEN_SETTINGS_AUDIO;
    else if(strcmp(name, "audio-input") == 0) {
        ui->screen = CALF_SCREEN_AUDIO_INPUT;
        (void)calf_ui_sync_audio_input(ui, 0, 0);
    }
    else if(strcmp(name, "audio-volume") == 0) {
        ui->screen = CALF_SCREEN_AUDIO_BUILTIN_VOLUME;
        (void)calf_ui_sync_audio_volume(
            ui, CALF_ACTION_SET_BUILTIN_MIC_VOLUME, 80);
    }
    else if(strcmp(name, "speaker-volume") == 0) {
        ui->screen = CALF_SCREEN_AUDIO_SPEAKER_VOLUME;
        (void)calf_ui_sync_speaker_volume(ui, 50);
    }
    else if(strcmp(name, "datetime") == 0) {
        ui->screen = CALF_SCREEN_SETTINGS_DATETIME;
        (void)calf_ui_sync_timezone(ui, "EST-1");
        calf_ui_sync_auto_time(ui, 1);
        calf_ui_sync_datetime(ui, 2026, 8, 6, 14, 5, 9);
    }
    else if(strcmp(name, "timezone") == 0) {
        ui->screen = CALF_SCREEN_TIMEZONE;
        (void)calf_ui_sync_timezone(ui, "EST-1");
    }
    else if(strcmp(name, "adjust-datetime") == 0) {
        calf_ui_sync_auto_time(ui, 0);
        calf_ui_sync_datetime(ui, 2026, 8, 6, 14, 5, 9);
        ui->screen = CALF_SCREEN_ADJUST_DATETIME;
    }
    else if(strcmp(name, "charging") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        ui->status.battery_percent = 55;
        ui->status.usb_power = 1;
    }
    else if(strcmp(name, "charged") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        ui->status.battery_percent = 100;
        ui->status.usb_power = 1;
    }
    else if(strcmp(name, "video") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        calf_ui_set_capture_mode(ui, CALF_CAPTURE_VIDEO);
    }
    else if(strcmp(name, "recording") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        calf_ui_set_capture_mode(ui, CALF_CAPTURE_VIDEO);
        ui->status.recording = 1;
        ui->status.recording_seconds = 754;
    }
    else if(strcmp(name, "capture-mode") == 0) {
        ui->screen = CALF_SCREEN_CAPTURE_MODE;
    }
    else if(strcmp(name, "gallery") == 0) {
        ui->screen = CALF_SCREEN_GALLERY;
        calf_ui_set_gallery(ui, "V1234567.MP4", 1, 4, 12, 0);
    }
    else if(strcmp(name, "gallery-playing") == 0) {
        ui->screen = CALF_SCREEN_GALLERY;
        (void)calf_ui_sync_speaker_volume(ui, 5 * 10);
        calf_ui_set_gallery(ui, "V1234567.MP4", 1, 4, 12, 1);
        calf_ui_set_gallery_playback(ui, 1, 12, 125, 1);
    }
    else if(strcmp(name, "delete-confirm") == 0) {
        ui->screen = CALF_SCREEN_DELETE_CONFIRM;
        calf_ui_set_gallery(ui, "V1234567.MP4", 1, 4, 12, 0);
        ui->focus_visible = 1;
    }
    else if(strcmp(name, "wifi") == 0) {
        static const calf_wifi_network_t networks[] = {
            {"CALF-LAB", 4, -42},
            {"Studio Guest", 3, -57},
            {"Workshop", 2, -71},
        };
        calf_ui_set_wifi_networks(ui, networks, 3, "CALF-LAB",
                                  "192.168.1.67");
    }
    else if(strcmp(name, "wifi-password") == 0) {
        static const calf_wifi_network_t networks[] = {
            {"Studio Guest", 3, -57},
        };
        calf_ui_set_wifi_networks(ui, networks, 1, "", "");
        calf_ui_wifi_require_password(ui, 0);
    }
    else if(strcmp(name, "wifi-off-confirm") == 0) {
        calf_ui_set_wifi_enabled(ui, 1);
        ui->screen = CALF_SCREEN_WIFI_OFF_CONFIRM;
        ui->focus_index = 0;
        ui->focus_visible = 1;
    }
    else if(strcmp(name, "update-confirm") == 0)
        calf_ui_set_update_ready(ui, 257);
    else if(strcmp(name, "limits") == 0) {
        ui->screen = CALF_SCREEN_MAIN;
        ui->exposure_index = 10;
        ui->exposure_known = 1;
        ui->iso_index = 8;
        ui->iso_known = 1;
    }
    else return -1;
    ++ui->revision;
    return 0;
}

int main(int argc, char **argv)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *preview_texture;
    SDL_Texture *overlay_texture;
    static uint32_t preview[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    static uint32_t overlay[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_backend_status_t status = {
        1, 0, 82, 64321, 47, 53, 0, 0, 0, 0, "192.168.50.24"
    };
    calf_ui_t ui;
    uint32_t rendered_revision = 0;
    uint32_t next_status_tick;
    uint32_t next_motion_tick;
    uint32_t clear_notice_tick = 0;
    uint32_t last_activity_tick;
    int running = 1;
    int smoke_frames = 0;

    make_preview(preview);
    calf_ui_init(&ui);
    (void)calf_ui_sync_timezone(&ui, "UTC");
    calf_ui_sync_auto_time(&ui, 0);
    sync_desktop_clock(&ui);
    calf_ui_set_status(&ui, &status);
    calf_ui_notice(&ui, "", 0);
    if((argc == 3 || argc == 4) && strcmp(argv[1], "--screenshot") == 0) {
        if(argc == 4 && select_screenshot_screen(&ui, argv[3]) != 0) {
            fprintf(stderr, "unknown screenshot screen: %s\n", argv[3]);
            return 2;
        }
        if(SDL_Init(0) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        running = save_screenshot(argv[2], &ui, preview, overlay);
        SDL_Quit();
        return running;
    }
    if(argc == 2 && strcmp(argv[1], "--smoke-test") == 0)
        smoke_frames = 4;
    else if(argc != 1) {
        fprintf(stderr,
                "usage: %s [--screenshot FILE.bmp "
                "[main|main-motion|settings|camera|resolution|drive-mode|image|video-recording|"
                "live-streaming|live-codec|uvc|"
                "general|language|power-history|brightness|display|display-off|"
                "audio|audio-input|audio-volume|speaker-volume|"
                "datetime|timezone|"
                "adjust-datetime|charging|charged|video|recording|limits|wifi|"
                "gallery|gallery-playing|delete-confirm|network|"
                "wifi-password|wifi-off-confirm|update-confirm]]\n"
                "       %s [--smoke-test]\n",
                argv[0],
                argv[0]);
        return 2;
    }
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("CALF replacement UI prototype",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              CALF_UI_WIDTH, CALF_UI_HEIGHT,
                              SDL_WINDOW_ALLOW_HIGHDPI);
    renderer = window != NULL ? SDL_CreateRenderer(window, -1,
                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if(renderer == NULL && window != NULL)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if(window == NULL || renderer == NULL) {
        fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    preview_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC,
                                        CALF_UI_WIDTH, CALF_UI_HEIGHT);
    overlay_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        CALF_UI_WIDTH, CALF_UI_HEIGHT);
    if(preview_texture == NULL || overlay_texture == NULL) {
        fprintf(stderr, "SDL texture creation failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetTextureBlendMode(overlay_texture, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(preview_texture, NULL, preview,
                      CALF_UI_WIDTH * (int)sizeof(uint32_t));

    next_status_tick = SDL_GetTicks() + 1000;
    next_motion_tick = SDL_GetTicks();
    last_activity_tick = SDL_GetTicks();

    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT) running = 0;
            else if(event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                calf_key_t key;
                last_activity_tick = SDL_GetTicks();
                if(event.key.keysym.sym == SDLK_ESCAPE) {
                    if(!ui.lcd_on) {
                        calf_action_t action = calf_ui_key_press(&ui, CALF_KEY_POWER);
                        complete_mock_action(&ui, action);
                    }
                    else if(ui.screen == CALF_SCREEN_MAIN) running = 0;
                    else (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
                }
                else if(desktop_key(event.key.keysym.sym, &key)) {
                    calf_action_t action = calf_ui_key_press(
                        &ui, ui.lcd_on ? key : CALF_KEY_POWER);
                    if(action.kind != CALF_ACTION_NONE) {
                        complete_mock_action(&ui, action);
                        clear_notice_tick = SDL_GetTicks() + 1500;
                    }
                }
            }
            else if(event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                int window_w;
                int window_h;
                int x;
                int y;
                calf_action_t action;
                last_activity_tick = SDL_GetTicks();
                SDL_GetWindowSize(window, &window_w, &window_h);
                x = event.button.x * CALF_UI_WIDTH / window_w;
                y = event.button.y * CALF_UI_HEIGHT / window_h;
                action = ui.lcd_on ? calf_ui_tap(&ui, x, y)
                                   : calf_ui_key_press(&ui, CALF_KEY_POWER);
                if(action.kind != CALF_ACTION_NONE) {
                    complete_mock_action(&ui, action);
                    clear_notice_tick = SDL_GetTicks() + 1500;
                }
            }
        }

        if(clear_notice_tick != 0 &&
           (int32_t)(SDL_GetTicks() - clear_notice_tick) >= 0) {
            calf_ui_notice(&ui, "", 0);
            clear_notice_tick = 0;
        }

        if((int32_t)(SDL_GetTicks() - next_status_tick) >= 0) {
            status.recording = ui.status.recording;
            status.recording_seconds = status.recording
                                           ? status.recording_seconds + 1 : 0;
            status.battery_percent = status.battery_percent > 20
                                       ? status.battery_percent - 1 : 82;
            calf_ui_set_status(&ui, &status);
            if(ui.live_histogram_visible)
                update_mock_live_histogram(&ui, SDL_GetTicks() / 1000u);
            sync_desktop_clock(&ui);
            next_status_tick += 1000;
        }

        if((int32_t)(SDL_GetTicks() - next_motion_tick) >= 0) {
            update_mock_motion(&ui, SDL_GetTicks() / 1000u);
            next_motion_tick += 67;
        }

        if(ui.lcd_on && ui.display_off_seconds >= 0 &&
           (uint32_t)(SDL_GetTicks() - last_activity_tick) >=
               (uint32_t)ui.display_off_seconds * 1000u) {
            calf_action_t action = calf_ui_key_press(&ui, CALF_KEY_POWER);
            if(action.kind != CALF_ACTION_NONE) complete_mock_action(&ui, action);
            last_activity_tick = SDL_GetTicks();
        }

        if(rendered_revision != ui.revision) {
            calf_ui_render(&ui, overlay, CALF_UI_WIDTH);
            SDL_UpdateTexture(overlay_texture, NULL, overlay,
                              CALF_UI_WIDTH * (int)sizeof(uint32_t));
            rendered_revision = ui.revision;
        }
        SDL_RenderClear(renderer);
        if(ui.lcd_on) {
            SDL_RenderCopy(renderer, preview_texture, NULL, NULL);
            SDL_RenderCopy(renderer, overlay_texture, NULL, NULL);
        }
        SDL_RenderPresent(renderer);
        if(smoke_frames > 0 && --smoke_frames == 0) running = 0;
        SDL_Delay(8);
    }

    SDL_DestroyTexture(overlay_texture);
    SDL_DestroyTexture(preview_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
