#include "target_internal.h"

static int capture_text_equal(const char *left, const char *right)
{
    size_t index = 0;
    while(left[index] != '\0' && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

int night_preview_fps_for_exposure(const char *exposure)
{
    if(capture_text_equal(exposure, "0.0666667")) return 15;
    if(capture_text_equal(exposure, "0.125")) return 8;
    if(capture_text_equal(exposure, "0.25") ||
       capture_text_equal(exposure, "0.5") ||
       capture_text_equal(exposure, "1") ||
       capture_text_equal(exposure, "2") ||
       capture_text_equal(exposure, "4") ||
       capture_text_equal(exposure, "8") ||
       capture_text_equal(exposure, "12"))
        return 4;
    return 0;
}

const char *night_preview_exposure_for_fps(int fps)
{
    if(fps == 15) return "0.0666667";
    if(fps == 8) return "0.125";
    /* Stay below the exact 250 ms frame boundary. The 50 Hz AIQ path applies
     * 240 ms (25200 lines) in practice; requesting that value explicitly
     * avoids an intermittent range rejection during a live 8/15 -> 4 fps
     * transition while the real capture value remains unchanged. */
    if(fps == 4) return "0.24";
    return (const char *)0;
}

int night_image_action_is_transient(calf_capture_mode_t mode,
                                    calf_action_kind_t kind)
{
    return mode == CALF_CAPTURE_NIGHT &&
           (kind == CALF_ACTION_SET_EXPOSURE ||
            kind == CALF_ACTION_SET_ISO);
}

void capture_sequence_init(capture_sequence_t *sequence)
{
    sequence->active = 0;
    sequence->interval = 0;
    sequence->burst = 0;
    sequence->sleeping = 0;
    sequence->deep_idle_enabled = 0;
    sequence->overrun = 0;
    sequence->interval_seconds = 0;
    sequence->shot_limit = 0;
    sequence->shot_count = 0;
    sequence->next_capture_ms = 0;
}

int capture_sequence_start(capture_sequence_t *sequence,
                           int drive_mode_index, uint64_t now_ms)
{
    int seconds;
    if(drive_mode_index <= 0 ||
       drive_mode_index >= (int)calf_drive_mode_count())
        return -1;
    seconds = calf_drive_mode_delay_seconds((size_t)drive_mode_index);
    if(seconds <= 0 && !calf_drive_mode_is_burst(
                           (size_t)drive_mode_index))
        return -1;
    capture_sequence_init(sequence);
    sequence->active = 1;
    sequence->interval = calf_drive_mode_is_interval(
        (size_t)drive_mode_index);
    sequence->burst = calf_drive_mode_is_burst((size_t)drive_mode_index);
    sequence->shot_limit = calf_drive_mode_shot_limit(
        (size_t)drive_mode_index);
    sequence->deep_idle_enabled = sequence->interval && seconds >= 10;
    sequence->interval_seconds = (unsigned)seconds;
    sequence->next_capture_ms = sequence->interval || sequence->burst
                                    ? now_ms
                                    : now_ms + (uint64_t)(unsigned)seconds *
                                                   1000u;
    return 0;
}

void capture_sequence_cancel(capture_sequence_t *sequence)
{
    capture_sequence_init(sequence);
}

int capture_sequence_capture_due(const capture_sequence_t *sequence,
                                 uint64_t now_ms)
{
    return sequence->active && !sequence->sleeping &&
           now_ms >= sequence->next_capture_ms;
}

int capture_sequence_should_sleep(const capture_sequence_t *sequence,
                                  uint64_t now_ms)
{
    uint64_t minimum_window =
        CAPTURE_SEQUENCE_WAKE_MARGIN_MS + CAPTURE_SEQUENCE_MIN_SLEEP_MS;
    return sequence->active && sequence->interval &&
           sequence->deep_idle_enabled && !sequence->sleeping &&
           sequence->next_capture_ms > now_ms + minimum_window;
}

int capture_sequence_should_wake(const capture_sequence_t *sequence,
                                 uint64_t now_ms)
{
    return sequence->active && sequence->sleeping &&
           now_ms + CAPTURE_SEQUENCE_WAKE_MARGIN_MS >=
               sequence->next_capture_ms;
}

void capture_sequence_set_sleeping(capture_sequence_t *sequence,
                                   int sleeping)
{
    if(sequence->active) sequence->sleeping = sleeping != 0;
}

void capture_sequence_complete_capture(capture_sequence_t *sequence,
                                       int success, uint64_t completed_ms)
{
    uint64_t interval_ms;
    uint64_t first_next;
    if(!sequence->active) return;
    if(!success) {
        capture_sequence_cancel(sequence);
        return;
    }
    ++sequence->shot_count;
    if(sequence->burst) {
        if(sequence->shot_count >= sequence->shot_limit) {
            sequence->active = 0;
            sequence->sleeping = 0;
        }
        else {
            sequence->next_capture_ms = completed_ms;
        }
        return;
    }
    if(!sequence->interval) {
        sequence->active = 0;
        sequence->sleeping = 0;
        return;
    }
    interval_ms = (uint64_t)sequence->interval_seconds * 1000u;
    first_next = sequence->next_capture_ms + interval_ms;
    sequence->overrun = completed_ms >= first_next;
    sequence->next_capture_ms = first_next;
    while(sequence->next_capture_ms <= completed_ms)
        sequence->next_capture_ms += interval_ms;
}

int capture_sequence_remaining_seconds(const capture_sequence_t *sequence,
                                       uint64_t now_ms)
{
    uint64_t remaining;
    if(!sequence->active || now_ms >= sequence->next_capture_ms) return 0;
    remaining = sequence->next_capture_ms - now_ms;
    return (int)((remaining + 999u) / 1000u);
}
