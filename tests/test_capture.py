from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from calf_fw_tool.capture import build_capture_request, build_capture_server

_FAKE_CURL = r"""#!/bin/sh
printf '%s\n' "$*" >> "$CALF_FAKE_CURL_LOG"
case "$*" in
    *'"type":"exp","value":"0.5"'*)
        fail_limit=${CALF_FAKE_IMAGE_FAIL_COUNT:-0}
        fail_count=0
        if [ -r "$CALF_FAKE_IMAGE_FAIL_STATE" ]; then
            read -r fail_count < "$CALF_FAKE_IMAGE_FAIL_STATE"
        fi
        if [ "$fail_count" -lt "$fail_limit" ]; then
            fail_count=$((fail_count + 1))
            printf '%s\n' "$fail_count" > "$CALF_FAKE_IMAGE_FAIL_STATE"
            printf '{"code":-1,"message":"set image parameters failed (-1)"}'
            exit 0
        fi
        ;;
esac
for argument do
    final_argument=$argument
done
case "$final_argument" in
    */snapshot)
        if [ -r "$CALF_NIGHT_STACK_COUNT" ]; then
            printf 'night-stack %s\n' "$(cat "$CALF_NIGHT_STACK_COUNT")" \
                >> "$CALF_FAKE_CURL_LOG"
        fi
        if [ "${CALF_FAKE_RAW_CREATE:-}" = 1 ]; then
            raw_count=$(cat "$CALF_RAW_COUNT_C0" 2>/dev/null || printf '1')
            printf 'raw-count %s\n' "$raw_count" >> "$CALF_FAKE_CURL_LOG"
            for camera in 0 1; do
                raw_dir="$CALF_FAKE_RAW_CAPTURE_DIR/Cam$camera-raw_1"
                mkdir -p "$raw_dir"
                printf 'metadata\n' > "$raw_dir/meta_data"
                raw_number=1
                while [ "$raw_number" -le "$raw_count" ]; do
                    printf 'raw\n' > "$raw_dir/frame${raw_number}_normal.raw"
                    raw_number=$((raw_number + 1))
                done
            done
            if [ "${CALF_FAKE_KEEP_RAW_TRIGGERS:-}" != 1 ]; then
                rm -f "$CALF_RAW_COUNT_C0" "$CALF_RAW_COUNT_C1"
            fi
            : > "$CALF_FAKE_RAW_OUTPUT_DIR/V1000999.jpg"
        fi
        if [ "${CALF_FAKE_SNAPSHOT_FAIL:-}" = 1 ]; then
            printf '{"code":-1,"message":"snapshot failed"}'
            exit 0
        fi
        printf '{"code":0,"body":{"filename":"V1000999.jpg"}}'
        ;;
    *)
        printf '{"code":0}'
        ;;
esac
"""

_FAKE_SENSOR_TIMING = r"""#!/bin/sh
printf 'sensor-timing %s\n' "$1" >> "$CALF_FAKE_CURL_LOG"
if [ "${CALF_FAKE_SENSOR_FAIL:-}" = "$1" ]; then
    exit 1
fi
exit 0
"""

_FAKE_RAW_CONVERTER = r"""#!/bin/sh
first=$1
for final do :; done
printf '%s -> %s\n' "$first" "$final" >> "$CALF_FAKE_RAW_CONVERTER_LOG"
printf 'dng\n' > "$final"
"""


class CaptureCoordinatorTests(unittest.TestCase):
    def test_server_relistens_immediately_after_successful_request(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            server = Path(temp_name) / "calf-capture-server"
            build_capture_server(server)
            text = server.read_text(encoding="ascii")

        self.assertIn('if [ "$nc_status" -ne 0 ]; then', text)
        self.assertNotIn("nc_pid=\n            sleep 1", text)

    def test_server_stop_does_not_kill_stale_reused_pid(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            temp = Path(temp_name)
            server = temp / "calf-capture-server"
            pidfile = temp / "server.pid"
            build_capture_server(server)
            unrelated = subprocess.Popen(["sleep", "30"])
            self.addCleanup(lambda: unrelated.terminate())
            pidfile.write_text(f"{unrelated.pid}\n", encoding="ascii")
            environment = os.environ.copy()
            environment["CALF_CAPTURE_PIDFILE"] = str(pidfile)

            subprocess.run(
                ["/bin/sh", str(server), "stop"],
                env=environment,
                check=True,
            )

            self.assertIsNone(unrelated.poll())
            self.assertFalse(pidfile.exists())

    def _run_request(
        self,
        exp: str,
        iso: str = "iso100",
        *,
        stale_lock: bool = False,
        live_lock: bool = False,
        fail_sensor_fps: str = "",
        raw_enabled: bool = False,
        raw_converter_available: bool = False,
        indicator_stealth: bool = False,
        fail_image_attempts: int = 0,
        fail_snapshot: bool = False,
        stale_raw_triggers: bool = False,
        profile_has_image_params: bool = True,
    ) -> tuple[str, list[str], Path]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        temp = Path(temporary.name)
        request = temp / "calf-snapshot-request"
        fake_curl = temp / "fake-curl"
        fake_sensor_timing = temp / "fake-sensor-timing"
        fake_raw_converter = temp / "fake-raw2dng"
        profile = temp / "profile.yaml"
        calls = temp / "calls.log"
        lock = temp / "capture.lock"
        fps_state = temp / "capture-fps"
        night_stack_count = temp / "night-stack-count"
        trace_file = temp / "capture-trace"
        capture_log = temp / "capture.log"
        raw_enabled_file = temp / "raw-enabled"
        indicator_led_file = temp / "indicator-led"
        blue_led_trigger = temp / "blue-trigger"
        blue_led_brightness = temp / "blue-brightness"

        build_capture_request(request)
        fake_curl.write_text(_FAKE_CURL, encoding="ascii")
        fake_sensor_timing.write_text(_FAKE_SENSOR_TIMING, encoding="ascii")
        fake_raw_converter.write_text(_FAKE_RAW_CONVERTER, encoding="ascii")
        request.chmod(request.stat().st_mode | stat.S_IXUSR)
        fake_curl.chmod(fake_curl.stat().st_mode | stat.S_IXUSR)
        fake_sensor_timing.chmod(
            fake_sensor_timing.stat().st_mode | stat.S_IXUSR
        )
        fake_raw_converter.chmod(
            fake_raw_converter.stat().st_mode | stat.S_IXUSR
        )
        image_params = (
            "image_params:\n" f"  iso: {iso}\n" f"  exp: {exp}\n"
            if profile_has_image_params
            else ""
        )
        profile.write_text(
            "cam_mode: photo\n"
            f"{image_params}"
            "wlan:\n"
            "  enable: true\n",
            encoding="ascii",
        )
        if stale_lock or live_lock:
            lock.mkdir()
        if raw_enabled:
            raw_enabled_file.write_text("1\n", encoding="ascii")
        indicator_led_file.write_text(
            "stealth\n" if indicator_stealth else "normal\n",
            encoding="ascii",
        )
        blue_led_trigger.write_text("timer\n", encoding="ascii")
        blue_led_brightness.write_text("7\n", encoding="ascii")
        if live_lock:
            (lock / "owner").write_text(f"{os.getpid()}\n", encoding="ascii")
        if stale_raw_triggers:
            (temp / "capture-c0").write_text("24\n", encoding="ascii")
            (temp / "capture-c1").write_text("24\n", encoding="ascii")

        environment = os.environ.copy()
        environment.update(
            {
                "CALF_CAPTURE_API": "http://camera.test/camera/v2",
                "CALF_CAPTURE_PROFILE": str(profile),
                "CALF_CAPTURE_CURL": str(fake_curl),
                "CALF_CAPTURE_SENSOR_TIMING": str(fake_sensor_timing),
                "CALF_CAPTURE_SLEEP": "/usr/bin/true",
                "CALF_CAPTURE_LOCK": str(lock),
                "CALF_CAPTURE_FPS_STATE": str(fps_state),
                "CALF_NIGHT_STACK_COUNT": str(night_stack_count),
                "CALF_CAPTURE_TRACE_FILE": str(trace_file),
                "CALF_CAPTURE_LOG": str(capture_log),
                "CALF_RAW_ENABLED_FILE": str(raw_enabled_file),
                "CALF_RAW_CAPTURE_DIR": str(temp / "raw-capture"),
                "CALF_RAW_COUNT_C0": str(temp / "capture-c0"),
                "CALF_RAW_COUNT_C1": str(temp / "capture-c1"),
                "CALF_RAW_CONVERTER": str(
                    fake_raw_converter
                    if raw_converter_available
                    else temp / "missing-raw2dng"
                ),
                "CALF_RAW_OUTPUT_DIR": str(temp),
                "CALF_RAW_JOB_DIR": str(temp / "raw-jobs"),
                "CALF_FAKE_RAW_CREATE": (
                    "1"
                    if (raw_enabled and raw_converter_available)
                    or exp in {"1", "2", "4", "8", "12"}
                    else ""
                ),
                "CALF_FAKE_RAW_CAPTURE_DIR": str(temp / "raw-capture"),
                "CALF_FAKE_RAW_OUTPUT_DIR": str(temp),
                "CALF_FAKE_RAW_CONVERTER_LOG": str(temp / "raw-converter.log"),
                "CALF_INDICATOR_LED_FILE": str(indicator_led_file),
                "CALF_BLUE_LED_TRIGGER": str(blue_led_trigger),
                "CALF_BLUE_LED_BRIGHTNESS": str(blue_led_brightness),
                "CALF_FAKE_CURL_LOG": str(calls),
                "CALF_FAKE_SENSOR_FAIL": fail_sensor_fps,
                "CALF_FAKE_IMAGE_FAIL_COUNT": str(fail_image_attempts),
                "CALF_FAKE_IMAGE_FAIL_STATE": str(temp / "image-fail-count"),
                "CALF_FAKE_SNAPSHOT_FAIL": "1" if fail_snapshot else "",
                "CALF_FAKE_KEEP_RAW_TRIGGERS": "1" if fail_snapshot else "",
            }
        )
        result = subprocess.run(
            ["/bin/sh", str(request)],
            input="POST /camera/v2/snapshot HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}",
            text=True,
            capture_output=True,
            env=environment,
            check=True,
        )
        call_lines = (
            calls.read_text(encoding="ascii").splitlines()
            if calls.exists()
            else []
        )
        if live_lock:
            self.assertTrue(lock.exists())
        else:
            self.assertFalse(lock.exists())
        self.assertFalse(fps_state.exists())
        self.assertFalse(night_stack_count.exists())
        self.assertFalse(trace_file.exists())
        return result.stdout, call_lines, temp

    def test_empty_stale_lock_is_reclaimed(self) -> None:
        output, calls, temp = self._run_request("-1", "auto", stale_lock=True)

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        self.assertIn(
            "reclaimed stale capture lock",
            (temp / "capture.log").read_text(encoding="ascii"),
        )

    def test_216_profile_without_image_section_clears_preview_transients(self) -> None:
        output, calls, _ = self._run_request(
            "unused", "unused", profile_has_image_params=False
        )

        self.assertIn("HTTP/1.1 200 OK", output)
        combined = "\n".join(calls)
        self.assertIn('"type":"exp","value":"-1"', combined)
        self.assertIn('"type":"iso","value":"auto"', combined)
        self.assertLess(combined.index('"type":"exp"'), combined.index("/snapshot"))
        self.assertLess(combined.index('"type":"iso"'), combined.index("/snapshot"))

    def test_live_lock_owner_remains_busy(self) -> None:
        output, calls, _ = self._run_request("-1", "auto", live_lock=True)

        self.assertIn("HTTP/1.1 503 Service Unavailable", output)
        self.assertIn('"message":"capture busy"', output)
        self.assertEqual(calls, [])

    def test_half_second_capture_applies_2_and_restores_30(self) -> None:
        output, calls, _ = self._run_request("0.5")

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertIn('"filename":"V1000999.jpg"', output)
        combined = "\n".join(calls)
        self.assertIn("sensor-timing 2", combined)
        self.assertIn("sensor-timing 30", combined)
        self.assertLess(
            combined.index("sensor-timing 2"),
            combined.index("/snapshot"),
        )
        self.assertLess(
            combined.index("/snapshot"),
            combined.index("sensor-timing 30"),
        )
        self.assertIn('"value":"0.5"', combined)
        self.assertIn('"value":"iso100"', combined)
        self.assertLess(
            combined.index("sensor-timing 2"),
            combined.index('"value":"0.5"'),
        )

    def test_all_slow_exposures_select_matching_sensor_timing(self) -> None:
        for exposure, fps in (
            ("12", "2"),
            ("8", "2"),
            ("4", "2"),
            ("2", "2"),
            ("1", "2"),
            ("0.25", "4"),
            ("0.125", "8"),
            ("0.0666667", "15"),
        ):
            with self.subTest(exposure=exposure, fps=fps):
                output, calls, _ = self._run_request(exposure)
                combined = "\n".join(calls)
                self.assertIn("HTTP/1.1 200 OK", output)
                self.assertIn(f"sensor-timing {fps}", combined)
                self.assertIn("sensor-timing 30", combined)
                self.assertLess(
                    combined.index(f"sensor-timing {fps}"),
                    combined.index("/snapshot"),
                )
                self.assertNotIn("/cameramode", combined)

    def test_night_exposures_capture_temporary_half_second_raw_stacks(self) -> None:
        for exposure, frame_count in (
            ("1", 2),
            ("2", 4),
            ("4", 8),
            ("8", 16),
            ("12", 24),
        ):
            with self.subTest(exposure=exposure):
                output, calls, temp = self._run_request(exposure)
                self.assertIn("HTTP/1.1 200 OK", output)
                combined = "\n".join(calls)
                self.assertIn('"value":"0.5"', combined)
                self.assertIn("sensor-timing 2", combined)
                self.assertIn(f"night-stack {frame_count}", calls)
                self.assertFalse((temp / "capture-c0").exists())
                self.assertFalse((temp / "capture-c1").exists())
                self.assertFalse(
                    (temp / "raw-capture/Cam0-raw_1").exists()
                )
                self.assertFalse(
                    (temp / "raw-capture/Cam1-raw_1").exists()
                )
                trace_log = (temp / "capture.log").read_text(encoding="ascii")
                self.assertIn("stage=backend-snapshot begin", trace_log)
                self.assertIn("stage=backend-snapshot result=0", trace_log)
                self.assertIn("stage=restore timing=ok", trace_log)

    def test_sensor_timing_failure_restores_30_without_snapshot(self) -> None:
        output, calls, temp = self._run_request(
            "0.125", fail_sensor_fps="8"
        )

        combined = "\n".join(calls)
        self.assertIn("HTTP/1.1 502 Bad Gateway", output)
        self.assertIn('"message":"sensor timing setup failed"', output)
        self.assertIn("sensor-timing 8", combined)
        self.assertIn("sensor-timing 30", combined)
        self.assertFalse(any("/snapshot" in call for call in calls))
        trace_log = (temp / "capture.log").read_text(encoding="ascii")
        self.assertLess(
            trace_log.index("stage=restore begin"),
            trace_log.index("stage=response status=502"),
        )

    def test_image_parameters_retry_transient_backend_failures(self) -> None:
        output, calls, temp = self._run_request("2", fail_image_attempts=2)

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        self.assertEqual(
            sum(
                '"type":"exp","value":"0.5"' in call
                for call in calls
            ),
            3,
        )
        trace_log = (temp / "capture.log").read_text(encoding="ascii")
        self.assertIn(
            "stage=image-params-request result=failed attempt=1",
            trace_log,
        )
        self.assertIn(
            "stage=image-params-request result=failed attempt=2",
            trace_log,
        )
        self.assertIn("stage=image-params result=ok", trace_log)

    def test_persistent_image_failure_rebuilds_graph_once(self) -> None:
        output, calls, temp = self._run_request("2", fail_image_attempts=3)

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        camera_mode_calls = [
            call for call in calls if "/cameramode" in call
        ]
        self.assertEqual(len(camera_mode_calls), 2)
        self.assertIn('"action":"stop"', camera_mode_calls[0])
        self.assertIn('"action":"start"', camera_mode_calls[1])
        self.assertIn('"mode":"VR180_PIC"', camera_mode_calls[1])
        self.assertEqual(calls.count("sensor-timing 2"), 2)
        self.assertEqual(calls.count("sensor-timing 30"), 2)
        trace_log = (temp / "capture.log").read_text(encoding="ascii")
        self.assertIn("stage=image-recovery result=ok", trace_log)
        self.assertIn("stage=image-params result=recovered", trace_log)

    def test_image_parameter_failure_restores_before_error_response(self) -> None:
        output, calls, temp = self._run_request("1", fail_image_attempts=6)

        self.assertIn("HTTP/1.1 502 Bad Gateway", output)
        self.assertIn('"message":"exposure setup failed"', output)
        self.assertFalse(any("/snapshot" in call for call in calls))
        trace_log = (temp / "capture.log").read_text(encoding="ascii")
        failed = trace_log.index("stage=image-params result=failed")
        restore = trace_log.index("stage=restore begin", failed)
        restored_params = trace_log.index(
            "stage=restore image-params=ok", restore
        )
        response = trace_log.index("stage=response status=502", restored_params)
        self.assertLess(failed, restore)
        self.assertLess(restore, restored_params)
        self.assertLess(restored_params, response)

    def test_failed_30_fps_restore_is_retried_and_reported(self) -> None:
        output, calls, _ = self._run_request("0.0666667", fail_sensor_fps="30")

        self.assertIn("HTTP/1.1 502 Bad Gateway", output)
        self.assertIn('"message":"30 fps restore failed"', output)
        self.assertEqual(calls.count("sensor-timing 30"), 2)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)

    def test_disconnect_signal_exits_through_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            request = Path(temp_name) / "calf-snapshot-request"
            build_capture_request(request)
            text = request.read_text(encoding="ascii")

        self.assertIn("trap cleanup EXIT", text)
        self.assertIn("trap 'exit 0' HUP INT TERM PIPE", text)

    def test_snapshot_backend_timeout_allows_24_frame_stack(self) -> None:
        with tempfile.TemporaryDirectory() as temp_name:
            request = Path(temp_name) / "calf-snapshot-request"
            build_capture_request(request)
            text = request.read_text(encoding="ascii")

        self.assertEqual(text.count("post_json \"$api/snapshot\" '{}' 90"), 2)

    def test_auto_bypasses_sensor_timing(self) -> None:
        output, calls, _ = self._run_request("-1", "auto")

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        self.assertFalse(any("cameramode" in call for call in calls))
        self.assertFalse(any("sensor-timing" in call for call in calls))

    def test_fast_manual_exposure_bypasses_sensor_timing(self) -> None:
        output, calls, _ = self._run_request("0.004")

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        self.assertFalse(any("cameramode" in call for call in calls))
        self.assertFalse(any("sensor-timing" in call for call in calls))

    def test_normal_indicator_pulses_and_finishes_off(self) -> None:
        output, _, temp = self._run_request("-1")

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(
            (temp / "blue-trigger").read_text(encoding="ascii"), "none\n"
        )
        self.assertEqual(
            (temp / "blue-brightness").read_text(encoding="ascii"), "0\n"
        )

    def test_stealth_indicator_does_not_touch_led(self) -> None:
        output, _, temp = self._run_request("0.5", indicator_stealth=True)

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(
            (temp / "blue-trigger").read_text(encoding="ascii"), "timer\n"
        )
        self.assertEqual(
            (temp / "blue-brightness").read_text(encoding="ascii"), "7\n"
        )

    def test_enabled_raw_fails_before_jpeg_when_converter_is_missing(self) -> None:
        output, calls, temp = self._run_request("-1", raw_enabled=True)

        self.assertIn("HTTP/1.1 507 Insufficient Storage", output)
        self.assertIn('"message":"RAW capture unavailable"', output)
        self.assertFalse(any("/snapshot" in call for call in calls))
        self.assertIn(
            "converter unavailable",
            (temp / "capture.log").read_text(encoding="ascii"),
        )

    def test_raw_conversion_is_queued_after_pairing(self) -> None:
        output, calls, temp = self._run_request(
            "-1", raw_enabled=True, raw_converter_available=True
        )

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        left = temp / "V1000999-L.dng"
        right = temp / "V1000999-R.dng"
        raw_left = temp / "raw-capture/Cam0-raw_1"
        raw_right = temp / "raw-capture/Cam1-raw_1"
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and not (
            left.exists()
            and right.exists()
            and not raw_left.exists()
            and not raw_right.exists()
            and not (temp / "raw-jobs/0").exists()
            and not (temp / "raw-jobs/1").exists()
        ):
            time.sleep(0.01)
        self.assertTrue(left.exists())
        self.assertTrue(right.exists())
        converter_calls = (temp / "raw-converter.log").read_text(
            encoding="ascii"
        ).splitlines()
        self.assertEqual(len(converter_calls), 2)
        self.assertFalse(raw_left.exists())
        self.assertFalse(raw_right.exists())

    def test_night_raw_uses_the_same_count_as_the_jpeg_stack(self) -> None:
        output, calls, temp = self._run_request(
            "4", raw_enabled=True, raw_converter_available=True
        )

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertIn("night-stack 8", calls)
        self.assertIn("raw-count 8", calls)
        self.assertFalse((temp / "capture-c0").exists())
        self.assertFalse((temp / "capture-c1").exists())
        left = temp / "V1000999-L.dng"
        right = temp / "V1000999-R.dng"
        raw_left = temp / "raw-capture/Cam0-raw_1"
        raw_right = temp / "raw-capture/Cam1-raw_1"
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and not (
            left.exists()
            and right.exists()
            and not raw_left.exists()
            and not raw_right.exists()
            and not (temp / "raw-jobs/0").exists()
            and not (temp / "raw-jobs/1").exists()
        ):
            time.sleep(0.01)
        self.assertTrue(left.exists())
        self.assertTrue(right.exists())
        self.assertFalse(raw_left.exists())
        self.assertFalse(raw_right.exists())

    def test_failed_night_snapshot_removes_triggers_and_temporary_raw(self) -> None:
        output, _, temp = self._run_request("4", fail_snapshot=True)

        self.assertIn("HTTP/1.1 502 Bad Gateway", output)
        self.assertFalse((temp / "capture-c0").exists())
        self.assertFalse((temp / "capture-c1").exists())
        self.assertFalse((temp / "raw-capture/Cam0-raw_1").exists())
        self.assertFalse((temp / "raw-capture/Cam1-raw_1").exists())

    def test_stale_raw_triggers_are_cleared_before_ordinary_snapshot(self) -> None:
        output, calls, temp = self._run_request(
            "-1", "auto", stale_raw_triggers=True
        )

        self.assertIn("HTTP/1.1 200 OK", output)
        self.assertEqual(sum("/snapshot" in call for call in calls), 1)
        self.assertFalse((temp / "capture-c0").exists())
        self.assertFalse((temp / "capture-c1").exists())


if __name__ == "__main__":
    unittest.main()
