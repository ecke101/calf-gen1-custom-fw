from __future__ import annotations

from pathlib import Path

from .util import sha256

CAPTURE_SERVER_FILESYSTEM_PATH = "/bin/calf-capture-server"
CAPTURE_SERVER_RUNTIME_PATH = "/app/bin/calf-capture-server"
CAPTURE_REQUEST_FILESYSTEM_PATH = "/bin/calf-snapshot-request"
CAPTURE_REQUEST_RUNTIME_PATH = "/app/bin/calf-snapshot-request"
CAPTURE_SERVER = r"""#!/bin/sh

pidfile=${CALF_CAPTURE_PIDFILE:-/tmp/calf-capture-server.pid}
nc_bin=${CALF_CAPTURE_NC:-/usr/bin/nc}
handler=${CALF_CAPTURE_HANDLER:-/app/bin/calf-snapshot-request}

pid_is_ours()
{
    candidate=$1
    case "$candidate" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$candidate" -gt 1 ] 2>/dev/null || return 1
    [ -r "/proc/$candidate/cmdline" ] || return 1
    tr '\000' '\n' < "/proc/$candidate/cmdline" |
        grep -F -x "$0" >/dev/null 2>&1 || return 1
    tr '\000' '\n' < "/proc/$candidate/cmdline" |
        grep -F -x serve >/dev/null 2>&1
}

case "$1" in
    start)
        if [ -r "$pidfile" ]; then
            read -r old_pid < "$pidfile"
            if pid_is_ours "$old_pid"; then
                exit 0
            fi
            rm -f "$pidfile"
        fi
        "$0" serve >/tmp/calf-capture-server.log 2>&1 &
        server_pid=$!
        pid_temp=$pidfile.$$
        (umask 077; set -C; printf '%s\n' "$server_pid" > "$pid_temp") || {
            kill "$server_pid" 2>/dev/null
            exit 1
        }
        mv -f "$pid_temp" "$pidfile" || {
            rm -f "$pid_temp"
            kill "$server_pid" 2>/dev/null
            exit 1
        }
        ;;
    stop)
        if [ -r "$pidfile" ]; then
            read -r old_pid < "$pidfile"
            if pid_is_ours "$old_pid"; then
                kill "$old_pid" 2>/dev/null
            fi
            rm -f "$pidfile"
        fi
        ;;
    serve)
        nc_pid=
        trap '[ -n "$nc_pid" ] && kill "$nc_pid" 2>/dev/null; exit 0' HUP INT TERM
        while :; do
            "$nc_bin" -l -p 8990 -e "$handler" &
            nc_pid=$!
            wait "$nc_pid"
            nc_status=$?
            nc_pid=
            if [ "$nc_status" -ne 0 ]; then
                sleep 1
            fi
        done
        ;;
    *)
        echo "Usage: $0 {start|stop|serve}" >&2
        exit 2
        ;;
esac
"""


CAPTURE_REQUEST = r"""#!/bin/sh

api=${CALF_CAPTURE_API:-http://127.0.0.1:8989/camera/v2}
profile=${CALF_CAPTURE_PROFILE:-/local/ngui-profile.yaml}
curl_bin=${CALF_CAPTURE_CURL:-/usr/bin/curl}
sleep_bin=${CALF_CAPTURE_SLEEP:-sleep}
sensor_timing=${CALF_CAPTURE_SENSOR_TIMING:-/app/bin/calf-sensor-timing}
lock_dir=${CALF_CAPTURE_LOCK:-/tmp/calf-snapshot.lock}
lock_owner=$lock_dir/owner
fps_state=${CALF_CAPTURE_FPS_STATE:-/tmp/calf-capture-fps}
night_stack_count_file=${CALF_NIGHT_STACK_COUNT:-/tmp/calf-night-stack-count}
trace_file=${CALF_CAPTURE_TRACE_FILE:-/tmp/calf-capture-trace}
log_file=${CALF_CAPTURE_LOG:-/media/DCIM/calf-capture.log}
raw_enabled_file=${CALF_RAW_ENABLED_FILE:-/local/calf-raw-enabled}
raw_capture_dir=${CALF_RAW_CAPTURE_DIR:-/tmp/capture_image}
raw_count_c0=${CALF_RAW_COUNT_C0:-/tmp/.capture_cnt_c0}
raw_count_c1=${CALF_RAW_COUNT_C1:-/tmp/.capture_cnt_c1}
raw_converter=${CALF_RAW_CONVERTER:-/app/bin/calf-raw2dng}
raw_output_dir=${CALF_RAW_OUTPUT_DIR:-/mnt/mmcblk1p1/DCIM}
raw_job_dir=${CALF_RAW_JOB_DIR:-/tmp/calf-raw-jobs}
indicator_led_file=${CALF_INDICATOR_LED_FILE:-/local/calf-ui-indicator-led}
blue_led_trigger=${CALF_BLUE_LED_TRIGGER:-/sys/class/leds/led-blue/trigger}
blue_led_brightness=${CALF_BLUE_LED_BRIGHTNESS:-/sys/class/leds/led-blue/brightness}

cr=$(printf '\r')
while IFS= read -r request_line; do
    [ -z "$request_line" ] && break
    [ "$request_line" = "$cr" ] && break
done

respond()
{
    response_status=$1
    response_body=$2
    log_trace "stage=response status=$response_status body=$response_body"
    printf 'HTTP/1.1 %s\r\n' "$response_status"
    printf 'Content-Type: application/json\r\n'
    printf 'Content-Length: %s\r\n' "${#response_body}"
    printf 'Connection: close\r\n\r\n'
    printf '%s' "$response_body"
}

log_message()
{
    if [ -n "${capture_id:-}" ]; then
        printf 'capture[%s] ts=%s %s\n' \
            "$capture_id" "$(date +%s 2>/dev/null)" "$*" \
            >> "$log_file" 2>/dev/null
    else
        printf '%s\n' "$*" >> "$log_file" 2>/dev/null
    fi
}

log_trace()
{
    log_message "$*"
}

pulse_capture_led()
{
    indicator_mode=normal
    if [ -r "$indicator_led_file" ]; then
        read -r indicator_mode < "$indicator_led_file"
    fi
    [ "$indicator_mode" = stealth ] && return 0
    [ -w "$blue_led_trigger" ] || return 0
    [ -w "$blue_led_brightness" ] || return 0
    printf 'none\n' > "$blue_led_trigger" 2>/dev/null || return 0
    printf '255\n' > "$blue_led_brightness" 2>/dev/null || return 0
    (
        "$sleep_bin" 0.12
        printf '0\n' > "$blue_led_brightness" 2>/dev/null
    ) </dev/null >/dev/null 2>&1 &
}

newest_raw_dir()
{
    camera_number=$1
    ls -1dt "$raw_capture_dir"/Cam"$camera_number"-raw_* 2>/dev/null |
        sed -n '1p'
}

prepare_raw_capture()
{
    raw_capture_pending=0
    raw_preserve=0
    # The vendor RAW worker normally consumes these one-shot files.  Clear any
    # leftovers before every request so an earlier pre-capture failure cannot
    # turn an unrelated snapshot into a RAW capture.
    if ! rm -f "$raw_count_c0" "$raw_count_c1" 2>/dev/null ||
       [ -e "$raw_count_c0" ] || [ -e "$raw_count_c1" ]; then
        log_message "RAW capture skipped: stale trigger cleanup failed"
        return 1
    fi
    if [ -r "$raw_enabled_file" ]; then
        read -r raw_preserve < "$raw_enabled_file"
    fi
    [ "$raw_preserve" = 1 ] || raw_preserve=0
    # Multi-frame Night processing always needs temporary sensor RAW.  The
    # preference controls DNG publication only; ordinary photos still avoid
    # RAW capture unless JPEG + RAW is selected.
    if [ "$raw_preserve" -ne 1 ] &&
       [ "${night_stack_count:-0}" -le 0 ]; then
        return 0
    fi

    if ! mkdir -p "$raw_capture_dir"; then
        log_message "RAW capture skipped: capture directory unavailable"
        return 1
    fi
    raw_capture_free_kb=$(df -Pk "$raw_capture_dir" 2>/dev/null |
        awk 'NR == 2 { print $4 }')
    case "$raw_capture_free_kb" in
        ''|*[!0-9]*)
            log_message "RAW capture skipped: temporary storage availability unknown"
            return 1
            ;;
    esac
    raw_capture_required_kb=$((raw_capture_count * 32000 + 64000))
    if [ "$raw_capture_free_kb" -lt "$raw_capture_required_kb" ]; then
        log_message "RAW capture skipped: insufficient temporary space for $raw_capture_count frames"
        return 1
    fi
    if [ "$raw_preserve" -eq 1 ]; then
        raw_free_kb=$(df -Pk "$raw_output_dir" 2>/dev/null |
            awk 'NR == 2 { print $4 }')
        case "$raw_free_kb" in
            ''|*[!0-9]*)
                log_message "RAW capture skipped: output storage availability unknown"
                return 1
                ;;
        esac
        if [ "$raw_free_kb" -lt 100000 ]; then
            log_message "RAW capture skipped: less than 100 MB free for DNG output"
            return 1
        fi
        if [ ! -x "$raw_converter" ]; then
            log_message "RAW capture skipped: converter unavailable"
            return 1
        fi
    fi
    raw_old_c0=$(newest_raw_dir 0)
    raw_old_c1=$(newest_raw_dir 1)
    if ! printf '%s\n' "$raw_capture_count" > "$raw_count_c0" ||
       ! printf '%s\n' "$raw_capture_count" > "$raw_count_c1"; then
        rm -f "$raw_count_c0" "$raw_count_c1" 2>/dev/null
        log_message "RAW capture trigger failed"
        return 1
    fi
    raw_capture_pending=1
    return 0
}

safe_remove_raw_dir()
{
    candidate=$1
    case "$candidate" in
        "$raw_capture_dir"/Cam0-raw_*|"$raw_capture_dir"/Cam1-raw_*)
            if [ -d "$candidate" ] && [ ! -L "$candidate" ]; then
                rm -rf -- "$candidate"
            fi
            ;;
    esac
}

raw_conversion_slot()
{
    mkdir -p "$raw_job_dir" 2>/dev/null || return 1
    for raw_slot_number in 0 1; do
        raw_slot="$raw_job_dir/$raw_slot_number"
        if mkdir "$raw_slot" 2>/dev/null; then
            printf '%s\n' "$$" > "$raw_slot/owner" 2>/dev/null || {
                rmdir "$raw_slot" 2>/dev/null
                return 1
            }
            printf '%s' "$raw_slot"
            return 0
        fi
        raw_slot_owner=
        if [ -r "$raw_slot/owner" ]; then
            read -r raw_slot_owner < "$raw_slot/owner"
        fi
        case "$raw_slot_owner" in
            ''|*[!0-9]*) raw_slot_owner= ;;
        esac
        if [ -z "$raw_slot_owner" ] ||
           [ ! -d "/proc/$raw_slot_owner" ]; then
            rm -f "$raw_slot/owner" 2>/dev/null
            if rmdir "$raw_slot" 2>/dev/null &&
               mkdir "$raw_slot" 2>/dev/null; then
                printf '%s\n' "$$" > "$raw_slot/owner" 2>/dev/null || {
                    rmdir "$raw_slot" 2>/dev/null
                    return 1
                }
                printf '%s' "$raw_slot"
                return 0
            fi
        fi
    done
    return 1
}

convert_raw_capture()
{
    if ! "$raw_converter" "$raw_new_c0"/frame*_normal.raw \
            "$raw_new_c0/meta_data" \
            "$raw_left_temp" >> "$log_file" 2>&1 ||
       ! "$raw_converter" "$raw_new_c1"/frame*_normal.raw \
            "$raw_new_c1/meta_data" \
            "$raw_right_temp" >> "$log_file" 2>&1 ||
       ! mv -f "$raw_left_temp" "$raw_left" ||
       ! mv -f "$raw_right_temp" "$raw_right"; then
        rm -f "$raw_left_temp" "$raw_right_temp"
        log_message "RAW-to-DNG conversion failed for $raw_stem"
        return 1
    fi
    safe_remove_raw_dir "$raw_new_c0"
    safe_remove_raw_dir "$raw_new_c1"
    log_message "saved $raw_capture_count synchronized RAW frames as stacked $raw_stem-L/R.dng"
    return 0
}

queue_raw_conversion()
{
    raw_slot=$(raw_conversion_slot) || {
        log_message "RAW conversion queue full; completing this pair inline"
        convert_raw_capture
        return $?
    }
    (
        trap 'rm -f "$raw_slot/owner" 2>/dev/null; rmdir "$raw_slot" 2>/dev/null' EXIT
        convert_raw_capture
    ) </dev/null >/dev/null 2>&1 &
    raw_job_pid=$!
    if ! printf '%s\n' "$raw_job_pid" > "$raw_slot/owner" 2>/dev/null; then
        wait "$raw_job_pid"
        return $?
    fi
    return 0
}

finish_raw_capture()
{
    [ "${raw_capture_pending:-0}" -eq 1 ] || return 0
    raw_wait=0
    raw_new_c0=
    raw_new_c1=
    raw_count_found_c0=0
    raw_count_found_c1=0
    while [ "$raw_wait" -lt 80 ]; do
        raw_new_c0=$(newest_raw_dir 0)
        raw_new_c1=$(newest_raw_dir 1)
        if [ -n "$raw_new_c0" ] && [ -n "$raw_new_c1" ] &&
           [ "$raw_new_c0" != "$raw_old_c0" ] &&
           [ "$raw_new_c1" != "$raw_old_c1" ] &&
            [ -f "$raw_new_c0/meta_data" ] &&
           [ -f "$raw_new_c1/meta_data" ]; then
            set -- "$raw_new_c0"/frame*_normal.raw
            raw_count_found_c0=$#
            set -- "$raw_new_c1"/frame*_normal.raw
            raw_count_found_c1=$#
            if [ "$raw_count_found_c0" -eq "$raw_capture_count" ] &&
               [ "$raw_count_found_c1" -eq "$raw_capture_count" ]; then
                break
            fi
        fi
        "$sleep_bin" 0.1
        raw_wait=$((raw_wait + 1))
    done
    if [ "$raw_count_found_c0" -ne "$raw_capture_count" ] ||
       [ "$raw_count_found_c1" -ne "$raw_capture_count" ]; then
        log_message "RAW capture timed out waiting for $raw_capture_count frames from both sensors"
        return 1
    fi

    raw_frame_ids_c0=
    raw_frame_ids_c1=
    for raw_file in "$raw_new_c0"/frame*_normal.raw; do
        raw_frame=$(basename "$raw_file" | sed -n 's/^frame\([0-9][0-9]*\)_.*/\1/p')
        [ -n "$raw_frame" ] || return 1
        raw_frame_ids_c0="${raw_frame_ids_c0}${raw_frame}:"
    done
    for raw_file in "$raw_new_c1"/frame*_normal.raw; do
        raw_frame=$(basename "$raw_file" | sed -n 's/^frame\([0-9][0-9]*\)_.*/\1/p')
        [ -n "$raw_frame" ] || return 1
        raw_frame_ids_c1="${raw_frame_ids_c1}${raw_frame}:"
    done
    if [ "$raw_frame_ids_c0" != "$raw_frame_ids_c1" ]; then
        log_message "RAW capture rejected: left/right frame sequences differ"
        return 1
    fi

    if [ "$raw_preserve" -ne 1 ]; then
        raw_capture_pending=0
        safe_remove_raw_dir "$raw_new_c0"
        safe_remove_raw_dir "$raw_new_c1"
        log_message "discarded temporary $raw_capture_count-frame RAW stack after JPEG"
        return 0
    fi

    raw_jpeg=$(printf '%s' "$snapshot_response" |
        sed -n 's/.*"filename"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
    raw_jpeg=${raw_jpeg##*/}
    case "$raw_jpeg" in
        *[!A-Za-z0-9_.-]*|'')
            log_message "RAW capture rejected: snapshot filename is unsafe"
            return 1
            ;;
        *.[Jj][Pp][Gg]) raw_stem=${raw_jpeg%.*} ;;
        *.[Jj][Pp][Ee][Gg]) raw_stem=${raw_jpeg%.*} ;;
        *)
            log_message "RAW capture rejected: snapshot filename is not JPEG"
            return 1
            ;;
    esac

    raw_jpeg_dir=
    raw_jpeg_matches=0
    for candidate_dir in "$raw_output_dir" "$raw_output_dir"/*; do
        if [ -d "$candidate_dir" ] && [ ! -L "$candidate_dir" ] &&
           [ -f "$candidate_dir/$raw_jpeg" ]; then
            raw_jpeg_dir=$candidate_dir
            raw_jpeg_matches=$((raw_jpeg_matches + 1))
        fi
    done
    if [ "$raw_jpeg_matches" -ne 1 ]; then
        log_message "RAW capture rejected: matching JPEG directory is ambiguous"
        return 1
    fi

    raw_left_temp="$raw_jpeg_dir/$raw_stem-L.dng.tmp"
    raw_right_temp="$raw_jpeg_dir/$raw_stem-R.dng.tmp"
    raw_left="$raw_jpeg_dir/$raw_stem-L.dng"
    raw_right="$raw_jpeg_dir/$raw_stem-R.dng"
    raw_capture_pending=0
    queue_raw_conversion
}

profile_value()
{
    awk -v wanted="$1" '
        /^image_params:[[:space:]]*$/ { in_section = 1; next }
        in_section && /^[^[:space:]]/ { exit }
        in_section {
            line = $0
            sub(/^[[:space:]]+/, "", line)
            if (index(line, wanted ":") == 1) {
                sub(/^[^:]*:[[:space:]]*/, "", line)
                print line
                exit
            }
        }
    ' "$profile" 2>/dev/null | tr -d "\"'"
}

post_json()
{
    post_timeout=${3:-25}
    post_response=$(
        "$curl_bin" -sS --connect-timeout 2 --max-time "$post_timeout" \
            -H 'Content-Type: application/json' \
            --data "$2" "$1" 2>/dev/null
    )
    post_result=$?
    if [ "$post_result" -eq 0 ] &&
       printf '%s' "$post_response" |
           grep -q '"code"[[:space:]]*:[[:space:]]*0'; then
        post_result=0
        return 0
    fi
    post_result=1
    return 1
}

apply_image_params()
{
    requested_exp=${1:-$capture_exp}
    image_params_attempt=1
    while [ "$image_params_attempt" -le 3 ]; do
        if post_json "$api/imgparams" \
                "{\"type\":\"exp\",\"value\":\"$requested_exp\"}" &&
           post_json "$api/imgparams" \
                "{\"type\":\"iso\",\"value\":\"$iso\"}"; then
            return 0
        fi
        log_trace "stage=image-params-request result=failed attempt=$image_params_attempt exp=$requested_exp iso=$iso response=$post_response"
        if [ "$image_params_attempt" -lt 3 ]; then
            "$sleep_bin" 0.2
        fi
        image_params_attempt=$((image_params_attempt + 1))
    done
    return 1
}

restore_sensor_timing()
{
    restore_success=0
    restore_attempt=1
    rm -f "$fps_state"
    log_trace "stage=restore begin fps=30"
    while [ "$restore_attempt" -le 2 ]; do
        log_trace "stage=restore attempt=$restore_attempt"
        if "$sensor_timing" 30 >> "$log_file" 2>&1; then
            restore_success=1
            log_trace "stage=restore timing=ok attempt=$restore_attempt"
            break
        fi
        log_message "30 fps sensor-timing restore attempt $restore_attempt failed"
        "$sleep_bin" 1
        restore_attempt=$((restore_attempt + 1))
    done
    if [ "$restore_success" -eq 1 ]; then
        "$sleep_bin" 1
        if apply_image_params "$exp" >/dev/null 2>&1; then
            log_trace "stage=restore image-params=ok exp=$exp iso=$iso"
        else
            log_trace "stage=restore image-params=failed exp=$exp iso=$iso response=$post_response"
        fi
    else
        log_trace "stage=restore timing=failed"
    fi
    restore_needed=0
}

restore_before_response()
{
    if [ "${restore_needed:-0}" -eq 1 ]; then
        restore_sensor_timing
    fi
}

recover_slow_image_pipeline()
{
    log_trace "stage=image-recovery begin"
    restore_sensor_timing
    log_trace "stage=image-recovery graph-stop begin"
    if ! post_json "$api/cameramode" '{"action":"stop"}' 30; then
        log_trace "stage=image-recovery graph-stop failed response=$post_response"
        return 1
    fi
    log_trace "stage=image-recovery graph-start begin"
    graph_start_attempt=1
    graph_start_success=0
    while [ "$graph_start_attempt" -le 2 ]; do
        if post_json "$api/cameramode" \
                '{"action":"start","mode":"VR180_PIC"}' 30; then
            graph_start_success=1
            break
        fi
        log_trace "stage=image-recovery graph-start attempt=$graph_start_attempt failed response=$post_response"
        "$sleep_bin" 0.5
        graph_start_attempt=$((graph_start_attempt + 1))
    done
    if [ "$graph_start_success" -ne 1 ]; then
        return 1
    fi
    if ! printf '%s\n' "$capture_fps" > "$fps_state"; then
        log_trace "stage=image-recovery fps-state failed"
        return 1
    fi
    restore_needed=1
    if ! "$sensor_timing" "$capture_fps" >> "$log_file" 2>&1; then
        log_trace "stage=image-recovery sensor-timing failed fps=$capture_fps"
        return 1
    fi
    if ! apply_image_params "$capture_exp"; then
        log_trace "stage=image-recovery image-params failed response=$post_response"
        return 1
    fi
    log_trace "stage=image-recovery result=ok"
    return 0
}

cleanup()
{
    rm -f "$night_stack_count_file" 2>/dev/null
    rm -f "$raw_count_c0" "$raw_count_c1" 2>/dev/null
    if [ "${raw_capture_pending:-0}" -eq 1 ]; then
        raw_cleanup_c0=${raw_new_c0:-}
        raw_cleanup_c1=${raw_new_c1:-}
        [ -n "$raw_cleanup_c0" ] || raw_cleanup_c0=$(newest_raw_dir 0)
        [ -n "$raw_cleanup_c1" ] || raw_cleanup_c1=$(newest_raw_dir 1)
        if [ -n "$raw_cleanup_c0" ] &&
           [ "$raw_cleanup_c0" != "${raw_old_c0:-}" ]; then
            safe_remove_raw_dir "$raw_cleanup_c0"
        fi
        if [ -n "$raw_cleanup_c1" ] &&
           [ "$raw_cleanup_c1" != "${raw_old_c1:-}" ]; then
            safe_remove_raw_dir "$raw_cleanup_c1"
        fi
        raw_capture_pending=0
    fi
    if [ "${restore_needed:-0}" -eq 1 ]; then
        restore_sensor_timing
    fi
    log_trace "stage=cleanup complete"
    rm -f "$trace_file" 2>/dev/null
    rm -f "$lock_owner" 2>/dev/null
    rmdir "$lock_dir" 2>/dev/null
}

acquire_lock()
{
    if mkdir "$lock_dir" 2>/dev/null; then
        printf '%s\n' "$$" > "$lock_owner" || {
            rmdir "$lock_dir" 2>/dev/null
            return 1
        }
        return 0
    fi

    previous_owner=
    if [ -r "$lock_owner" ]; then
        read -r previous_owner < "$lock_owner"
    fi
    case "$previous_owner" in
        ''|*[!0-9]*) ;;
        *)
            if kill -0 "$previous_owner" 2>/dev/null; then
                return 1
            fi
            ;;
    esac

    rm -f "$lock_owner" 2>/dev/null
    rmdir "$lock_dir" 2>/dev/null || return 1
    mkdir "$lock_dir" 2>/dev/null || return 1
    printf '%s\n' "$$" > "$lock_owner" || {
        rmdir "$lock_dir" 2>/dev/null
        return 1
    }
    log_message "reclaimed stale capture lock"
    return 0
}

capture_id="$(date +%s 2>/dev/null)-$$"
if ! acquire_lock; then
    log_trace "stage=lock result=busy"
    respond '503 Service Unavailable' '{"code":-1,"message":"capture busy"}'
    exit 0
fi
restore_needed=0
trap cleanup EXIT
trap 'exit 0' HUP INT TERM PIPE
if ! printf '%s\n' "$capture_id" > "$trace_file"; then
    respond '500 Internal Server Error' \
        "{\"code\":-1,\"message\":\"trace setup failed\",\"trace\":\"$capture_id\"}"
    exit 0
fi
log_trace "stage=request accepted"

exp=$(profile_value exp)
iso=$(profile_value iso)
[ -n "$exp" ] || exp=-1
[ -n "$iso" ] || iso=auto
capture_exp=$exp
raw_capture_count=1
night_stack_count=0

case "$exp" in
    -1) capture_fps=30 ;;
    12) capture_fps=2; settle_seconds=2; capture_exp=0.5; raw_capture_count=24; night_stack_count=24 ;;
    8) capture_fps=2; settle_seconds=2; capture_exp=0.5; raw_capture_count=16; night_stack_count=16 ;;
    4) capture_fps=2; settle_seconds=2; capture_exp=0.5; raw_capture_count=8; night_stack_count=8 ;;
    2) capture_fps=2; settle_seconds=2; capture_exp=0.5; raw_capture_count=4; night_stack_count=4 ;;
    1) capture_fps=2; settle_seconds=2; capture_exp=0.5; raw_capture_count=2; night_stack_count=2 ;;
    0.5) capture_fps=2; settle_seconds=2 ;;
    0.25) capture_fps=4; settle_seconds=1 ;;
    0.125) capture_fps=8; settle_seconds=1 ;;
    0.0666667) capture_fps=15; settle_seconds=1 ;;
    0.0333333|0.0166667|0.008|0.004|0.001|0.00025)
        capture_fps=30
        ;;
    *)
        log_message "invalid EXP profile value; using AUTO"
        exp=-1
        capture_fps=30
        ;;
esac

case "$iso" in
    auto|iso100|iso200|iso400|iso800|iso1600|iso3200|iso6400|iso12800) ;;
    *)
        log_message "invalid ISO profile value; using AUTO"
        iso=auto
        ;;
esac
log_trace "stage=profile exp=$exp iso=$iso capture_exp=$capture_exp fps=$capture_fps stack=$night_stack_count raw_count=$raw_capture_count"

if [ "$capture_fps" = 30 ]; then
    log_trace "stage=raw-prepare begin"
    if ! prepare_raw_capture; then
        respond '507 Insufficient Storage' \
            '{"code":-1,"message":"RAW capture unavailable"}'
        exit 0
    fi
    log_trace "stage=backend-snapshot begin"
    pulse_capture_led
    post_json "$api/snapshot" '{}' 90
    log_trace "stage=backend-snapshot result=$post_result response=$post_response"
    if [ "$post_result" -eq 0 ]; then
        snapshot_response=$post_response
        finish_raw_capture || log_message "JPEG saved but RAW companion failed"
        respond '200 OK' "$post_response"
    else
        respond '502 Bad Gateway' '{"code":-1,"message":"snapshot failed"}'
    fi
    exit 0
fi

log_message "capture setup: effective_exp=$exp sub_exp=$capture_exp iso=$iso interval=$capture_fps frames=$raw_capture_count"
printf '%s\n' "$capture_fps" > "$fps_state"
restore_needed=1

log_trace "stage=sensor-timing begin fps=$capture_fps"
if ! "$sensor_timing" "$capture_fps" >> "$log_file" 2>&1; then
    restore_before_response
    respond '502 Bad Gateway' '{"code":-1,"message":"sensor timing setup failed"}'
    exit 0
fi
log_trace "stage=sensor-timing result=ok fps=$capture_fps"
# Apply manual exposure only after the longer frame interval exists. AIQ
# otherwise accepts the requested range while its applied value remains
# clamped to the 30 fps preview interval.
log_trace "stage=image-params begin exp=$capture_exp iso=$iso"
if ! apply_image_params "$capture_exp"; then
    log_trace "stage=image-params result=failed response=$post_response"
    if ! recover_slow_image_pipeline; then
        restore_before_response
        respond '502 Bad Gateway' \
            '{"code":-1,"message":"exposure setup failed"}'
        exit 0
    fi
    log_trace "stage=image-params result=recovered"
else
    log_trace "stage=image-params result=ok"
fi
"$sleep_bin" "$settle_seconds"
log_trace "stage=settle complete seconds=$settle_seconds"

if [ "$night_stack_count" -gt 0 ] &&
   ! printf '%s\n' "$night_stack_count" > "$night_stack_count_file"; then
    restore_before_response
    respond '502 Bad Gateway' '{"code":-1,"message":"stack setup failed"}'
    exit 0
fi
log_trace "stage=stack-flag count=$night_stack_count"
log_trace "stage=raw-prepare begin"
if ! prepare_raw_capture; then
    restore_before_response
    respond '507 Insufficient Storage' \
        '{"code":-1,"message":"RAW capture unavailable"}'
    exit 0
fi
log_trace "stage=raw-prepare result=ok pending=$raw_capture_pending preserve=$raw_preserve"
pulse_capture_led
log_trace "stage=backend-snapshot begin"
post_json "$api/snapshot" '{}' 90
snapshot_result=$post_result
snapshot_response=$post_response
log_trace "stage=backend-snapshot result=$snapshot_result response=$snapshot_response"
restore_sensor_timing

if [ "$snapshot_result" -eq 0 ] && [ "$restore_success" -eq 1 ]; then
    finish_raw_capture || log_message "JPEG saved but RAW companion failed"
    respond '200 OK' "$snapshot_response"
elif [ "$restore_success" -ne 1 ]; then
    respond '502 Bad Gateway' '{"code":-1,"message":"30 fps restore failed"}'
else
    respond '502 Bad Gateway' '{"code":-1,"message":"snapshot failed"}'
fi
"""


def build_capture_server(destination: Path) -> dict[str, object]:
    destination.write_text(CAPTURE_SERVER, encoding="ascii")
    return {
        "target": CAPTURE_SERVER_FILESYSTEM_PATH,
        "runtime_path": CAPTURE_SERVER_RUNTIME_PATH,
        "sha256": sha256(destination),
        "description": (
            "Run a fail-open localhost snapshot coordinator on port 8990 "
            "using the factory BusyBox nc applet."
        ),
    }


def build_capture_request(destination: Path) -> dict[str, object]:
    destination.write_text(CAPTURE_REQUEST, encoding="ascii")
    return {
        "target": CAPTURE_REQUEST_FILESYSTEM_PATH,
        "runtime_path": CAPTURE_REQUEST_RUNTIME_PATH,
        "sha256": sha256(destination),
        "description": (
            "Serialize snapshots; temporarily apply coordinated sensor and "
            "XVS timing through 15 fps for manual captures; make 1/2/4/8/12-second "
            "Night shots from synchronized 2/4/8/16/24-frame half-second stacks; "
            "optionally preserve stacked left/right DNG companions; keep AUTO "
            "at 30 fps; and restore the 30 fps preview in a two-attempt cleanup "
            "path."
        ),
    }
