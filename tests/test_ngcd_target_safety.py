from __future__ import annotations

from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent


def test_four_fps_night_bridge_rearms_before_final_pair() -> None:
    source = (REPOSITORY_ROOT / "ngcd/src/backend_target.c").read_text(
        encoding="utf-8"
    )
    start = source.index("static int target_night_preview(")
    end = source.index("\nstatic int target_set_image(", start)
    function = source[start:end]

    initial_timing = function.index("run_sensor_timing_helper(fps)")
    bridge = function.index("target_apply_preview_pair(state, 0.12f, iso)")
    bridge_rearm = function.index("run_sensor_timing_helper(fps)", bridge)
    fresh_sensor_frame = function.index(
        "target_wait_sensor_outputs(state, 3000)", bridge_rearm
    )
    bridge_stabilization = function.index(
        "target_apply_preview_pair(state, 0.12f, iso)", bridge + 1
    )
    final_rearm = function.index(
        "run_sensor_timing_helper(fps)", bridge_stabilization
    )
    final_pair = function.index(
        "target_apply_preview_pair(state, exposure, iso)", final_rearm
    )

    assert (
        initial_timing
        < bridge
        < bridge_rearm
        < fresh_sensor_frame
        < bridge_stabilization
        < final_rearm
        < final_pair
    )
    assert function.count("run_sensor_timing_helper(fps)") == 3
    assert "target_tick(backend)" not in function
    assert "failure = -35;\n            goto rollback;" in function
    assert "failure = -36;\n            goto rollback;" in function
    assert "failure = -37;\n                goto rollback;" in function
    assert "failure = -38;\n            goto rollback;" in function
    assert "rollback:\n    if (run_sensor_timing_helper(old_fps)" in function


def test_entering_night_restarts_graph_before_image_and_preview() -> None:
    source = (REPOSITORY_ROOT / "ui/src/target.c").read_text(encoding="utf-8")
    capture_mode = source.index(
        "else if(action.kind == CALF_ACTION_SET_CAPTURE_MODE)"
    )
    end = source.index(
        "else if(action.kind == CALF_ACTION_SET_RESOLUTION)", capture_mode
    )
    branch = source[capture_mode:end]

    entering_night = branch.index("int entering_night =")
    stop_graph = branch.index("api_stop_camera_graph()", entering_night)
    start_graph = branch.index(
        "api_start_initial_camera_graph(\n                                         selected_profile)",
        stop_graph,
    )
    image_state = branch.index("api_apply_actual_image_values(", start_graph)
    night_preview = branch.index("api_apply_night_preview(", image_state)
    rollback = branch.index("if(result != 0 && graph_changed)", night_preview)

    assert entering_night < stop_graph < start_graph < image_state < night_preview
    assert night_preview < rollback
    assert "graph_changed = 1;" in branch[stop_graph:start_graph]
    assert '"CAPTURE_MODE", "stage:graph-stop", -1' in branch


def test_graph_start_waits_for_fresh_sensor_output() -> None:
    source = (REPOSITORY_ROOT / "ngcd/src/backend_target.c").read_text(
        encoding="utf-8"
    )
    start = source.index("static int target_restore_graph(")
    end = source.index("\nstatic int target_image_accepted(", start)
    function = source[start:end]

    graph_start = function.index("ngcd_rk_graph_start_in_system(")
    camera_running = function.index(
        "backend->state.camera_running = true", graph_start
    )
    reapply = function.index("target_reapply_image_state(backend, dirty)")
    fresh_output = function.index("target_wait_sensor_outputs(state, 3000)")
    success = function.index("return 0;", fresh_output)
    cleanup = function.index("target_graph_stop(state);", success)

    assert graph_start < camera_running < reapply < fresh_output < success < cleanup


def test_capture_trace_uses_supported_storage_mounts_with_tmp_fallback() -> None:
    source = (REPOSITORY_ROOT / "ngcd/src/backend_target.c").read_text(
        encoding="utf-8"
    )
    start = source.index("static void capture_tracef(")
    end = source.index("\n/* Replace the already-written", start)
    function = source[start:end]

    assert '"/mnt/mmcblk1p1/DCIM/calf-capture.log"' in function
    assert '"/mnt/mmcblk1p2/DCIM/calf-capture.log"' in function
    assert '"/mnt/sda2/DCIM/calf-capture.log"' in function
    assert '"/tmp/calf-capture.log"' in function
    assert "/media/DCIM" not in function
