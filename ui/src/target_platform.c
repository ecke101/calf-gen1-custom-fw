#include "target_internal.h"

static uint32_t g_overlay_staging[CALF_UI_WIDTH * CALF_UI_HEIGHT];

static int api_get_graphics_id(void)
{
    char response[HTTP_BUFFER_SIZE];
    int identifier = -1;
    if(http_request("GET", "/camera/v2/graphctrlmb", (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response) ||
       parse_integer_after(response, "\"ctrlmb\"", &identifier) != 0)
        return -1;
    return identifier;
}

int display_open(display_t *display)
{
    int attempt;
    int control_id = -1;
    int pixel_id;
    size_t required_bytes = (size_t)CALF_UI_WIDTH * CALF_UI_HEIGHT * 4;
    display->control = (volatile uint32_t *)0;
    display->pixels = (uint32_t *)0;
    display->pixel_bytes = 0;
    display->control_fd = -1;
    display->pixel_fd = -1;
    /*
     * ngmonitor starts ngcd and ngui concurrently, so the HTTP endpoint and
     * DRM unique-ID import are not necessarily ready in the same instant.
     * Retry the complete handoff, not just the HTTP request.  The smaller
     * replacement reaches this point much sooner than the stock UI.
     */
    for(attempt = 0;
        attempt < 100 && display->control_fd < 0 && g_running;
        ++attempt) {
        control_id = api_get_graphics_id();
        if(control_id >= 0)
            display->control_fd = RK_MPI_MB_UniqueId2Fd(control_id);
        if(display->control_fd < 0) usleep(100000);
    }
    if(display->control_fd < 0) return -1;
    display->control = (volatile uint32_t *)mmap((void *)0, 24,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            display->control_fd, 0);
    if((void *)display->control == MAP_FAILED) {
        display->control = (volatile uint32_t *)0;
        return -1;
    }
    for(attempt = 0; attempt < 100 && g_running; ++attempt) {
        if(display->control[2] == CALF_UI_HEIGHT &&
           display->control[3] == CALF_UI_WIDTH &&
           display->control[5] >= required_bytes)
            break;
        usleep(100000);
    }
    if(display->control[2] != CALF_UI_HEIGHT ||
       display->control[3] != CALF_UI_WIDTH ||
       display->control[5] < required_bytes)
        return -1;
    pixel_id = (int)display->control[4];
    display->pixel_bytes = display->control[5];
    for(attempt = 0;
        attempt < 100 && display->pixel_fd < 0 && g_running;
        ++attempt) {
        display->pixel_fd = RK_MPI_MB_UniqueId2Fd(pixel_id);
        if(display->pixel_fd < 0) usleep(100000);
    }
    if(display->pixel_fd < 0) return -1;
    display->pixels = (uint32_t *)mmap((void *)0, display->pixel_bytes,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            display->pixel_fd, 0);
    if((void *)display->pixels == MAP_FAILED) {
        display->pixels = (uint32_t *)0;
        return -1;
    }
    return 0;
}

void display_present(display_t *display, const calf_ui_t *ui)
{
    /*
     * The backend can still be scanning the previously submitted DMA buffer.
     * Do the comparatively slow software render off-screen, then keep the
     * shared-buffer update window to one contiguous copy before publishing a
     * new generation.
     */
    calf_ui_render(ui, g_overlay_staging, CALF_UI_WIDTH);
    (void)memcpy(display->pixels, g_overlay_staging,
                 (size_t)CALF_UI_WIDTH * CALF_UI_HEIGHT * sizeof(uint32_t));
    __sync_synchronize();
    display->control[1] = display->control[1] + 1;
}

void display_clear(display_t *display)
{
    size_t pixel_count = (size_t)CALF_UI_WIDTH * CALF_UI_HEIGHT;
    size_t i;
    if(display->pixels == (uint32_t *)0 || display->control == (volatile uint32_t *)0)
        return;
    for(i = 0; i < pixel_count; ++i) display->pixels[i] = 0;
    display->control[1] = display->control[1] + 1;
}

void display_close(display_t *display)
{
    if(display->pixels != (uint32_t *)0)
        (void)munmap(display->pixels, display->pixel_bytes);
    if(display->control != (volatile uint32_t *)0)
        (void)munmap((void *)display->control, 24);
    if(display->pixel_fd >= 0) (void)close(display->pixel_fd);
    if(display->control_fd >= 0) (void)close(display->control_fd);
}

static void path_for_event(char *path, int event_number)
{
    size_t used = 0;
    path[0] = '\0';
    buffer_append(path, 32, &used, "/dev/input/event");
    buffer_append_uint(path, 32, &used, (unsigned)event_number);
}

static unsigned int elapsed_milliseconds(const struct timeval *newer,
                                         const struct timeval *older)
{
    long seconds = newer->seconds - older->seconds;
    long microseconds = newer->microseconds - older->microseconds;
    long milliseconds;
    if(microseconds < 0) {
        --seconds;
        microseconds += 1000000;
    }
    if(seconds < 0) return 0;
    if(seconds > 4294967) return 0xffffffffu;
    milliseconds = seconds * 1000 + microseconds / 1000;
    return (unsigned int)milliseconds;
}

int touch_open(touch_t *touch)
{
    int event_number;
    touch->fd = -1;
    touch->x = 0;
    touch->y = 0;
    touch->pressed = 0;
    touch->dirty = 0;
    touch->wake_consumed = 0;
    touch->tap_armed = 0;
    touch->last_tap_valid = 0;
    for(event_number = 0; event_number < 16; ++event_number) {
        char path[32];
        struct input_absinfo slot_info;
        int descriptor;
        path_for_event(path, event_number);
        descriptor = open(path, O_RDONLY | O_NONBLOCK);
        if(descriptor < 0) continue;
        if(ioctl(descriptor, EVIOCGABS_MT_SLOT, &slot_info) == 0) {
            touch->fd = descriptor;
            return 0;
        }
        close(descriptor);
    }
    return -1;
}

int touch_read_action(touch_t *touch, calf_ui_t *ui,
                      calf_action_t *action, int suppress_actions,
                      int allow_wake, int *activity)
{
    struct input_event event;
    int emitted = 0;
    int wake_requested = 0;
    action->kind = CALF_ACTION_NONE;
    action->value = (const char *)0;
    action->selection = -1;
    while(read(touch->fd, &event, sizeof(event)) == (ssize_t)sizeof(event)) {
        int this_activity = 0;
        if(event.type == EV_ABS && event.code == ABS_MT_POSITION_X) {
            touch->x = event.value;
            touch->dirty = 1;
            *activity = 1;
            this_activity = 1;
        }
        else if(event.type == EV_ABS && event.code == ABS_MT_POSITION_Y) {
            touch->y = event.value;
            touch->dirty = 1;
            *activity = 1;
            this_activity = 1;
        }
        else if(event.type == EV_ABS && event.code == ABS_MT_TRACKING_ID) {
            if(event.value >= 0 && !touch->pressed)
                touch->tap_armed = 1;
            touch->pressed = event.value >= 0;
            touch->dirty = 1;
            *activity = 1;
            this_activity = 1;
        }
        else if(event.type == EV_KEY && event.code == BTN_TOUCH) {
            if(event.value == 1 && !touch->pressed)
                touch->tap_armed = 1;
            touch->pressed = event.value == 1;
            touch->dirty = 1;
            *activity = 1;
            this_activity = 1;
        }
        else if(event.type == EV_SYN && event.code == SYN_REPORT && touch->dirty) {
            if(!touch->pressed && touch->wake_consumed) {
                touch->wake_consumed = 0;
                touch->tap_armed = 0;
            }
            else if(!touch->pressed && touch->tap_armed &&
               touch->x >= 0 && touch->x < CALF_UI_WIDTH &&
               touch->y >= 0 && touch->y < CALF_UI_HEIGHT) {
                int debounced = touch->last_tap_valid &&
                    elapsed_milliseconds(&event.time, &touch->last_tap) <
                        INPUT_DEBOUNCE_MS;
                touch->tap_armed = 0;
                if(!debounced) {
                    calf_action_t candidate = calf_ui_tap(
                        ui, touch->x, touch->y);
                    touch->last_tap = event.time;
                    touch->last_tap_valid = 1;
                    if(candidate.kind != CALF_ACTION_NONE)
                        *action = candidate;
                    emitted = 1;
                }
            }
            touch->dirty = 0;
        }
        if(suppress_actions && this_activity) {
            touch->wake_consumed = 1;
            wake_requested = 1;
        }
    }
    if(wake_requested && allow_wake) {
        calf_action_t candidate = calf_ui_key_press(ui, CALF_KEY_POWER);
        if(candidate.kind != CALF_ACTION_NONE)
            *action = candidate;
        emitted = 1;
    }
    return emitted;
}

int keys_open(keys_t *keys)
{
    int event_number;
    keys->count = 0;
    keys->power_pressed = 0;
    keys->pressed_keys = 0U;
    keys->timed_keys = 0U;
    keys->button_release_valid = 0;
    for(event_number = 0; event_number < MAX_EVENT_INPUTS; ++event_number) {
        char path[32];
        int descriptor;
        path_for_event(path, event_number);
        descriptor = open(path, O_RDONLY | O_NONBLOCK);
        if(descriptor >= 0)
            keys->descriptors[keys->count++] = descriptor;
    }
    return keys->count;
}

static int power_hold_milliseconds(const keys_t *keys,
                                   const struct input_event *event)
{
    long seconds = event->time.seconds - keys->power_down.seconds;
    long microseconds = event->time.microseconds - keys->power_down.microseconds;
    return (int)(seconds * 1000 + microseconds / 1000);
}

static int key_from_linux_code(uint16_t code, calf_key_t *key)
{
    if(code == KEY_UP) *key = CALF_KEY_UP;
    else if(code == KEY_DOWN) *key = CALF_KEY_DOWN;
    else if(code == KEY_LEFT) *key = CALF_KEY_LEFT;
    else if(code == KEY_RIGHT) *key = CALF_KEY_RIGHT;
    else if(code == KEY_MENU) *key = CALF_KEY_MENU;
    else if(code == KEY_BACK) *key = CALF_KEY_BACK;
    else if(code == KEY_RECORD) *key = CALF_KEY_SHUTTER;
    else if(code == KEY_FILE) *key = CALF_KEY_FILE;
    else if(code == KEY_F1) *key = CALF_KEY_F1;
    else if(code == KEY_F2) *key = CALF_KEY_F2;
    else return 0;
    return 1;
}

int keys_read_action(keys_t *keys, int descriptor, calf_ui_t *ui,
                     calf_action_t *action, int suppress_actions,
                     int allow_wake, int *short_power, int *long_power,
                     int *activity)
{
    struct input_event event;
    int emitted = 0;
    action->kind = CALF_ACTION_NONE;
    action->value = (const char *)0;
    action->selection = -1;
    while(read(descriptor, &event, sizeof(event)) == (ssize_t)sizeof(event)) {
        calf_key_t key;
        if(event.type != EV_KEY) continue;
        if(event.code == KEY_POWER) {
            *activity = 1;
            if(event.value == 1) {
                int quiet = !keys->button_release_valid ||
                    elapsed_milliseconds(&event.time,
                                         &keys->last_button_release) >=
                        PHYSICAL_BUTTON_QUIET_MS;
                if(quiet && keys->pressed_keys == 0U &&
                   !keys->power_pressed) {
                    keys->power_pressed = 1;
                    keys->power_down = event.time;
                }
            }
            else if(event.value == 0 &&
                    (keys->power_pressed ||
                     (suppress_actions && !allow_wake))) {
                int held = keys->power_pressed
                               ? power_hold_milliseconds(keys, &event) : 0;
                keys->power_pressed = 0;
                if(held >= 2000) {
                    *long_power = 1;
                    emitted = 1;
                }
                else {
                    *short_power = 1;
                    emitted = 1;
                }
                keys->last_button_release = event.time;
                keys->button_release_valid = 1;
            }
            else if(event.value == 0) {
                keys->last_button_release = event.time;
                keys->button_release_valid = 1;
            }
        }
        else if(key_from_linux_code(event.code, &key)) {
            uint32_t bit = UINT32_C(1) << (unsigned int)key;
            *activity = 1;
            if(event.value == 0) {
                keys->pressed_keys &= ~bit;
                keys->last_button_release = event.time;
                keys->button_release_valid = 1;
            }
            else if(event.value == 1 && !(keys->pressed_keys & bit)) {
                int debounced = (keys->timed_keys & bit) &&
                    elapsed_milliseconds(&event.time,
                                         &keys->last_key_press[key]) <
                        INPUT_DEBOUNCE_MS;
                int bank_busy = keys->pressed_keys != 0U ||
                    keys->power_pressed;
                int bank_quiet = !keys->button_release_valid ||
                    elapsed_milliseconds(&event.time,
                                         &keys->last_button_release) >=
                        PHYSICAL_BUTTON_QUIET_MS;
                keys->pressed_keys |= bit;
                if(!debounced && !bank_busy && bank_quiet) {
                    keys->last_key_press[key] = event.time;
                    keys->timed_keys |= bit;
                    if(!suppress_actions || allow_wake) {
                        calf_action_t candidate = calf_ui_key_press(
                            ui, suppress_actions ? CALF_KEY_POWER : key);
                        if(candidate.kind != CALF_ACTION_NONE)
                            *action = candidate;
                        emitted = 1;
                    }
                }
            }
        }
    }
    return emitted;
}
