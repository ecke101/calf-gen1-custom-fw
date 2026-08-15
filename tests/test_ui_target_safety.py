from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def test_left_right_names_use_operator_lcd_viewpoint() -> None:
    model = (REPOSITORY_ROOT / "ui/src/ui.c").read_text(encoding="utf-8")
    capture = (REPOSITORY_ROOT / "src/calf_fw_tool/capture.py").read_text(
        encoding="utf-8"
    )

    assert '{"LEFT", "SENSOR0_4K"}' in model
    assert '{"RIGHT", "SENSOR1_4K"}' in model
    assert '"$raw_converter" "$raw_new_c0"/frame*_normal.raw' in capture
    assert '"$raw_left_temp"' in capture
    assert '"$raw_converter" "$raw_new_c1"/frame*_normal.raw' in capture
    assert '"$raw_right_temp"' in capture


def test_speaker_volume_does_not_read_process_local_rockit_state() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    abi = (REPOSITORY_ROOT / "ui/src/target_abi.h").read_text(encoding="utf-8")

    # librockit keeps its AO device table in process-local state. The UI does
    # not create an AO device, so RK_MPI_AO_GetVolume dereferences a null
    # device pointer when the replacement is the first UI launched at boot.
    assert "RK_MPI_AO_GetVolume" not in target
    assert "RK_MPI_AO_GetVolume" not in abi
    assert 'http_request("POST", "/camera/v2/aplay"' in target
    assert "save_speaker_volume(" in target


def test_replacement_owns_unexpected_exit_fallback_marker() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")

    assert "fatal_handler" in target
    assert "(void)request_stock_ui_session();" in target
    assert "if(!g_supervisor_stop)" in target


def test_gallery_exit_restores_night_preview_after_backend_graph() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    start = target.index("else if(action.kind == CALF_ACTION_GALLERY_EXIT)")
    end = target.index("else if(action.kind == CALF_ACTION_GALLERY_PREV", start)
    gallery_exit = target[start:end]

    assert "result = gallery_close_backend(&gallery);" in gallery_exit
    assert "if(result == 0)" in gallery_exit
    assert "gallery.graph_suspended = 0;" in gallery_exit
    assert "api_start_initial_camera_graph" not in gallery_exit
    assert gallery_exit.index("gallery_close_backend(&gallery)") < gallery_exit.index(
        "api_apply_night_preview("
    )
    assert "capture_mode == CALF_CAPTURE_NIGHT" in gallery_exit
    assert '"GALLERY_EXIT", "stage:night-preview", -1' in gallery_exit


def test_night_preview_is_atomic_and_reapplies_on_wake() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")

    assert '"POST", "/camera/v2/nightpreview"' in target
    assert "api_set_night_preview_transaction(" in target
    assert "action.kind == CALF_ACTION_SET_LCD_POWER" in target
    assert "capture_mode == CALF_CAPTURE_NIGHT" in target
    assert '"LCD_POWER", action.value' in target


def test_gallery_lcd_wake_does_not_apply_night_preview_to_playback_graph() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    start = target.index(
        "if(result == 0 && action.kind == CALF_ACTION_SET_LCD_POWER &&"
    )
    end = target.index(
        "if(result == 0 && capture_mode == CALF_CAPTURE_NIGHT &&", start
    )
    wake = target[start:end]

    assert "action.selection != 0" in wake
    assert "!gallery.active && camera_graph_available" in wake
    assert "api_apply_night_preview(" in wake


def test_gallery_play_reopens_video_after_end_of_file() -> None:
    gallery = (REPOSITORY_ROOT / "ui/src/target_gallery.c").read_text(
        encoding="utf-8"
    )
    start = gallery.index("int gallery_toggle_playback(")
    end = gallery.index("int gallery_delete_current(", start)
    toggle = gallery[start:end]

    assert "state.running == 0" in toggle
    assert "state.paused != 0" in toggle
    assert "state.sample_index >= state.sample_count - 1" in toggle
    assert 'api_playback_action("close")' in toggle
    assert "api_playback_open(gallery->paths[gallery->index])" in toggle
    assert 'api_playback_action("toggle")' in toggle


def test_gallery_volume_notice_is_transient() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    ui_input = (REPOSITORY_ROOT / "ui/src/ui_input.c").read_text(
        encoding="utf-8"
    )
    render = (REPOSITORY_ROOT / "ui/src/ui_render.c").read_text(
        encoding="utf-8"
    )

    assert "GALLERY_VOLUME_OVERLAY_MS" in target
    assert "calf_ui_set_gallery_volume_visible(&ui, 1);" in target
    assert "calf_ui_set_gallery_volume_visible(&ui, 0);" in target
    assert "gallery_volume_action ||" in target
    assert '? "" : success_message(action.kind)' in target
    assert "return begin_quiet_action(ui, CALF_ACTION_SET_SPEAKER_VOLUME" in ui_input
    assert "ui->gallery_volume_visible" in render
    assert 'append_text(volume, sizeof(volume), "  UP/DOWN")' not in render


def test_image_setter_retries_one_shot_coordinator_restart() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    start = target.index("static int api_perform_action(")
    end = target.index("static int api_apply_audio_input_state(", start)
    perform_action = target[start:end]

    assert "image-state coordinator uses a one-shot netcat listener" in perform_action
    assert "type != (const char *)0" in perform_action
    assert "usleep(250000u);" in perform_action
    assert perform_action.count('http_request("POST", path, fixed_body') == 2


def test_image_profile_save_migrates_stock_216_profile() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    start = target.index("static int save_stock_image_parameter(")
    end = target.index("static int parse_decimal_string(", start)
    save_image = target[start:end]

    assert 'if(section == (const char *)0) {' in save_image
    assert '"image_params:\\n  "' in save_image
    assert "return write_profile_edit(source, end, end, addition);" in save_image


def test_deep_idle_wake_accepts_release_and_lights_lcd_before_graph() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    platform = (REPOSITORY_ROOT / "ui/src/target_platform.c").read_text(
        encoding="utf-8"
    )
    wake_start = target.index("if(deep_idle) {")
    wake_end = target.index("else if(!ui.lcd_on)", wake_start)
    wake = target[wake_start:wake_end]

    assert "keys->power_pressed ||" in platform
    assert "(suppress_actions && !allow_wake)" in platform
    assert "? power_hold_milliseconds(keys, &event) : 0" in platform
    assert wake.index("api_set_lcd_power(&ui, 1)") < wake.index(
        "api_restore_primary_graph("
    )


def test_interval_graph_sleep_preserves_independent_lcd_control() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")

    power_start = target.index("if(physical_power_short &&")
    power_end = target.index("if(capture_sequence.active &&", power_start)
    power = target[power_start:power_end]
    assert power.index("if(capture_sequence.active)") < power.index(
        "else if(deep_idle)"
    )
    assert "api_set_lcd_power(&ui, !ui.lcd_on)" in power

    sleep_start = target.index("else if(scheduled_snapshot && result == 0")
    sleep_end = target.index("if(gallery.active)", sleep_start)
    interval_sleep = target[sleep_start:sleep_end]
    assert "api_stop_camera_graph()" in interval_sleep
    assert "api_set_lcd_power" not in interval_sleep
    assert "INTERVAL LCD SLEEP FAILED" not in target

    assert "if(!has_action && ui.lcd_on &&\n" in target
    assert "!deep_idle || capture_sequence.active" in target

    cancel_start = target.index(
        "else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL)"
    )
    cancel_end = target.index(
        "else if(action.kind == CALF_ACTION_GALLERY_ENTER)", cancel_start
    )
    cancel = target[cancel_start:cancel_end]
    assert "if(!deep_idle)" in cancel
    assert "api_restore_primary_graph(" in cancel
    assert "deep_idle = 0;" in cancel


def test_snapshot_uses_direct_coordinator_port() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    start = target.index("action.kind == CALF_ACTION_SNAPSHOT", target.index("request_result"))
    snapshot = target[start : target.index("/* The image-state coordinator", start)]

    assert "http_request_port_with_timeout" in snapshot
    assert "120, 8990" in snapshot


def test_target_recovers_orphaned_pending_actions() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    platform = (REPOSITORY_ROOT / "ui/src/target_platform.c").read_text(
        encoding="utf-8"
    )

    assert "if(ui.pending_action != CALF_ACTION_NONE &&" in target
    assert "!api_action_worker_busy(&action_worker)" in target
    assert "!worker_result_ready" in target
    assert 'calf_ui_notice(&ui, "READY - TRY AGAIN", 1);' in target
    assert platform.count("if(candidate.kind != CALF_ACTION_NONE)") >= 3


def test_backend_api_actions_run_without_blocking_the_ui_loop() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    abi = (REPOSITORY_ROOT / "ui/src/target_abi.h").read_text(encoding="utf-8")
    builder = (REPOSITORY_ROOT / "src/calf_fw_tool/ui_builder.py").read_text(
        encoding="utf-8"
    )

    assert "api_action_worker_submit(" in target
    assert "api_action_worker_take(" in target
    assert "atomic_store_explicit" in target
    assert "atomic_load_explicit" in target
    assert "api_action_worker_busy(&action_worker)" in target
    assert "API_WORKER_WIFI_SCAN" in target
    assert "wifi_scan_collect(&worker->wifi_scan)" in target
    assert "calf_ui_set_wifi_networks(" in target
    assert "http_request_with_timeout(\"POST\", path" in target
    assert "sizeof(response), 120" in target
    assert "pthread_create" in abi and "pthread_join" in abi
    assert '"/lib/libpthread-2.33.so"' in builder


def test_wifi_password_renderer_masks_secret_text() -> None:
    render = (REPOSITORY_ROOT / "ui/src/ui_render.c").read_text(encoding="utf-8")
    start = render.index("static void draw_wifi_password(")
    end = render.index("static void draw_wifi_off_confirm(", start)
    password_screen = render[start:end]

    assert "masked_password" in password_screen
    assert "masked_password[index] = '*'" in password_screen
    assert ": ui->wifi_password" not in password_screen


def test_target_debounces_touch_releases_and_physical_keys() -> None:
    platform = (REPOSITORY_ROOT / "ui/src/target_platform.c").read_text(
        encoding="utf-8"
    )
    internal = (REPOSITORY_ROOT / "ui/src/target_internal.h").read_text(
        encoding="utf-8"
    )

    assert "#define INPUT_DEBOUNCE_MS 180u" in internal
    assert "touch->tap_armed = 1;" in platform
    assert "!touch->pressed && touch->tap_armed" in platform
    assert "touch->tap_armed = 0;" in platform
    assert "keys->pressed_keys |= bit;" in platform
    assert "keys->pressed_keys &= ~bit;" in platform
    assert platform.count("elapsed_milliseconds(") >= 3
    assert "#define PHYSICAL_BUTTON_QUIET_MS 300u" in internal
    assert "keys->last_button_release = event.time;" in platform
    assert "keys->pressed_keys != 0U ||" in platform
    assert "!debounced && !bank_busy && bank_quiet" in platform


def test_gallery_play_is_quiet_and_has_action_level_lockout() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    ui_input = (REPOSITORY_ROOT / "ui/src/ui_input.c").read_text(
        encoding="utf-8"
    )
    internal = (REPOSITORY_ROOT / "ui/src/target_internal.h").read_text(
        encoding="utf-8"
    )

    assert "#define GALLERY_PLAY_DEBOUNCE_MS 750u" in internal
    assert "now_ms < gallery.play_ready_ms" in target
    assert "now_ms + GALLERY_PLAY_DEBOUNCE_MS" in target
    assert "action.kind ==\n                                             CALF_ACTION_GALLERY_PLAY_TOGGLE" in target
    tap_start = ui_input.index("calf_action_t calf_ui_tap(")
    gallery_tap = ui_input[tap_start:]
    gallery_tap = gallery_tap[: gallery_tap.index("CALF_ACTION_GALLERY_NEXT")]
    assert "begin_quiet_action(ui," in gallery_tap


def test_target_persists_supervisor_lifecycle_diagnostics() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    internal = (REPOSITORY_ROOT / "ui/src/target_internal.h").read_text(
        encoding="utf-8"
    )

    assert '#define SUPERVISOR_LOG_PATH "/local/calf-supervisor.log"' in internal
    assert "supervisor_log_ui_start();" in target
    assert "supervisor_log_ui_exit(result);" in target
    assert 'supervisor_log_append("CALF_UI_SIGNAL term\\n"' in target
    assert 'supervisor_log_append("CALF_UI_FATAL segv\\n"' in target


def test_startup_restores_persisted_primary_graph_state() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    platform = (REPOSITORY_ROOT / "ui/src/target_platform.c").read_text(
        encoding="utf-8"
    )

    startup = target[target.index("(void)load_camera_profiles(&camera_profiles);") :]
    startup = startup[: startup.index("touch_available =")]
    assert "capture_mode = camera_profiles.mode;" in startup
    assert "camera_profiles.video : camera_profiles.photo" in startup
    assert "api_start_initial_camera_graph(primary_profile)" in startup
    assert "api_apply_encoder_profile(primary_profile)" in startup
    assert "api_sync_image_state(&ui, 1)" in startup
    assert "api_apply_audio_input_state(&audio_input_state)" in startup

    restore_start = target.index("static int api_restore_primary_graph(")
    restore_end = target.index("static int api_sync_audio_state(", restore_start)
    restore = target[restore_start:restore_end]
    assert "api_start_initial_camera_graph(profile)" in restore
    assert "api_apply_encoder_profile(profile)" in restore
    assert "api_sync_image_state(ui, 1)" in restore
    assert "api_apply_audio_input_state(audio_state)" in restore

    assert "attempt < 100 && display->control_fd < 0" in platform
    assert "attempt < 100 && display->pixel_fd < 0" in platform


def test_enabled_wifi_is_started_and_retried_at_boot() -> None:
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    internal = (REPOSITORY_ROOT / "ui/src/target_internal.h").read_text(
        encoding="utf-8"
    )

    service_start = target[target.index("static int wifi_start_service(void)") :]
    service_start = service_start[
        : service_start.index("static void wifi_refresh_connection")
    ]
    assert '"/app/bin/wifiservice", "start"' in service_start
    assert '"/app/bin/calf-wlan"' not in service_start

    startup = target[target.index("if(camera_graph_available) {") :]
    startup = startup[: startup.index("touch_available =")]
    assert "if(ui.wifi_enabled)" in startup
    assert "(void)wifi_start_service();" in startup

    assert "WIFI_START_MAX_ATTEMPTS 3u" in internal
    assert "wifi_start_attempts < WIFI_START_MAX_ATTEMPTS" in target
    assert "WIFI_START_RETRY_INTERVAL_MS" in target


def test_gallery_media_pixels_are_backend_owned() -> None:
    gallery = (REPOSITORY_ROOT / "ui/src/target_gallery.c").read_text(
        encoding="utf-8"
    )
    target = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")

    assert "gallery_load_turbojpeg" not in gallery
    assert "tjDecompress" not in gallery
    assert "gallery_compute_photo_histogram" not in gallery
    assert "gallery_load_photo_preview" not in target


def test_gallery_photo_delete_also_removes_raw_pair() -> None:
    gallery = (REPOSITORY_ROOT / "ui/src/target_gallery.c").read_text(
        encoding="utf-8"
    )
    start = gallery.index("int gallery_delete_current(")
    delete = gallery[start:]

    assert 'left_suffix[] = "-L.dng"' in delete
    assert 'right_suffix[] = "-R.dng"' in delete
    assert "(void)unlink(raw_left);" in delete
    assert "(void)unlink(raw_right);" in delete


def test_gallery_catalog_is_paged_from_backend() -> None:
    gallery = (REPOSITORY_ROOT / "ui/src/target_gallery.c").read_text(
        encoding="utf-8"
    )

    assert '"/camera/v2/media?offset="' in gallery
    assert '"&limit=64"' in gallery
    assert 'parse_integer_after(response, "\\\"total\\\"", &total)' in gallery
    assert "total > 10000" in gallery
