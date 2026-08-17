#include "calf_ui.h"
#include "calf_sha256.h"
#include "../src/target_internal.h"
#include "../src/ui_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_defaults(void)
{
    calf_ui_t ui;
    calf_ui_init(&ui);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(strcmp(calf_exposure_label((size_t)ui.exposure_index), "AUTO") == 0);
    assert(strcmp(calf_iso_label((size_t)ui.iso_index), "AUTO") == 0);
    assert(ui.exposure_known == 0);
    assert(ui.iso_known == 0);
    assert(ui.lens_known == 0);
    assert(ui.capture_mode == CALF_CAPTURE_PHOTO);
    assert(ui.drive_mode_known == 1);
    assert(ui.drive_mode_index == 0);
    assert(ui.pending_action == CALF_ACTION_NONE);
    assert(strcmp(ui.message, "BOOTING") == 0);
}

static void test_drive_mode_settings_and_capture_controls(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status = {
        1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, "",
    };
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    assert(calf_drive_mode_count() == CALF_DRIVE_MODE_COUNT);
    assert(strcmp(calf_drive_mode_label(10), "INTERVAL 10S") == 0);
    assert(strcmp(calf_drive_mode_value(10), "interval-10") == 0);
    assert(calf_drive_mode_index_from_value("timer-5") == 2);
    assert(calf_drive_mode_delay_seconds(2) == 5);
    assert(calf_drive_mode_is_interval(2) == 0);
    assert(calf_drive_mode_is_interval(10) == 1);
    assert(calf_drive_mode_is_burst(5) == 1);
    assert(calf_drive_mode_shot_limit(5) == 5u);
    assert(calf_drive_mode_shot_limit(10) == 0u);

    (void)calf_ui_tap(&ui, 80, 440);
    (void)calf_ui_tap(&ui, 100, 100);
    assert(ui.screen == CALF_SCREEN_SETTINGS_CAMERA);
    (void)calf_ui_tap(&ui, 500, 300);
    assert(ui.screen == CALF_SCREEN_DRIVE_MODE);
    {
        rect_t cell = drive_mode_cell(10);
        action = calf_ui_tap(&ui, cell.x + cell.w / 2,
                             cell.y + cell.h / 2);
    }
    assert(action.kind == CALF_ACTION_SET_DRIVE_MODE);
    assert(action.selection == 10);
    assert(strcmp(action.value, "interval-10") == 0);
    calf_ui_complete_action(&ui, action, 1, "DRIVE MODE UPDATED");
    assert(ui.drive_mode_index == 10);
    assert(ui.screen == CALF_SCREEN_SETTINGS_CAMERA);

    ui.screen = CALF_SCREEN_MAIN;
    action = calf_ui_tap(&ui, 550, 440);
    assert(action.kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_DRIVE_MODE);
    assert(ui.return_to_main == 1);
    action = calf_ui_tap(&ui, 250, 250);
    assert(action.kind == CALF_ACTION_SET_DRIVE_MODE);
    assert(action.selection == 1);
    calf_ui_complete_action(&ui, action, 1, "DRIVE MODE UPDATED");
    assert(ui.drive_mode_index == 1);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(ui.return_to_main == 0);
    assert(calf_ui_tap(&ui, 550, 440).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_DRIVE_MODE);
    assert(calf_ui_tap(&ui, 250, 250).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(ui.return_to_main == 0);

    ui.drive_mode_index = 10;
    action = calf_ui_key_press(&ui, CALF_KEY_SHUTTER);
    assert(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_START);
    calf_ui_set_capture_sequence(&ui, 1, 1, 0, 10, 0);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.message[0] == '\0');
    assert(calf_ui_key_press(&ui, CALF_KEY_DOWN).kind == CALF_ACTION_NONE);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[220 * CALF_UI_WIDTH + 400] >> 24) != 0);
    action = calf_ui_key_press(&ui, CALF_KEY_SHUTTER);
    assert(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL);
    calf_ui_set_capture_sequence(&ui, 0, 0, 0, 0, 0);
    calf_ui_complete_action(&ui, action, 1, "CAPTURE STOPPED");
    assert(ui.capture_sequence_active == 0);
}

static void test_capture_sequence_scheduler(void)
{
    capture_sequence_t sequence;
    capture_sequence_init(&sequence);
    assert(sequence.active == 0);

    assert(capture_sequence_start(&sequence, 3, 1000u) == 0);
    assert(sequence.active == 1);
    assert(sequence.interval == 0);
    assert(capture_sequence_remaining_seconds(&sequence, 1000u) == 10);
    assert(capture_sequence_capture_due(&sequence, 10999u) == 0);
    assert(capture_sequence_capture_due(&sequence, 11000u) == 1);
    capture_sequence_complete_capture(&sequence, 1, 11500u);
    assert(sequence.active == 0);
    assert(sequence.shot_count == 1u);

    assert(capture_sequence_start(&sequence, 10, 1000u) == 0);
    assert(sequence.interval == 1);
    assert(sequence.deep_idle_enabled == 1);
    assert(capture_sequence_capture_due(&sequence, 1000u) == 1);
    capture_sequence_complete_capture(&sequence, 1, 1500u);
    assert(sequence.shot_count == 1u);
    assert(sequence.next_capture_ms == 11000u);
    assert(capture_sequence_should_sleep(&sequence, 1500u) == 1);
    capture_sequence_set_sleeping(&sequence, 1);
    assert(capture_sequence_should_wake(&sequence, 8999u) == 0);
    assert(capture_sequence_should_wake(&sequence, 9000u) == 1);
    capture_sequence_set_sleeping(&sequence, 0);
    assert(capture_sequence_capture_due(&sequence, 11000u) == 1);
    capture_sequence_complete_capture(&sequence, 1, 33000u);
    assert(sequence.overrun == 1);
    assert(sequence.next_capture_ms == 41000u);

    assert(capture_sequence_start(&sequence, 9, 0u) == 0);
    assert(sequence.interval_seconds == 5u);
    assert(sequence.deep_idle_enabled == 0);
    assert(capture_sequence_should_sleep(&sequence, 0u) == 0);
    capture_sequence_complete_capture(&sequence, 0, 1u);
    assert(sequence.active == 0);
    assert(capture_sequence_start(&sequence, 0, 0u) == -1);

    assert(capture_sequence_start(&sequence, 5, 2000u) == 0);
    assert(sequence.burst == 1);
    assert(sequence.interval == 0);
    assert(sequence.shot_limit == 5u);
    assert(capture_sequence_capture_due(&sequence, 2000u) == 1);
    capture_sequence_complete_capture(&sequence, 1, 2100u);
    assert(sequence.active == 1 && sequence.shot_count == 1u);
    assert(capture_sequence_capture_due(&sequence, 2100u) == 1);
    capture_sequence_complete_capture(&sequence, 1, 2200u);
    capture_sequence_complete_capture(&sequence, 1, 2300u);
    capture_sequence_complete_capture(&sequence, 1, 2400u);
    capture_sequence_complete_capture(&sequence, 1, 2500u);
    assert(sequence.active == 0 && sequence.shot_count == 5u);
}

static void test_night_preview_timing(void)
{
    assert(night_preview_fps_for_exposure("0.0666667") == 15);
    assert(night_preview_fps_for_exposure("0.125") == 8);
    assert(night_preview_fps_for_exposure("0.25") == 4);
    assert(night_preview_fps_for_exposure("0.5") == 4);
    assert(night_preview_fps_for_exposure("1") == 4);
    assert(night_preview_fps_for_exposure("2") == 4);
    assert(night_preview_fps_for_exposure("4") == 4);
    assert(night_preview_fps_for_exposure("8") == 4);
    assert(night_preview_fps_for_exposure("12") == 4);
    assert(night_preview_fps_for_exposure("0.0333333") == 0);
    assert(strcmp(night_preview_exposure_for_fps(15), "0.0666667") == 0);
    assert(strcmp(night_preview_exposure_for_fps(8), "0.125") == 0);
    assert(strcmp(night_preview_exposure_for_fps(4), "0.24") == 0);
    assert(night_preview_exposure_for_fps(30) == (const char *)0);
    assert(night_image_action_is_transient(
               CALF_CAPTURE_NIGHT, CALF_ACTION_SET_EXPOSURE) == 1);
    assert(night_image_action_is_transient(
               CALF_CAPTURE_NIGHT, CALF_ACTION_SET_ISO) == 1);
    assert(night_image_action_is_transient(
               CALF_CAPTURE_PHOTO, CALF_ACTION_SET_EXPOSURE) == 0);
    assert(night_image_action_is_transient(
               CALF_CAPTURE_NIGHT, CALF_ACTION_SET_WHITE_BALANCE) == 0);
}

static void test_drive_mode_row_layout_and_navigation(void)
{
    calf_backend_status_t status = {
        1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, "",
    };
    int index;
    for(index = 0; index < CALF_DRIVE_MODE_COUNT; ++index) {
        calf_ui_t ui;
        calf_action_t action;
        rect_t cell = drive_mode_cell(index);
        assert(cell.x >= 184 && cell.y >= 84);
        assert(cell.x + cell.w <= CALF_UI_WIDTH);
        assert(cell.y + cell.h <= CALF_UI_HEIGHT);
        calf_ui_init(&ui);
        calf_ui_set_status(&ui, &status);
        ui.screen = CALF_SCREEN_DRIVE_MODE;
        action = calf_ui_tap(&ui, cell.x + cell.w / 2,
                             cell.y + cell.h / 2);
        if(index == 0) {
            assert(action.kind == CALF_ACTION_NONE);
            assert(ui.screen == CALF_SCREEN_SETTINGS_CAMERA);
        }
        else {
            assert(action.kind == CALF_ACTION_SET_DRIVE_MODE);
            assert(action.selection == index);
        }
    }

    {
        calf_ui_t ui;
        calf_action_t action;
        calf_ui_init(&ui);
        calf_ui_set_status(&ui, &status);
        calf_ui_set_drive_mode(&ui, 10);
        ui.screen = CALF_SCREEN_DRIVE_MODE;
        calf_ui_focus_default(&ui);
        assert(ui.focus_index == 10);
        (void)calf_ui_key_press(&ui, CALF_KEY_UP);
        assert(ui.focus_index == 5);
        (void)calf_ui_key_press(&ui, CALF_KEY_UP);
        assert(ui.focus_index == 2);
        (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
        assert(ui.focus_index == 5);
        (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
        assert(ui.focus_index == 10);
        (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
        assert(ui.focus_index == 11);
        action = calf_ui_key_press(&ui, CALF_KEY_MENU);
        assert(action.kind == CALF_ACTION_SET_DRIVE_MODE);
        assert(action.selection == 11);
    }
}

static void test_sha256_known_vector_and_chunking(void)
{
    static const unsigned char expected[32] = {
        0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
        0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
        0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
        0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu,
    };
    calf_sha256_t context;
    unsigned char digest[32];
    calf_sha256_init(&context);
    calf_sha256_update(&context, "a", 1);
    calf_sha256_update(&context, "bc", 2);
    calf_sha256_final(&context, digest);
    assert(memcmp(digest, expected, sizeof(expected)) == 0);
}

static void test_exposure_is_confirmed_only_after_success(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    (void)calf_ui_tap(&ui, 300, 40);
    assert(ui.screen == CALF_SCREEN_EXPOSURE);
    action = calf_ui_tap(&ui, 20, 100);
    assert(action.kind == CALF_ACTION_SET_EXPOSURE);
    assert(strcmp(action.value, "0.5") == 0);
    assert(ui.exposure_index == 4);
    calf_ui_complete_action(&ui, action, 0, "FAILED");
    assert(ui.exposure_index == 4);
    assert(ui.message_is_error == 1);

    action = calf_ui_tap(&ui, 20, 100);
    calf_ui_complete_action(&ui, action, 1, "APPLIED");
    assert(ui.exposure_index == 0);
    assert(ui.exposure_known == 1);
    assert(ui.screen == CALF_SCREEN_MAIN);
}

static void test_recording_blocks_lens_switch(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 1, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    calf_action_t action;
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP RECORDING") != NULL);
}

static void test_live_and_playback_block_lens_switch(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 0, 80, 1000, 50, 55, 1, 0, 0, 0, ""};
    calf_action_t action;
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP LIVE") != NULL);

    status.streaming = 0;
    status.playback = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP PLAYBACK") != NULL);
}

static void test_unknown_status_blocks_lens_switch(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {0, 0, 80, 1000, 50, 55, -1, -1, -1, 0, ""};
    calf_action_t action;
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STATUS UNKNOWN") != NULL);
}

static void test_status_changes_revision(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 0, 72, 2048, 48, 52, 0, 0, 0, 0, ""};
    uint32_t revision;
    calf_ui_init(&ui);
    revision = ui.revision;
    calf_ui_set_status(&ui, &status);
    assert(ui.revision > revision);
    revision = ui.revision;
    calf_ui_set_status(&ui, &status);
    assert(ui.revision == revision);
}

static void test_render_keeps_preview_area_transparent(void)
{
    calf_ui_t ui;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[200 * CALF_UI_WIDTH + 400] == 0);
    assert((pixels[20 * CALF_UI_WIDTH + 20] >> 24) != 0);
    assert((pixels[430 * CALF_UI_WIDTH + 400] >> 24) != 0);
}

static void test_live_histogram_button_and_render(void)
{
    calf_ui_t ui;
    uint32_t bins[CALF_HISTOGRAM_BIN_COUNT];
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    uint32_t revision;
    int index;
    calf_ui_init(&ui);
    assert(calf_ui_tap(&ui, 400, 440).kind == CALF_ACTION_NONE);
    assert(ui.live_histogram_visible == 1);
    assert(ui.live_histogram_valid == 0);
    for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index)
        bins[index] = (uint32_t)(index + 1);
    calf_ui_set_live_histogram(&ui, bins, 1);
    assert(ui.live_histogram_valid == 1);
    assert(ui.live_histogram_error == 0);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[200 * CALF_UI_WIDTH + 500] >> 24) != 0);
    revision = ui.revision;
    calf_ui_set_live_histogram(&ui, bins, 1);
    assert(ui.revision == revision);
    calf_ui_set_live_histogram(&ui, (const uint32_t *)0, -1);
    assert(ui.live_histogram_valid == 0);
    assert(ui.live_histogram_error == 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_F1).kind == CALF_ACTION_NONE);
    assert(ui.live_histogram_visible == 0);
}

static void test_motion_reticle_and_settle_state(void)
{
    calf_ui_t ui;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    uint32_t revision;
    int sample;
    calf_ui_init(&ui);
    assert(ui.motion_valid == 0);
    revision = ui.revision;
    for(sample = 0; sample < 5; ++sample)
        calf_ui_set_motion(&ui, 65 + sample % 2, -85 - sample % 2,
                           -70 + sample % 2, 1);
    assert(ui.revision > revision);
    assert(ui.motion_valid == 1);
    assert(ui.motion_calibration_samples == 5);
    assert(ui.motion_bias_x == 65);
    assert(ui.motion_bias_y == -85);
    assert(ui.motion_bias_z >= -70 && ui.motion_bias_z <= -69);
    calf_ui_set_level(&ui, -6, -13, 1);
    assert(ui.level_valid == 1);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[240 * CALF_UI_WIDTH + 400] >> 24) != 0);
    for(sample = 0; sample < 3; ++sample)
        calf_ui_set_motion(&ui, 66, -84, -69, 1);
    assert(ui.motion_stable_samples == 3);
    assert(ui.motion_score == 0);
    calf_ui_set_motion(&ui, 105, -145, -670, 1);
    assert(ui.motion_score >= 10 && ui.motion_score <= 25);
    assert(ui.motion_stable_samples == 0);
    calf_ui_set_motion(&ui, 0, 0, 0, 0);
    assert(ui.motion_valid == 0);
    assert(ui.motion_stable_samples == 0);
    calf_ui_set_level(&ui, 0, 0, 0);
    assert(ui.level_valid == 0);
}

static void test_main_bottom_button_touch_areas(void)
{
    calf_ui_t ui;
    calf_ui_init(&ui);
    assert(calf_ui_tap(&ui, 50, 30).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_WHITE_BALANCE);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 180, 30).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_EV);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 300, 30).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_EXPOSURE);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 450, 30).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ISO);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 162, 440).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 550, 440).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_DRIVE_MODE);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_MAIN);
    assert(calf_ui_tap(&ui, 80, 440).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
}

static void test_battery_charge_states_render(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 0, 55, 2048, 48, 52, 0, 0, 1, 0, ""};
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[23 * CALF_UI_WIDTH + 704] == 0xffffe49au);

    status.battery_percent = 100;
    calf_ui_set_status(&ui, &status);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[22 * CALF_UI_WIDTH + 692] == 0xff238a50u);
}

static void test_main_shows_image_values_and_connected_wifi_icon(void)
{
    calf_ui_t ui;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    ui.white_balance_index = 4;
    ui.white_balance_known = 1;
    ui.ev_index = 5;
    ui.ev_known = 1;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[41 * CALF_UI_WIDTH + 664] != 0xff55e6b5u);
    calf_ui_set_wifi_connection(&ui, "CALF-LAB", "192.168.1.67");
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[41 * CALF_UI_WIDTH + 664] == 0xff55e6b5u);
    assert(pixels[20 * CALF_UI_WIDTH + 20] != 0xd0182028u);
    assert(pixels[20 * CALF_UI_WIDTH + 160] != 0xd0182028u);
}

static void test_long_exposure_iso_title_does_not_overlap_status(void)
{
    calf_ui_t ui;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    int x;
    int y;
    calf_ui_init(&ui);
    ui.exposure_index = 10;
    ui.exposure_known = 1;
    ui.iso_index = 8;
    ui.iso_known = 1;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    for(y = 0; y < 72; ++y)
        for(x = 532; x < 540; ++x)
            assert(pixels[y * CALF_UI_WIDTH + x] == 0xd0182028u);
}

static void test_settings_hierarchy_and_image_action(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);

    (void)calf_ui_tap(&ui, 80, 440);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
    (void)calf_ui_tap(&ui, 430, 100);
    assert(ui.screen == CALF_SCREEN_SETTINGS_IMAGE);
    (void)calf_ui_tap(&ui, 600, 100);
    assert(ui.screen == CALF_SCREEN_IMAGE_BRIGHTNESS);

    action = calf_ui_tap(&ui, 360, 220);
    assert(action.kind == CALF_ACTION_SET_BRIGHTNESS);
    assert(strcmp(action.value, "10") == 0);
    assert(ui.image_level_known[0] == 0);
    calf_ui_complete_action(&ui, action, 1, "APPLIED");
    assert(ui.image_level_known[0] == 1);
    assert(ui.image_level_index[0] == 10);
    assert(ui.screen == CALF_SCREEN_SETTINGS_IMAGE);

    (void)calf_ui_tap(&ui, 50, 30);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
    (void)calf_ui_tap(&ui, 50, 30);
    assert(ui.screen == CALF_SCREEN_MAIN);
}

static void test_settings_hub_has_opaque_touch_tiles(void)
{
    calf_ui_t ui;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    ui.screen = CALF_SCREEN_SETTINGS;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[100 * CALF_UI_WIDTH + 200] >> 24) != 0);
    assert((pixels[430 * CALF_UI_WIDTH + 600] >> 24) != 0);
}

static void test_display_uses_full_stock_range(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    (void)calf_ui_tap(&ui, 80, 440);
    (void)calf_ui_tap(&ui, 430, 430);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
    (void)calf_ui_tap(&ui, 100, 100);
    assert(ui.screen == CALF_SCREEN_DISPLAY);

    action = calf_ui_tap(&ui, 500, 400);
    assert(action.kind == CALF_ACTION_SET_BACKLIGHT);
    assert(strcmp(action.value, "251") == 0);
    assert(ui.backlight_known == 0);
    calf_ui_complete_action(&ui, action, 1, "APPLIED");
    assert(ui.backlight_known == 1);
    assert(ui.backlight_index == 25);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
}

static void test_stock_main_key_shortcuts(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);

    action = calf_ui_key_press(&ui, CALF_KEY_UP);
    assert(action.kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ISO);
    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    assert(ui.screen == CALF_SCREEN_MAIN);

    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    assert(ui.screen == CALF_SCREEN_EV);
    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    assert(ui.screen == CALF_SCREEN_WHITE_BALANCE);
    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
    assert(ui.screen == CALF_SCREEN_EXPOSURE);

    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    action = calf_ui_key_press(&ui, CALF_KEY_SHUTTER);
    assert(action.kind == CALF_ACTION_SNAPSHOT);
    assert(calf_ui_key_press(&ui, CALF_KEY_SHUTTER).kind == CALF_ACTION_NONE);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.message[0] == '\0');
}

static void test_key_focus_navigation_and_activation(void)
{
    calf_ui_t ui;
    calf_action_t action;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    (void)calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
    (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
    assert(ui.focus_visible == 1);
    assert(ui.focus_index == 2);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[162 * CALF_UI_WIDTH + 12] == 0xffffd166u);
    (void)calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(ui.screen == CALF_SCREEN_SETTINGS_ENCODING);
    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    assert(ui.screen == CALF_SCREEN_MAIN);

    (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
    (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    assert(ui.focus_index == 1);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_SET_EXPOSURE);
    assert(action.selection == 1);
    assert(strcmp(action.value, "0.25") == 0);
}

static void test_settings_focus_is_visible_on_entry(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);

    (void)calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(ui.screen == CALF_SCREEN_SETTINGS);
    assert(ui.focus_visible == 1);
    assert(ui.focus_index == 0);

    (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_IMAGE);
    assert(ui.focus_visible == 1);
    assert(ui.focus_index == 0);

    ui.white_balance_known = 1;
    ui.white_balance_index = 3;
    (void)calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(ui.screen == CALF_SCREEN_WHITE_BALANCE);
    assert(ui.focus_visible == 1);
    assert(ui.focus_index == 3);
}

static void test_value_menu_focus_starts_at_applied_selection(void)
{
    calf_ui_t ui;
    calf_ui_init(&ui);

    ui.exposure_index = 6;
    ui.exposure_known = 1;
    (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    assert(ui.focus_visible == 1);
    assert(ui.focus_index == 5);

    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    ui.iso_index = 7;
    ui.iso_known = 1;
    (void)calf_ui_key_press(&ui, CALF_KEY_UP);
    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    assert(ui.focus_index == 6);

    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    ui.ev_index = 5;
    ui.ev_known = 1;
    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    assert(ui.focus_index == 4);

    (void)calf_ui_key_press(&ui, CALF_KEY_BACK);
    ui.white_balance_index = 4;
    ui.white_balance_known = 1;
    (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    (void)calf_ui_key_press(&ui, CALF_KEY_LEFT);
    assert(ui.focus_index == 3);
}

static void test_power_key_short_toggle_model(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    action = calf_ui_key_press(&ui, CALF_KEY_POWER);
    assert(action.kind == CALF_ACTION_SET_LCD_POWER);
    assert(strcmp(action.value, "turn_off") == 0);
    calf_ui_complete_action(&ui, action, 1, "LCD UPDATED");
    assert(ui.lcd_on == 0);
    action = calf_ui_key_press(&ui, CALF_KEY_POWER);
    assert(strcmp(action.value, "turn_on") == 0);
    calf_ui_complete_action(&ui, action, 1, "LCD UPDATED");
    assert(ui.lcd_on == 1);
}

static void test_video_mode_uses_shutter_for_recording(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_VIDEO);
    action = calf_ui_key_press(&ui, CALF_KEY_SHUTTER);
    assert(action.kind == CALF_ACTION_RECORD_TOGGLE);
    assert(strcmp(action.value, "start") == 0);
    assert(strcmp(ui.message, "APPLYING") == 0);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.message[0] == '\0');
    action = calf_ui_tap(&ui, 550, 440);
    assert(action.kind == CALF_ACTION_RECORD_TOGGLE);
    assert(strcmp(action.value, "stop") == 0);
    assert(strcmp(ui.message, "SAVING") == 0);
}

static void test_video_exposure_choices_follow_profile_frame_rate(void)
{
    calf_ui_t ui;
    calf_action_t action;
    const choice_t *choices;
    size_t count;
    calf_ui_init(&ui);
    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_VIDEO);

    assert(calf_ui_sync_resolution(
               &ui, CALF_CAPTURE_VIDEO, "VR180_8K") == 0);
    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 11);
    assert(strcmp(choices[0].label, "AUTO") == 0);
    assert(strcmp(choices[1].label, "1/30") == 0);
    assert(strcmp(choices[2].label, "1/50") == 0);

    assert(calf_ui_sync_resolution(
               &ui, CALF_CAPTURE_VIDEO, "VR180_6K") == 0);
    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 10);
    assert(strcmp(choices[0].label, "AUTO") == 0);
    assert(strcmp(choices[1].label, "1/50") == 0);
    assert(strcmp(choices[2].label, "1/60") == 0);
    assert(strcmp(choices[3].label, "1/100") == 0);

    (void)calf_ui_tap(&ui, 300, 40);
    action = calf_ui_tap(&ui, 220, 100);
    assert(action.kind == CALF_ACTION_SET_EXPOSURE);
    assert(strcmp(action.value, "0.02") == 0);
    assert(action.selection == 11);
    calf_ui_complete_action(&ui, action, 1, "APPLIED");
    assert(ui.exposure_index == 11);
    (void)calf_ui_tap(&ui, 300, 40);
    assert(calf_ui_exposure_visible_selection(&ui) == 1);

    assert(calf_ui_sync_resolution(
               &ui, CALF_CAPTURE_VIDEO, "3D_4K") == 0);
    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 9);
    assert(strcmp(choices[0].label, "AUTO") == 0);
    assert(strcmp(choices[1].label, "1/60") == 0);
    assert(strcmp(choices[2].label, "1/100") == 0);
    assert(calf_ui_exposure_visible_selection(&ui) == -1);

    assert(calf_ui_sync_resolution(
               &ui, CALF_CAPTURE_VIDEO, "VR180_5K60") == 0);
    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 9);
    assert(strcmp(choices[1].label, "1/60") == 0);

    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_PHOTO);
    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 11);
    assert(strcmp(choices[0].label, "1/2") == 0);
    assert(strcmp(choices[4].label, "AUTO") == 0);
}

static void test_main_mode_button_opens_capture_mode(void)
{
    calf_ui_t ui;
    calf_ui_init(&ui);
    assert(calf_ui_tap(&ui, 720, 420).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_CAPTURE_MODE);
}

static void test_night_mode_has_manual_long_exposure_controls(void)
{
    calf_ui_t ui;
    calf_action_t action;
    const choice_t *choices;
    size_t count;
    calf_ui_init(&ui);
    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_NIGHT);

    choices = calf_ui_exposure_choices(&ui, &count);
    assert(count == 9);
    assert(strcmp(choices[0].value, "0.0666667") == 0);
    assert(strcmp(choices[6].value, "4") == 0);
    assert(strcmp(choices[7].value, "8") == 0);
    assert(strcmp(choices[8].value, "12") == 0);
    assert(calf_exposure_allowed(CALF_CAPTURE_NIGHT, "4"));
    assert(calf_exposure_allowed(CALF_CAPTURE_NIGHT, "8"));
    assert(calf_exposure_allowed(CALF_CAPTURE_NIGHT, "12"));
    assert(!calf_exposure_allowed(CALF_CAPTURE_NIGHT, "-1"));

    choices = calf_ui_iso_choices(&ui, &count);
    assert(count == 4);
    assert(strcmp(choices[0].value, "iso100") == 0);
    assert(strcmp(choices[3].value, "iso800") == 0);
    assert(calf_iso_allowed(CALF_CAPTURE_NIGHT, "iso800"));
    assert(!calf_iso_allowed(CALF_CAPTURE_NIGHT, "auto"));

    (void)calf_ui_key_press(&ui, CALF_KEY_DOWN);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_SET_EXPOSURE);
}

static void test_main_zoom_toggles_operator_left_and_stereo_directly(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status = {1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    ui.lens_known = 1;
    ui.lens_index = 1;
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_SET_CAMERA_MODE);
    assert(strcmp(action.value, "SENSOR0_4K") == 0);
    assert(action.selection == 0);
    calf_ui_complete_action(&ui, action, 1, "MODE UPDATED");
    assert(ui.lens_index == 0);
    action = calf_ui_tap(&ui, 200, 440);
    assert(action.kind == CALF_ACTION_SET_CAMERA_MODE);
    assert(strcmp(action.value, "PRIMARY") == 0);
    assert(action.selection == 1);
}

static void test_capture_mode_switch_is_transactional_and_interlocked(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status = {1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    (void)calf_ui_tap(&ui, 720, 420);
    action = calf_ui_tap(&ui, 400, 220);
    assert(action.kind == CALF_ACTION_SET_CAPTURE_MODE);
    assert(strcmp(action.value, "night") == 0);
    assert(ui.capture_mode == CALF_CAPTURE_PHOTO);
    calf_ui_complete_action(&ui, action, 0, "FAILED");
    assert(ui.capture_mode == CALF_CAPTURE_PHOTO);
    action = calf_ui_tap(&ui, 400, 220);
    calf_ui_complete_action(&ui, action, 1, "MODE UPDATED");
    assert(ui.capture_mode == CALF_CAPTURE_NIGHT);
    assert(ui.screen == CALF_SCREEN_MAIN);

    (void)calf_ui_tap(&ui, 720, 420);
    action = calf_ui_tap(&ui, 600, 220);
    calf_ui_complete_action(&ui, action, 1, "MODE UPDATED");
    assert(ui.capture_mode == CALF_CAPTURE_VIDEO);
    assert(ui.screen == CALF_SCREEN_MAIN);

    (void)calf_ui_tap(&ui, 720, 420);
    status.recording = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 180, 220);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP RECORDING") != NULL);
}

static void test_gallery_navigation_and_confirmed_delete(void)
{
    calf_ui_t ui;
    calf_action_t action;
    static const uint32_t preview[8] = {
        0xffff0000u, 0xffff0000u, 0xff00ff00u, 0xff00ff00u,
        0xffff0000u, 0xffff0000u, 0xff00ff00u, 0xff00ff00u,
    };
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    ui.status.online = 1;
    ui.status.streaming = 0;
    ui.status.playback = 0;
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_GALLERY_ENTER);
    calf_ui_set_gallery(&ui, "V1234567.MP4", 1, 0, 3, 0);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.screen == CALF_SCREEN_GALLERY);
    assert(ui.gallery_has_item == 1 && ui.gallery_is_video == 1);
    {
        unsigned revision = ui.revision;
        calf_ui_set_gallery_index(&ui, 2);
        assert(ui.gallery_index == 2 && ui.revision == revision + 1);
        calf_ui_set_gallery_index(&ui, 3);
        assert(ui.gallery_index == 2 && ui.revision == revision + 1);
        calf_ui_set_gallery_index(&ui, 0);
    }
    calf_ui_set_gallery_playback(&ui, 0, 12, 125, 1);
    assert(ui.gallery_position_seconds == 12);
    assert(ui.gallery_duration_seconds == 125);
    assert(ui.gallery_timing_known == 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_UP).kind == CALF_ACTION_NONE);
    assert(ui.gallery_histogram_visible == 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_DOWN).kind == CALF_ACTION_NONE);
    assert(ui.gallery_zoom_right == 1);
    {
        uint32_t bins[CALF_HISTOGRAM_BIN_COUNT];
        int index;
        for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index)
            bins[index] = (uint32_t)(index + 1);
        calf_ui_set_gallery_histogram(&ui, bins, 1);
        assert(ui.gallery_histogram_valid == 1);
    }
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.gallery_playing == 1);
    action = calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    assert(action.kind == CALF_ACTION_GALLERY_NEXT);
    calf_ui_set_gallery(&ui, "V1234566.JPG", 0, 1, 3, 0);
    calf_ui_complete_action(&ui, action, 1, "");

    assert(calf_ui_key_press(&ui, CALF_KEY_FILE).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_DELETE_CONFIRM);
    assert(calf_ui_key_press(&ui, CALF_KEY_MENU).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_GALLERY);
    (void)calf_ui_key_press(&ui, CALF_KEY_FILE);
    (void)calf_ui_key_press(&ui, CALF_KEY_RIGHT);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_GALLERY_DELETE);
    calf_ui_set_gallery(&ui, "V1234565.JPG", 0, 1, 2, 0);
    calf_ui_complete_action(&ui, action, 1, "DELETED");
    assert(ui.screen == CALF_SCREEN_GALLERY && ui.gallery_count == 2);

    calf_ui_notice(&ui, "", 0);
    ui.gallery_histogram_visible = 0;
    calf_ui_set_gallery_preview(&ui, preview, 4, 2);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert(pixels[220 * CALF_UI_WIDTH + 400] == 0xff00ff00u);
    assert((pixels[420 * CALF_UI_WIDTH + 400] >> 24) != 0);
    action = calf_ui_key_press(&ui, CALF_KEY_BACK);
    assert(action.kind == CALF_ACTION_GALLERY_EXIT);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.screen == CALF_SCREEN_MAIN);
}

static void test_gallery_enter_reports_blocking_camera_state(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 1, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    calf_action_t action;

    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP RECORDING") != NULL);

    status.recording = 0;
    status.streaming = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP LIVE") != NULL);

    status.streaming = 0;
    status.playback = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP PLAYBACK") != NULL);

    status.playback = 0;
    status.online = 0;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STATUS UNKNOWN") != NULL);

    status.online = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_key_press(&ui, CALF_KEY_FILE);
    assert(action.kind == CALF_ACTION_GALLERY_ENTER);
}

static void test_recording_duration_updates_status_revision(void)
{
    calf_ui_t ui;
    calf_backend_status_t status = {1, 1, 80, 1000, 50, 55, 0, 0, 0, 1, ""};
    uint32_t revision;
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    revision = ui.revision;
    status.recording_seconds = 2;
    calf_ui_set_status(&ui, &status);
    assert(ui.status.recording_seconds == 2);
    assert(ui.revision > revision);
}

static void test_capture_requires_primary_stereo_graph(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    action.kind = CALF_ACTION_SNAPSHOT;
    action.value = NULL;
    action.selection = -1;
    assert(calf_ui_action_requires_primary(&ui, action) == 1);
    ui.lens_known = 1;
    ui.lens_index = 0;
    assert(calf_ui_action_requires_primary(&ui, action) == 1);
    ui.lens_index = 1;
    assert(calf_ui_action_requires_primary(&ui, action) == 0);

    action.kind = CALF_ACTION_RECORD_TOGGLE;
    ui.lens_index = 2;
    ui.status.recording = 0;
    assert(calf_ui_action_requires_primary(&ui, action) == 1);
    ui.status.recording = 1;
    assert(calf_ui_action_requires_primary(&ui, action) == 0);
}

static void test_backend_image_state_syncs_all_supported_controls(void)
{
    calf_ui_t ui;
    uint32_t revision;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    int x;
    int y;
    int detail_pixels = 0;
    calf_ui_init(&ui);
    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_VIDEO);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_EXPOSURE, "0.001") == 0);
    assert(ui.exposure_known == 1 && ui.exposure_index == 9);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_ISO, "iso12800") == 0);
    assert(ui.iso_known == 1 && ui.iso_index == 8);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_WHITE_BALANCE, "shadow") == 0);
    assert(ui.white_balance_known == 1 && ui.white_balance_index == 3);
    assert(calf_ui_sync_image_value(&ui, CALF_ACTION_SET_EV, "2") == 0);
    assert(ui.ev_known == 1 && ui.ev_index == 5);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_BRIGHTNESS, "20") == 0);
    assert(ui.image_level_known[0] == 1 && ui.image_level_index[0] == 20);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_SATURATION, "8") == 0);
    assert(ui.image_level_known[2] == 1 && ui.image_level_index[2] == 8);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_ANTIFLICKER, "60hz") == 0);
    assert(ui.antiflicker_known == 1 && ui.antiflicker_index == 3);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_EFFECT, "blackwhite") == 0);
    assert(ui.effect_known == 1 && ui.effect_index == 1);
    revision = ui.revision;
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_EFFECT, "blackwhite") == 0);
    assert(ui.revision == revision);
    assert(calf_ui_sync_image_value(
               &ui, CALF_ACTION_SET_ISO, "invalid") == -1);
    assert(ui.iso_index == 8);

    ui.screen = CALF_SCREEN_IMAGE_SATURATION;
    ui.focus_visible = 0;
    assert(calf_ui_key_press(&ui, CALF_KEY_LEFT).kind == CALF_ACTION_NONE);
    assert(ui.focus_visible == 1 && ui.focus_index == 7);

    ui.screen = CALF_SCREEN_SETTINGS_IMAGE;
    ui.focus_visible = 0;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    for(y = 266; y < 308; ++y)
        for(x = 280; x < 518; ++x)
            if(pixels[y * CALF_UI_WIDTH + x] == 0xff55e6b5u)
                ++detail_pixels;
    assert(detail_pixels > 0);
}

static void test_audio_input_settings_use_backend_readback_and_large_grids(void)
{
    calf_ui_t ui;
    calf_action_t action;
    uint32_t revision;
    calf_ui_init(&ui);
    calf_ui_set_capture_mode(&ui, CALF_CAPTURE_VIDEO);

    assert(calf_ui_sync_audio_input(&ui, 0, 2) == 0);
    assert(ui.audio_input_known == 1 && ui.audio_input_index == 3);
    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_BUILTIN_MIC_VOLUME, 80) == 0);
    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_LINEIN_VOLUME, 50) == 0);
    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_USB_MIC_VOLUME, 100) == 0);
    assert(ui.audio_input_volume_index[0] == 8);
    assert(ui.audio_input_volume_index[1] == 5);
    assert(ui.audio_input_volume_index[2] == 10);
    revision = ui.revision;
    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_USB_MIC_VOLUME, 100) == 0);
    assert(ui.revision == revision);
    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_USB_MIC_VOLUME, 83) == -1);
    assert(ui.audio_input_volume_known[2] == 0);

    ui.screen = CALF_SCREEN_SETTINGS;
    assert(calf_ui_tap(&ui, 100, 350).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_AUDIO);
    assert(calf_ui_tap(&ui, 100, 130).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_AUDIO_INPUT);
    action = calf_ui_tap(&ui, 600, 180);
    assert(action.kind == CALF_ACTION_SET_AUDIO_INPUT);
    assert(strcmp(action.value, "builtin_mic") == 0);
    calf_ui_complete_action(&ui, action, 1, "APPLIED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_AUDIO);
    assert(ui.audio_input_index == 1);

    assert(calf_ui_tap(&ui, 100, 260).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_AUDIO_LINEIN_VOLUME);
    action = calf_ui_tap(&ui, 100, 360);
    assert(action.kind == CALF_ACTION_SET_LINEIN_VOLUME);
    assert(strcmp(action.value, "80") == 0);
    calf_ui_complete_action(&ui, action, 0, "BACKEND ERROR");
    assert(ui.audio_input_volume_index[1] == 5);
}

static void test_audio_key_focus_starts_at_current_values(void)
{
    calf_ui_t ui;
    calf_ui_init(&ui);
    assert(calf_ui_sync_audio_input(&ui, 0, 2) == 0);
    ui.screen = CALF_SCREEN_AUDIO_INPUT;
    assert(calf_ui_key_press(&ui, CALF_KEY_LEFT).kind == CALF_ACTION_NONE);
    assert(ui.focus_visible == 1 && ui.focus_index == 2);

    assert(calf_ui_sync_audio_volume(
               &ui, CALF_ACTION_SET_BUILTIN_MIC_VOLUME, 80) == 0);
    ui.screen = CALF_SCREEN_AUDIO_BUILTIN_VOLUME;
    ui.focus_visible = 0;
    assert(calf_ui_key_press(&ui, CALF_KEY_LEFT).kind == CALF_ACTION_NONE);
    assert(ui.focus_visible == 1 && ui.focus_index == 7);
}

static void test_speaker_volume_audio_screen_and_gallery_keys(void)
{
    calf_ui_t ui;
    calf_action_t action;
    uint32_t revision;
    calf_ui_init(&ui);
    assert(calf_ui_sync_speaker_volume(&ui, 50) == 0);
    assert(ui.speaker_volume_known == 1 && ui.speaker_volume_index == 5);
    revision = ui.revision;
    assert(calf_ui_sync_speaker_volume(&ui, 50) == 0);
    assert(ui.revision == revision);

    ui.screen = CALF_SCREEN_SETTINGS_AUDIO;
    assert(calf_ui_tap(&ui, 100, 350).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_AUDIO_SPEAKER_VOLUME);
    action = calf_ui_tap(&ui, 500, 300);
    assert(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME);
    assert(action.selection == 10 && strcmp(action.value, "100") == 0);
    calf_ui_complete_action(&ui, action, 1, "VOLUME UPDATED");
    assert(ui.speaker_volume_index == 10);
    assert(ui.screen == CALF_SCREEN_SETTINGS_AUDIO);

    calf_ui_set_gallery(&ui, "V1234567.MP4", 1, 0, 1, 1);
    ui.screen = CALF_SCREEN_GALLERY;
    action = calf_ui_key_press(&ui, CALF_KEY_UP);
    assert(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME);
    assert(action.selection == 11 && strcmp(action.value, "110") == 0);
    assert(ui.pending_action == CALF_ACTION_SET_SPEAKER_VOLUME);
    assert(ui.message[0] == '\0');
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.screen == CALF_SCREEN_GALLERY);
    assert(ui.speaker_volume_index == 11);
    assert(ui.message[0] == '\0');
    revision = ui.revision;
    calf_ui_set_gallery_volume_visible(&ui, 1);
    assert(ui.gallery_volume_visible == 1 && ui.revision == revision + 1);
    revision = ui.revision;
    calf_ui_set_gallery_volume_visible(&ui, 1);
    assert(ui.revision == revision);
    calf_ui_set_gallery_volume_visible(&ui, 0);
    assert(ui.gallery_volume_visible == 0 && ui.revision == revision + 1);
    action = calf_ui_key_press(&ui, CALF_KEY_DOWN);
    assert(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME);
    assert(action.selection == 10 && strcmp(action.value, "100") == 0);
    calf_ui_complete_action(&ui, action, 0, "BACKEND ERROR");
    assert(ui.screen == CALF_SCREEN_GALLERY);
    assert(ui.speaker_volume_index == 11);

    calf_ui_set_gallery_playback(&ui, 0, 0, 1, 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_UP).kind == CALF_ACTION_NONE);
    assert(ui.gallery_histogram_visible == 1);
    assert(calf_ui_sync_speaker_volume(&ui, 55) == -1);
    assert(ui.speaker_volume_known == 0);
}

static void test_display_off_choices_and_setting(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    assert(calf_display_off_count() == 8);
    assert(strcmp(calf_display_off_label(0), "ALWAYS ON") == 0);
    assert(calf_display_off_seconds(0) == -1);
    assert(calf_display_off_seconds(7) == 1800);
    assert(calf_display_off_index_from_seconds(300) == 4);
    assert(calf_display_off_index_from_seconds(7) == -1);
    assert(ui.display_off_known == 1);
    assert(ui.display_off_seconds == -1);

    (void)calf_ui_tap(&ui, 80, 440);
    (void)calf_ui_tap(&ui, 430, 430);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
    (void)calf_ui_tap(&ui, 600, 100);
    assert(ui.screen == CALF_SCREEN_DISPLAY_OFF);
    action = calf_ui_tap(&ui, 600, 100);
    assert(action.kind == CALF_ACTION_SET_DISPLAY_OFF);
    assert(action.selection == 1);
    assert(strcmp(action.value, "10") == 0);
    calf_ui_complete_action(&ui, action, 1, "DISPLAY TIMER UPDATED");
    assert(ui.display_off_index == 1);
    assert(ui.display_off_seconds == 10);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);

    calf_ui_set_display_off(&ui, 6);
    assert(ui.display_off_index == 6);
    assert(ui.display_off_seconds == 1200);
}

static void test_bq25703_power_decode_tracks_charge_direction(void)
{
    calf_power_sample_t sample;
    assert(calf_power_decode_bq25703(
               0x1900u, 0x0400u, 0x1400u, 0x5656u,
               0x0210u, 0xa0ffu, 1, &sample) == 0);
    assert(sample.valid == 1);
    assert(sample.recording == 1);
    assert(sample.usb_mv == 4800);
    assert(sample.usb_ma == 1000);
    assert(sample.usb_mw == 4800);
    assert(sample.battery_mv == 8384);
    assert(sample.battery_ma == -256);
    assert(sample.battery_mw == -2146);
    assert(sample.device_mw == 2654);

    assert(calf_power_decode_bq25703(
               0x0000u, 0x0003u, 0x0000u, 0x5656u,
               0x0210u, 0xa0ffu, 0, &sample) == 0);
    assert(sample.usb_mv == 0 && sample.usb_mw == 0);
    assert(sample.battery_ma == 768);
    assert(sample.battery_mw == 6439);
    assert(sample.device_mw == 6439);

    assert(calf_power_decode_bq25703(
               0x1900u, 0x0400u, 0x1400u, 0x5656u,
               0x0c00u, 0xa0ffu, 0, &sample) == 0);
    assert(sample.usb_ma == 500);
    assert(sample.battery_ma == -128);
    assert(calf_power_decode_bq25703(
               0x1900u, 0x0400u, 0x1400u, 0x5656u,
               0x0210u, 0xa000u, 0, &sample) == -1);
    assert(sample.valid == 0);
}

static void test_power_regmap_parser_requires_complete_snapshot(void)
{
    static const char registers[] =
        "26: 1900\n28: 0400\n2a: 1400\n2c: 5656\n"
        "30: 0210\n3a: a0ff\n";
    calf_power_sample_t sample;
    assert(target_power_parse_registers(registers, 1, &sample) == 0);
    assert(sample.valid == 1);
    assert(sample.recording == 1);
    assert(sample.usb_mw == 4800);
    assert(sample.battery_mw == -2146);
    assert(target_power_parse_registers("26: 1900\n", 0, &sample) == -1);
    assert(sample.valid == 0);
}

static void test_power_samples_average_valid_readings_and_recording(void)
{
    calf_power_sample_t samples[5] = {0};
    calf_power_sample_t average;
    int index;
    for(index = 0; index < 5; ++index) {
        samples[index].usb_mv = 4800;
        samples[index].usb_ma = 1000 + index * 50;
        samples[index].usb_mw = 4800 + index * 240;
        samples[index].battery_mv = 8000;
        samples[index].battery_ma = index < 2 ? -64 : index * 256;
        samples[index].battery_mw = index < 2 ? -512 : index * 2048;
        samples[index].device_mw = samples[index].usb_mw +
                                   samples[index].battery_mw;
        samples[index].valid = 1;
    }
    samples[2].recording = 1;
    samples[4].valid = 0;
    assert(calf_power_average_samples(samples, 5, &average) == 0);
    assert(average.valid == 1);
    assert(average.recording == 1);
    assert(average.usb_mv == 4800);
    assert(average.usb_ma == 1075);
    assert(average.battery_ma == 288);
    assert(average.battery_mw == 2304);
    assert(average.device_mw == 7464);

    for(index = 0; index < 5; ++index) samples[index].valid = 0;
    assert(calf_power_average_samples(samples, 5, &average) == -1);
    assert(average.valid == 0);
    assert(average.recording == 1);
}

static void test_power_history_navigation_ring_and_render(void)
{
    calf_ui_t ui;
    calf_power_sample_t sample;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    int index;
    int usb_pixels = 0;
    int battery_pixels = 0;
    int temperature_pixels = 0;
    calf_ui_init(&ui);
    ui.screen = CALF_SCREEN_SETTINGS_GENERAL;
    assert(calf_ui_tap(&ui, 100, 380).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_POWER_HISTORY);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);

    ui.screen = CALF_SCREEN_POWER_HISTORY;
    for(index = 0; index < CALF_POWER_HISTORY_COUNT + 5; ++index) {
        sample.usb_mv = 4800;
        sample.usb_ma = 1000;
        sample.usb_mw = index;
        sample.battery_mv = 8384;
        sample.battery_ma = -256;
        sample.battery_mw = -2146;
        sample.device_mw = sample.usb_mw + sample.battery_mw;
        sample.recording = index >= 100 && index < 140;
        sample.valid = 1;
        calf_ui_add_power_sample(&ui, &sample);
    }
    assert(ui.power_history_count == CALF_POWER_HISTORY_COUNT);
    assert(ui.power_history_next == 5);
    assert(ui.power_history[ui.power_history_next].usb_mw == 5);
    assert(ui.power.usb_mw == CALF_POWER_HISTORY_COUNT + 4);
    ui.status.battery_percent = 92;
    ui.status.system_temp = 56;
    ui.status.core_temp = 59;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[100 * CALF_UI_WIDTH + 200] >> 24) != 0);
    assert((pixels[300 * CALF_UI_WIDTH + 400] >> 24) != 0);
    for(index = 0; index < CALF_UI_WIDTH * CALF_UI_HEIGHT; ++index) {
        if(pixels[index] == 0xff52c7ffu) ++usb_pixels;
        if(pixels[index] == 0xffffc14du) ++battery_pixels;
        if(pixels[index] == 0xffff7b72u) ++temperature_pixels;
    }
    assert(usb_pixels > 20);
    assert(battery_pixels > 20);
    assert(temperature_pixels > 20);
}

static void test_datetime_settings_are_touch_and_key_friendly(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    assert(calf_ui_sync_timezone(&ui, "UTC") == 0);
    calf_ui_sync_auto_time(&ui, 0);
    calf_ui_sync_datetime(&ui, 2026, 8, 6, 14, 5, 9);

    ui.screen = CALF_SCREEN_SETTINGS;
    assert(calf_ui_tap(&ui, 100, 430).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_DATETIME);
    assert(calf_ui_tap(&ui, 100, 150).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_TIMEZONE);
    action = calf_ui_tap(&ui, 520, 250);
    assert(action.kind == CALF_ACTION_SET_TIMEZONE);
    assert(action.selection == 13);
    assert(strcmp(action.value, "EST-1") == 0);
    calf_ui_complete_action(&ui, action, 1, "TIME ZONE UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_DATETIME);
    assert(ui.timezone_index == 13);

    assert(calf_ui_tap(&ui, 600, 150).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_AUTO_TIME);
    action = calf_ui_tap(&ui, 600, 200);
    assert(action.kind == CALF_ACTION_SET_AUTO_TIME);
    assert(strcmp(action.value, "1") == 0);
    calf_ui_complete_action(&ui, action, 1, "AUTO TIME UPDATED");
    assert(ui.auto_time_index == 1);
    assert(calf_ui_tap(&ui, 100, 300).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_DATETIME);
    assert(strstr(ui.message, "TURN AUTO SET OFF") != NULL);

    calf_ui_sync_auto_time(&ui, 0);
    assert(calf_ui_tap(&ui, 100, 300).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ADJUST_DATETIME);
    assert(calf_ui_tap(&ui, 550, 90).kind == CALF_ACTION_NONE);
    assert(ui.datetime_year == 2027);
    assert(calf_ui_key_press(&ui, CALF_KEY_DOWN).kind == CALF_ACTION_NONE);
    assert(calf_ui_key_press(&ui, CALF_KEY_RIGHT).kind == CALF_ACTION_NONE);
    assert(ui.datetime_month == 9);
    action = calf_ui_tap(&ui, 400, 430);
    assert(action.kind == CALF_ACTION_SET_DATETIME);
    assert(strcmp(action.value, "2027-09-06T14:05:09") == 0);
    calf_ui_complete_action(&ui, action, 1, "CLOCK UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_DATETIME);
}

static void test_wifi_scan_connect_and_password_entry(void)
{
    static const calf_wifi_network_t networks[] = {
        {"CALF-LAB", 4, -42},
        {"Studio Guest", 3, -57},
    };
    calf_ui_t ui;
    calf_action_t action;
    int index;
    calf_ui_init(&ui);
    ui.screen = CALF_SCREEN_SETTINGS_NETWORK;

    action = calf_ui_tap(&ui, 100, 150);
    assert(action.kind == CALF_ACTION_WIFI_SCAN);
    calf_ui_set_wifi_networks(&ui, networks, 2, "CALF-LAB",
                              "192.168.1.67");
    calf_ui_complete_action(&ui, action, 1, "NETWORKS UPDATED");
    assert(ui.screen == CALF_SCREEN_WIFI_LIST);
    assert(ui.wifi_network_count == 2);
    assert(strcmp(ui.wifi_current_ssid, "CALF-LAB") == 0);

    action = calf_ui_tap(&ui, 200, 210);
    assert(action.kind == CALF_ACTION_WIFI_CONNECT_SAVED);
    assert(action.selection == 1);
    assert(strcmp(action.value, "Studio Guest") == 0);
    calf_ui_wifi_require_password(&ui, action.selection);
    assert(ui.screen == CALF_SCREEN_WIFI_PASSWORD);
    assert(ui.message[0] == '\0');

    assert(calf_ui_tap(&ui, 40, 235).kind == CALF_ACTION_NONE);
    assert(strcmp(ui.wifi_password, "q") == 0);
    assert(calf_ui_tap(&ui, 450, 430).kind == CALF_ACTION_NONE);
    assert(ui.wifi_password[0] == '\0');
    for(index = 0; index < 8; ++index)
        assert(calf_ui_tap(&ui, 40, 180).kind == CALF_ACTION_NONE);
    assert(strcmp(ui.wifi_password, "11111111") == 0);
    action = calf_ui_tap(&ui, 650, 430);
    assert(action.kind == CALF_ACTION_WIFI_CONNECT_PASSWORD);
    assert(strcmp(action.value, "Studio Guest") == 0);
    calf_ui_complete_action(&ui, action, 0, "WI-FI CONNECTION FAILED");
    assert(ui.screen == CALF_SCREEN_WIFI_PASSWORD);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_WIFI_LIST);
    assert(ui.wifi_password[0] == '\0');

    assert(calf_ui_key_press(&ui, CALF_KEY_DOWN).kind == CALF_ACTION_NONE);
    assert(ui.focus_index == 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_DOWN).kind == CALF_ACTION_NONE);
    assert(ui.focus_index == 2);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_WIFI_SCAN);
}

static void test_firmware_update_requires_check_and_confirmation(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    ui.screen = CALF_SCREEN_SETTINGS_GENERAL;

    action = calf_ui_tap(&ui, 100, 280);
    assert(action.kind == CALF_ACTION_FIRMWARE_CHECK);
    calf_ui_set_update_ready(&ui, 257);
    calf_ui_complete_action(&ui, action, 1, "");
    assert(ui.screen == CALF_SCREEN_UPDATE_CONFIRM);
    assert(ui.update_ready == 1);
    assert(ui.update_size_mb == 257);
    assert(calf_ui_key_press(&ui, CALF_KEY_RIGHT).kind == CALF_ACTION_NONE);
    assert(calf_ui_key_press(&ui, CALF_KEY_SHUTTER).kind == CALF_ACTION_NONE);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_FIRMWARE_INSTALL);

    calf_ui_complete_action(&ui, action, 0, "UPDATE FILE CHANGED; CHECK AGAIN");
    assert(ui.screen == CALF_SCREEN_UPDATE_CONFIRM);
    assert(ui.message_is_error == 1);
    assert(calf_ui_key_press(&ui, CALF_KEY_LEFT).kind == CALF_ACTION_NONE);
    assert(calf_ui_key_press(&ui, CALF_KEY_MENU).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);

    action = calf_ui_tap(&ui, 100, 280);
    calf_ui_complete_action(&ui, action, 0, "VALID vpupdate.bin NOT FOUND");
    assert(ui.update_ready == 0);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
}

static void test_wifi_power_toggle_is_confirmed_and_recoverable(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_ui_init(&ui);
    calf_ui_set_wifi_enabled(&ui, 1);
    ui.screen = CALF_SCREEN_SETTINGS_NETWORK;

    assert(calf_ui_tap(&ui, 500, 160).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_WIFI_OFF_CONFIRM);
    assert(ui.focus_visible == 1 && ui.focus_index == 0);
    assert(calf_ui_key_press(&ui, CALF_KEY_SHUTTER).kind == CALF_ACTION_NONE);
    assert(calf_ui_key_press(&ui, CALF_KEY_RIGHT).kind == CALF_ACTION_NONE);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_SET_WIFI_ENABLED);
    assert(action.selection == 0);
    assert(strcmp(action.value, "0") == 0);
    calf_ui_complete_action(&ui, action, 1, "WI-FI POWER UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_NETWORK);
    assert(ui.wifi_enabled_known == 1 && ui.wifi_enabled == 0);

    assert(calf_ui_tap(&ui, 100, 160).kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "TURN WI-FI ON") != NULL);
    action = calf_ui_tap(&ui, 500, 160);
    assert(action.kind == CALF_ACTION_SET_WIFI_ENABLED);
    assert(action.selection == 1);
    assert(strcmp(action.value, "1") == 0);
    calf_ui_complete_action(&ui, action, 1, "WI-FI POWER UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_NETWORK);
    assert(ui.wifi_enabled == 1);
}

static void test_usb_ethernet_modes_are_selectable(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status;
    calf_ui_init(&ui);
    ui.screen = CALF_SCREEN_SETTINGS_NETWORK;

    status = ui.status;
    strcpy(status.ethernet_ip_address, "192.168.50.24");
    calf_ui_set_status(&ui, &status);
    assert(calf_ui_tap(&ui, 100, 330).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ETHERNET);
    assert(strcmp(ui.status.ethernet_ip_address, "192.168.50.24") == 0);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_NETWORK);

    assert(calf_ui_tap(&ui, 600, 330).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_USB_ETHERNET);
    action = calf_ui_tap(&ui, 600, 215);
    assert(action.kind == CALF_ACTION_SET_USB_ETHERNET);
    assert(action.selection == 3);
    assert(strcmp(action.value, "win:USB2") == 0);
    calf_ui_complete_action(&ui, action, 1,
                            "USB NETWORK  192.168.2.101");
    assert(ui.screen == CALF_SCREEN_SETTINGS_NETWORK);
    assert(ui.usb_ethernet_known == 1 && ui.usb_ethernet_index == 3);

    assert(calf_ui_tap(&ui, 600, 330).kind == CALF_ACTION_NONE);
    action = calf_ui_tap(&ui, 100, 120);
    assert(action.kind == CALF_ACTION_SET_USB_ETHERNET);
    assert(action.selection == 0);
    assert(strcmp(action.value, "off") == 0);
    calf_ui_complete_action(&ui, action, 1, "USB NETWORK OFF");
    assert(ui.usb_ethernet_index == 0);
}

static void test_stock_ui_switch_is_confirmed_and_interlocked(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status = {1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);
    ui.screen = CALF_SCREEN_SETTINGS_GENERAL;

    assert(calf_ui_tap(&ui, 600, 400).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_STOCK_UI_CONFIRM);
    assert(ui.focus_visible == 1 && ui.focus_index == 0);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[200 * CALF_UI_WIDTH + 400] >> 24) != 0);
    assert(calf_ui_key_press(&ui, CALF_KEY_RIGHT).kind == CALF_ACTION_NONE);
    action = calf_ui_key_press(&ui, CALF_KEY_MENU);
    assert(action.kind == CALF_ACTION_LOAD_STOCK_UI);
    calf_ui_complete_action(&ui, action, 1, "LOADING STOCK UI");
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);

    status.recording = 1;
    calf_ui_set_status(&ui, &status);
    assert(calf_ui_tap(&ui, 600, 400).kind == CALF_ACTION_NONE);
    action = calf_ui_tap(&ui, 520, 350);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP RECORDING") != NULL);
    assert(calf_ui_key_press(&ui, CALF_KEY_BACK).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
}

static void test_resolution_and_encoder_settings_are_functional(void)
{
    calf_ui_t ui;
    calf_action_t action;
    calf_backend_status_t status = {1, 0, 80, 1000, 50, 55, 0, 0, 0, 0, ""};
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    calf_ui_set_status(&ui, &status);

    assert(calf_ui_sync_resolution(
               &ui, CALF_CAPTURE_PHOTO, "VR180_PIC") == 0);
    ui.screen = CALF_SCREEN_SETTINGS_CAMERA;
    assert(calf_ui_tap(&ui, 600, 160).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_CAMERA_RESOLUTION);
    action = calf_ui_tap(&ui, 600, 200);
    assert(action.kind == CALF_ACTION_SET_RESOLUTION);
    assert(strcmp(action.value, "3D_4K") == 0);
    calf_ui_complete_action(&ui, action, 1, "RESOLUTION UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_CAMERA);
    assert(ui.resolution_index == 1 && ui.resolution_known == 1);

    calf_ui_sync_photo_format(&ui, 0);
    assert(calf_ui_tap(&ui, 100, 300).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_PHOTO_FORMAT);
    action = calf_ui_tap(&ui, 600, 200);
    assert(action.kind == CALF_ACTION_SET_PHOTO_FORMAT);
    assert(action.selection == 1 && strcmp(action.value, "1") == 0);
    calf_ui_complete_action(&ui, action, 1, "PHOTO FORMAT UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_CAMERA);
    assert(ui.photo_format_known == 1 && ui.photo_format_index == 1);

    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_ENCODING_CODEC, "H265") == 0);
    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_IMAGE_QUALITY, "medium") == 0);
    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_ENCODING_COLOR_RANGE, "1") == 0);
    ui.screen = CALF_SCREEN_SETTINGS_ENCODING;
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[150 * CALF_UI_WIDTH + 200] >> 24) != 0);
    assert(calf_ui_tap(&ui, 100, 150).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ENCODING_CODEC);
    action = calf_ui_tap(&ui, 100, 200);
    assert(action.kind == CALF_ACTION_SET_ENCODING_CODEC);
    assert(strcmp(action.value, "H264") == 0);
    calf_ui_complete_action(&ui, action, 1, "ENCODER UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_ENCODING);
    assert(ui.encoding_codec_index == 0);

    assert(calf_ui_tap(&ui, 600, 150).kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_ENCODING_IMAGE_QUALITY);
    action = calf_ui_tap(&ui, 100, 150);
    assert(action.kind == CALF_ACTION_SET_IMAGE_QUALITY);
    assert(strcmp(action.value, "higher") == 0);
    calf_ui_complete_action(&ui, action, 1, "ENCODER UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_ENCODING);
    assert(ui.image_quality_index == 0);

    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_RECORDING_CODEC, "H264_MAIN") == 0);
    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_RECORDING_BITRATE, "30000") == 0);
    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_RECORDING_GOP, "20") == 0);
    assert(calf_ui_sync_encoder_value(
               &ui, CALF_ACTION_SET_RECORDING_COLOR_RANGE, "0") == 0);
    ui.screen = CALF_SCREEN_RECORDING_BITRATE;
    action = calf_ui_tap(&ui, 600, 430);
    assert(action.kind == CALF_ACTION_SET_RECORDING_BITRATE);
    assert(strcmp(action.value, "100000") == 0);
    calf_ui_complete_action(&ui, action, 0, "BACKEND ERROR");
    assert(ui.recording_bitrate_index == 2);

    status.recording = 1;
    calf_ui_set_status(&ui, &status);
    action = calf_ui_tap(&ui, 600, 430);
    assert(action.kind == CALF_ACTION_NONE);
    assert(strstr(ui.message, "STOP RECORDING") != NULL);
}

static void test_language_menu_and_localization_boundary(void)
{
    calf_ui_t ui;
    calf_action_t action;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    assert(calf_language_count() == 1);
    assert(strcmp(calf_language_label(0), "ENGLISH") == 0);
    assert(strcmp(calf_language_value(0), "en") == 0);
    assert(calf_language_index_from_value("en") == 0);
    assert(calf_language_index_from_value("sv") == -1);
    assert(strcmp(calf_ui_translate(CALF_LANGUAGE_ENGLISH, "SETTINGS"),
                  "SETTINGS") == 0);
    assert(ui.language_known == 1);
    assert(ui.language_index == CALF_LANGUAGE_ENGLISH);

    ui.screen = CALF_SCREEN_SETTINGS_GENERAL;
    action = calf_ui_tap(&ui, 100, 200);
    assert(action.kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_LANGUAGE);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[200 * CALF_UI_WIDTH + 400] >> 24) != 0);

    action = calf_ui_tap(&ui, 400, 200);
    assert(action.kind == CALF_ACTION_SET_LANGUAGE);
    assert(action.selection == CALF_LANGUAGE_ENGLISH);
    assert(strcmp(action.value, "en") == 0);
    calf_ui_complete_action(&ui, action, 1, "LANGUAGE UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
    assert(ui.language_index == CALF_LANGUAGE_ENGLISH);
}

static void test_indicator_led_setting(void)
{
    calf_ui_t ui;
    calf_action_t action;
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    calf_ui_init(&ui);
    assert(ui.indicator_led_known == 1);
    assert(ui.indicator_led_index == 0);

    ui.screen = CALF_SCREEN_SETTINGS_GENERAL;
    action = calf_ui_tap(&ui, 600, 200);
    assert(action.kind == CALF_ACTION_NONE);
    assert(ui.screen == CALF_SCREEN_INDICATOR_LED);
    calf_ui_render(&ui, pixels, CALF_UI_WIDTH);
    assert((pixels[200 * CALF_UI_WIDTH + 600] >> 24) != 0);

    action = calf_ui_tap(&ui, 600, 200);
    assert(action.kind == CALF_ACTION_SET_INDICATOR_LED);
    assert(action.selection == 1);
    assert(strcmp(action.value, "stealth") == 0);
    calf_ui_complete_action(&ui, action, 1, "INDICATOR UPDATED");
    assert(ui.screen == CALF_SCREEN_SETTINGS_GENERAL);
    assert(ui.indicator_led_index == 1);

    calf_ui_set_indicator_led(&ui, 0);
    assert(ui.indicator_led_index == 0);
}

static void test_embedded_font_supports_utf8_and_symbol_fallbacks(void)
{
    static uint32_t pixels[CALF_UI_WIDTH * CALF_UI_HEIGHT];
    size_t index;
    int painted = 0;
    bytes_zero(pixels, sizeof(pixels));

    assert(calf_font_has_codepoint(0x00c5u)); /* Å: Noto Sans */
    assert(calf_font_has_codepoint(0x03a9u)); /* Ω: Noto Sans */
    assert(calf_font_has_codepoint(0x0416u)); /* Ж: Noto Sans */
    assert(calf_font_has_codepoint(0x2192u)); /* →: Symbols */
    assert(calf_font_has_codepoint(0x2713u)); /* ✓: Symbols 2 */
    assert(calf_font_has_codepoint(0x1f4f7u)); /* 📷: Symbols 2 */
    assert(calf_font_text_width("SPRÅK Ω Ж → ✓ 📷", 3) > 0);
    assert(calf_font_text_height(1) == 18);
    assert(calf_font_text_height(3) == 30);

    calf_font_draw(pixels, CALF_UI_WIDTH, 20, 20,
                   "SPRÅK Ω Ж → ✓ 📷", 4, 0xffffffffu);
    for(index = 0; index < ARRAY_SIZE(pixels); ++index) {
        if(pixels[index] != 0u) {
            painted = 1;
            break;
        }
    }
    assert(painted);
}

int main(void)
{
    test_sha256_known_vector_and_chunking();
    test_defaults();
    test_drive_mode_settings_and_capture_controls();
    test_capture_sequence_scheduler();
    test_night_preview_timing();
    test_drive_mode_row_layout_and_navigation();
    test_exposure_is_confirmed_only_after_success();
    test_recording_blocks_lens_switch();
    test_live_and_playback_block_lens_switch();
    test_unknown_status_blocks_lens_switch();
    test_status_changes_revision();
    test_render_keeps_preview_area_transparent();
    test_live_histogram_button_and_render();
    test_motion_reticle_and_settle_state();
    test_main_bottom_button_touch_areas();
    test_battery_charge_states_render();
    test_main_shows_image_values_and_connected_wifi_icon();
    test_long_exposure_iso_title_does_not_overlap_status();
    test_settings_hierarchy_and_image_action();
    test_settings_hub_has_opaque_touch_tiles();
    test_display_uses_full_stock_range();
    test_stock_main_key_shortcuts();
    test_key_focus_navigation_and_activation();
    test_settings_focus_is_visible_on_entry();
    test_value_menu_focus_starts_at_applied_selection();
    test_power_key_short_toggle_model();
    test_video_mode_uses_shutter_for_recording();
    test_video_exposure_choices_follow_profile_frame_rate();
    test_main_mode_button_opens_capture_mode();
    test_night_mode_has_manual_long_exposure_controls();
    test_main_zoom_toggles_operator_left_and_stereo_directly();
    test_capture_mode_switch_is_transactional_and_interlocked();
    test_gallery_navigation_and_confirmed_delete();
    test_gallery_enter_reports_blocking_camera_state();
    test_recording_duration_updates_status_revision();
    test_capture_requires_primary_stereo_graph();
    test_backend_image_state_syncs_all_supported_controls();
    test_audio_input_settings_use_backend_readback_and_large_grids();
    test_audio_key_focus_starts_at_current_values();
    test_speaker_volume_audio_screen_and_gallery_keys();
    test_display_off_choices_and_setting();
    test_bq25703_power_decode_tracks_charge_direction();
    test_power_regmap_parser_requires_complete_snapshot();
    test_power_samples_average_valid_readings_and_recording();
    test_power_history_navigation_ring_and_render();
    test_datetime_settings_are_touch_and_key_friendly();
    test_wifi_scan_connect_and_password_entry();
    test_firmware_update_requires_check_and_confirmation();
    test_wifi_power_toggle_is_confirmed_and_recoverable();
    test_usb_ethernet_modes_are_selectable();
    test_stock_ui_switch_is_confirmed_and_interlocked();
    test_resolution_and_encoder_settings_are_functional();
    test_language_menu_and_localization_boundary();
    test_indicator_led_setting();
    test_embedded_font_supports_utf8_and_symbol_fallbacks();
    puts("calf-ui model tests passed");
    return 0;
}
