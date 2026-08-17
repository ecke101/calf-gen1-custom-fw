#include "ngcd.h"
#include "ngcd_audio.h"
#include "ngcd_imu.h"
#include "ngcd_mp4.h"
#include "ngcd_raw.h"
#include "ngcd_rk.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECORDING_STORAGE_CHECK_NS UINT64_C(1000000000)
#define RECORDING_FREE_RESERVE (UINT64_C(128) * 1024U * 1024U)
#define RECORDING_DEFAULT_SIZE_LIMIT (UINT64_C(4) * 1024U * 1024U * 1024U)
#define RECORDING_DEFAULT_TIME_LIMIT UINT64_C(300)
#define RECORDING_FINALIZE_RESERVE (UINT64_C(8) * 1024U * 1024U)
#define GRAPH_STOP_WATCHDOG_NS UINT64_C(15000000000)
#define GRAPH_STOP_WATCHDOG_POLL_NS UINT64_C(100000000)
#define NIGHT_PREVIEW_FPS_PATH "/tmp/calf-capture-fps"
#define SENSOR_TIMING_HELPER "/app/bin/calf-sensor-timing"

enum recording_split_type {
    RECORDING_SPLIT_SIZE = 0,
    RECORDING_SPLIT_TIME = 1,
    RECORDING_SPLIT_NONE = 2,
};

struct target_state {
    struct ngcd_imu *imu;
    struct ngcd_audio_control *audio_control;
    struct ngcd_rk_target *target;
    struct ngcd_rk_api api;
    struct ngcd_rk_display display;
    struct ngcd_rk_graph graph;
    struct ngcd_rk_audio_output audio_output;
    struct ngcd_rk_playback *playback;
    struct ngcd_profile profile;
    bool system_started;
    uint32_t image_dirty;
    struct ngcd_image_state image_desired;
    float transient_exposure;
    unsigned int transient_iso;
    bool transient_exposure_automatic;
    bool transient_exposure_valid;
    bool transient_iso_valid;
    int night_preview_fps;
    struct ngcd_encoder_state encoder_previous;
    int encoder_pending_channel;
    unsigned int media_sequence;
    uint64_t recording_storage_check_ns;
    uint64_t recording_size_limit;
    uint64_t recording_time_limit;
    uint64_t next_audio_detect_ns;
    int recording_split_type;
    char recording_temporary[NGCD_PATH_MAX];
    char recording_final[NGCD_PATH_MAX];
    bool encoder_update_pending;
    bool playback_restore_pending;
    pthread_t graph_watchdog_thread;
    atomic_bool graph_watchdog_shutdown;
    atomic_uint_fast64_t graph_stop_deadline_ns;
    bool graph_watchdog_started;
};

enum image_dirty_bit {
    IMAGE_DIRTY_EXPOSURE = 1U << 0,
    IMAGE_DIRTY_ISO = 1U << 1,
    IMAGE_DIRTY_WHITE_BALANCE = 1U << 2,
    IMAGE_DIRTY_EV = 1U << 3,
    IMAGE_DIRTY_BRIGHTNESS = 1U << 4,
    IMAGE_DIRTY_CONTRAST = 1U << 5,
    IMAGE_DIRTY_SATURATION = 1U << 6,
    IMAGE_DIRTY_HUE = 1U << 7,
    IMAGE_DIRTY_SHARPNESS = 1U << 8,
    IMAGE_DIRTY_NOISE_REDUCTION = 1U << 9,
    IMAGE_DIRTY_ANTI_FLICKER = 1U << 10,
    IMAGE_DIRTY_EFFECT = 1U << 11,
};

struct mode_profile {
    const char *mode;
    const char *path;
};

static const struct mode_profile MODE_PROFILES[] = {
    {"VR180_8K", "/local/ngcd-vr180-8k.yaml"},
    {"VR180_8K_MASK", "/local/ngcd-vr180-8k-mask.yaml"},
    {"VR180_6K", "/local/ngcd-vr180-6k.yaml"},
    {"VR180_6K_MASK", "/local/ngcd-vr180-6k-mask.yaml"},
    {"VR180_5K7", "/local/ngcd-vr180-5k7.yaml"},
    {"VR180_5K7_MASK", "/local/ngcd-vr180-5k7-mask.yaml"},
    {"SBS_5K7", "/local/ngcd-sbs-5k7.yaml"},
    {"VR180_4K", "/local/ngcd-vr180-4k.yaml"},
    {"VR180_4K_MASK", "/local/ngcd-vr180-4k-mask.yaml"},
    {"VR180_5K60", "/local/ngcd-vr180-5k60.yaml"},
    {"VR180_PIC", "/local/ngcd-vr180-pic.yaml"},
    {"3D_4K", "/local/ngcd-3d-4k.yaml"},
    {"3D_1080P", "/local/ngcd-3d-1080p.yaml"},
    {"SENSOR0_4K", "/local/ngcd-sensor0-4k.yaml"},
    {"SENSOR1_4K", "/local/ngcd-sensor1-4k.yaml"},
    {"SENSOR0_MAX", "/local/ngcd-sensor0-max.yaml"},
    {"SENSOR1_MAX", "/local/ngcd-sensor1-max.yaml"},
    {"SBS_6K", "/local/ngcd-sbs.yaml"},
    {"MIX", "/local/ngcd-mix.yaml"},
};

static const char *profile_path(const char *mode)
{
    size_t index;
    for (index = 0; index < sizeof(MODE_PROFILES) / sizeof(MODE_PROFILES[0]);
         ++index)
        if (strcmp(mode, MODE_PROFILES[index].mode) == 0)
            return MODE_PROFILES[index].path;
    return NULL;
}

static int publish_media(const char *temporary, const char *final_path)
{
    char link_target;
    if (readlink(final_path, &link_target, 1U) >= 0)
        return -1;
    if (errno != EINVAL && errno != ENOENT)
        return -1;
    if (access(final_path, F_OK) == 0 || errno != ENOENT)
        return -1;
    return rename(temporary, final_path);
}

static int recovery_media_paths(const char *directory, const char *name,
                                char *temporary, size_t temporary_size,
                                char *final_path, size_t final_size)
{
    size_t length;
    size_t index;
    int count;
    if (directory == NULL || name == NULL || temporary == NULL ||
        final_path == NULL)
        return -1;
    length = strlen(name);
    if (length < 24U || name[0] != '.' || name[1] != 'V' ||
        name[9] != '.' || memcmp(name + 10U, "mp4.ngcd-", 9U) != 0 ||
        memcmp(name + length - 4U, ".tmp", 4U) != 0)
        return -1;
    for (index = 2U; index <= 8U; ++index)
        if (name[index] < '0' || name[index] > '9')
            return -1;
    if (length == 23U)
        return -1;
    for (index = 19U; index < length - 4U; ++index)
        if (name[index] < '0' || name[index] > '9')
            return -1;
    count = snprintf(temporary, temporary_size, "%s/%s", directory, name);
    if (count <= 0 || (size_t)count >= temporary_size)
        return -1;
    count = snprintf(final_path, final_size, "%s/%.*s", directory,
                     12, name + 1U);
    return count > 0 && (size_t)count < final_size ? 0 : -1;
}

static void target_recover_recordings(void)
{
    struct ngcd_storage_info storage;
    char directory_path[NGCD_PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int count;
    if (ngcd_storage_read_status(&storage) != 0 || storage.read_only)
        return;
    count = snprintf(directory_path, sizeof(directory_path),
                     "%s/DCIM/100_CALF", storage.location);
    if (count <= 0 || (size_t)count >= sizeof(directory_path))
        return;
    directory = opendir(directory_path);
    if (directory == NULL)
        return;
    while ((entry = readdir(directory)) != NULL) {
        char temporary[NGCD_PATH_MAX];
        char final_path[NGCD_PATH_MAX];
        char link_target;
        if (recovery_media_paths(directory_path, entry->d_name,
                                 temporary, sizeof(temporary), final_path,
                                 sizeof(final_path)) != 0 ||
            readlink(temporary, &link_target, 1U) >= 0 || errno != EINVAL)
            continue;
        if (ngcd_mp4_recover(temporary) == 0 &&
            publish_media(temporary, final_path) == 0)
            fprintf(stderr, "ngcd: recovered interrupted recording %s\n",
                    final_path);
    }
    (void)closedir(directory);
}

static uint64_t target_monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void *target_graph_watchdog(void *opaque)
{
    struct target_state *state = opaque;
    const struct timespec delay = {
        0, (long)GRAPH_STOP_WATCHDOG_POLL_NS
    };
    while (!atomic_load_explicit(&state->graph_watchdog_shutdown,
                                 memory_order_acquire)) {
        uint64_t deadline = atomic_load_explicit(
            &state->graph_stop_deadline_ns, memory_order_acquire);
        if (deadline != 0U && target_monotonic_nanoseconds() >= deadline) {
            static const char message[] =
                "ngcd: camera graph stop watchdog expired; restarting\n";
            (void)write(2, message, sizeof(message) - 1U);
            (void)ngcd_select_stock_session();
            _exit(1);
        }
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

static int target_graph_watchdog_start(struct target_state *state)
{
    atomic_init(&state->graph_watchdog_shutdown, false);
    atomic_init(&state->graph_stop_deadline_ns, 0U);
    if (pthread_create(&state->graph_watchdog_thread, NULL,
                       target_graph_watchdog, state) != 0)
        return -1;
    state->graph_watchdog_started = true;
    return 0;
}

static void target_graph_watchdog_stop(struct target_state *state)
{
    if (!state->graph_watchdog_started)
        return;
    atomic_store_explicit(&state->graph_stop_deadline_ns, 0U,
                          memory_order_release);
    atomic_store_explicit(&state->graph_watchdog_shutdown, true,
                          memory_order_release);
    (void)pthread_join(state->graph_watchdog_thread, NULL);
    state->graph_watchdog_started = false;
}

static void target_graph_stop(struct target_state *state)
{
    uint64_t now = target_monotonic_nanoseconds();
    uint64_t deadline = now > UINT64_MAX - GRAPH_STOP_WATCHDOG_NS
                            ? UINT64_MAX
                            : now + GRAPH_STOP_WATCHDOG_NS;
    atomic_store_explicit(&state->graph_stop_deadline_ns, deadline,
                          memory_order_release);
    ngcd_rk_graph_stop(&state->graph);
    atomic_store_explicit(&state->graph_stop_deadline_ns, 0U,
                          memory_order_release);
}

static void target_abort_recording(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    if (state == NULL)
        return;
    ngcd_rk_graph_record_abort(&state->graph);
    if (state->recording_temporary[0] != '\0')
        (void)unlink(state->recording_temporary);
    backend->state.recording = false;
    backend->state.recording_started_ns = 0U;
    state->recording_temporary[0] = '\0';
    state->recording_final[0] = '\0';
    state->recording_storage_check_ns = 0U;
    state->recording_size_limit = 0U;
    state->recording_time_limit = 0U;
    state->recording_split_type = RECORDING_SPLIT_NONE;
}

static int target_finish_recording(struct ngcd_backend *backend);
static int target_split_recording(struct ngcd_backend *backend);

static void target_apply_profile_encoders(struct ngcd_backend *backend,
                                          const struct ngcd_profile *profile)
{
    size_t index;
    for (index = 0; index < profile->encoder_count && index < 3U; ++index)
        backend->state.encoder[index] = profile->encoder[index];
}

static void target_store_encoder(struct ngcd_backend *backend,
                                 struct target_state *state, int channel,
                                 const struct ngcd_encoder_state *encoder)
{
    size_t index;
    if (state->profile.encoder_count <= (size_t)channel) {
        for (index = state->profile.encoder_count;
             index <= (size_t)channel; ++index)
            state->profile.encoder[index] = backend->state.encoder[index];
        state->profile.encoder_count = (size_t)channel + 1U;
    }
    state->profile.encoder[channel] = *encoder;
    backend->state.encoder[channel] = *encoder;
}

static int parse_iso(const char *value, unsigned int *iso)
{
    static const char *const names[] = {
        "iso100", "iso200", "iso400", "iso800", "iso1600",
        "iso3200", "iso6400", "iso12800",
    };
    static const unsigned int values[] = {
        100U, 200U, 400U, 800U, 1600U, 3200U, 6400U, 12800U,
    };
    size_t index;
    if (strcmp(value, "auto") == 0) {
        *iso = 0U;
        return 0;
    }
    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (strcmp(value, names[index]) == 0) {
            *iso = values[index];
            return 0;
        }
    }
    return -1;
}

static int parse_exposure(const char *value, float *seconds, bool *automatic)
{
    const char *cursor;
    char *end = NULL;
    double parsed;
    bool digit = false;
    bool decimal_point = false;
    if (strcmp(value, "auto") == 0 || strcmp(value, "-1") == 0) {
        *seconds = 0.0f;
        *automatic = true;
        return 0;
    }
    for (cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            digit = true;
        } else if (*cursor == '.' && !decimal_point) {
            decimal_point = true;
        } else {
            return -1;
        }
    }
    if (!digit)
        return -1;
    parsed = strtod(value, &end);
    if (end == value || *end != '\0' || !(parsed > 0.0) || parsed > 12.0)
        return -1;
    *seconds = (float)parsed;
    if (!(*seconds > 0.0f))
        return -1;
    *automatic = false;
    return 0;
}

static bool exposure_iso_matches(float measured_exposure,
                                 unsigned int measured_iso,
                                 float requested_exposure,
                                 unsigned int requested_iso)
{
    float exposure_difference = measured_exposure - requested_exposure;
    /* Anti-flicker may shorten a manual ceiling to the nearest mains-aligned
     * exposure. At 15 fps in a 50 Hz environment, a requested 1/15 second is
     * legitimately applied as 60 ms (6300 sensor lines), exactly 10% shorter.
     * Retain the one-millisecond sensor/readback allowance while accepting
     * that bounded reduction. Stale timing remains far outside this window
     * (for example the 4 fps preview applies about 250 ms). */
    float exposure_tolerance = requested_exposure * 0.10f + 0.001f;
    unsigned int iso_difference = measured_iso > requested_iso
        ? measured_iso - requested_iso : requested_iso - measured_iso;
    unsigned int iso_tolerance = requested_iso / 16U + 2U;
    if (exposure_difference < 0.0f)
        exposure_difference = -exposure_difference;
    return exposure_difference <= exposure_tolerance &&
           iso_difference <= iso_tolerance;
}

static int wait_for_exposure_iso(struct target_state *state,
                                 float requested_exposure,
                                 unsigned int requested_iso,
                                 unsigned int attempts,
                                 long delay_nanoseconds,
                                 float *measured_exposure,
                                 unsigned int *measured_iso)
{
    unsigned int attempt;
    if (state == NULL || !(requested_exposure > 0.0f) ||
        requested_iso == 0U || attempts == 0U ||
        delay_nanoseconds < 0L || delay_nanoseconds >= 1000000000L ||
        measured_exposure == NULL || measured_iso == NULL)
        return -1;
    for (attempt = 0U; attempt < attempts; ++attempt) {
        struct timespec delay = {0, delay_nanoseconds};
        if (ngcd_rk_image_query_exposure(&state->graph, measured_exposure,
                                         measured_iso) == 0 &&
            exposure_iso_matches(*measured_exposure, *measured_iso,
                                 requested_exposure, requested_iso))
            return 0;
        if (attempt + 1U < attempts) {
            /* The HTTP server normally advances display and graph work once
             * between requests. A Night preview transaction deliberately
             * keeps the whole transition inside one request, so preserve
             * that progress while waiting for AIQ to publish a new applied
             * sensor frame. */
            (void)ngcd_rk_display_tick(&state->display);
            (void)ngcd_rk_graph_tick(&state->graph);
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        }
    }
    return -1;
}

static int write_night_preview_fps(int fps)
{
    char text[3];
    int descriptor;
    int count;
    ssize_t written;
    if (fps == 30) {
        if (unlink(NIGHT_PREVIEW_FPS_PATH) != 0 && errno != ENOENT)
            return -1;
        return 0;
    }
    if (fps != 4 && fps != 8 && fps != 15)
        return -1;
    count = snprintf(text, sizeof(text), "%d", fps);
    if (count <= 0 || (size_t)count >= sizeof(text))
        return -1;
    descriptor = open(NIGHT_PREVIEW_FPS_PATH,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    if (descriptor < 0)
        return -1;
    written = write(descriptor, text, (size_t)count);
    if (written != count || fsync(descriptor) != 0) {
        (void)close(descriptor);
        (void)unlink(NIGHT_PREVIEW_FPS_PATH);
        return -1;
    }
    if (close(descriptor) != 0) {
        (void)unlink(NIGHT_PREVIEW_FPS_PATH);
        return -1;
    }
    return 0;
}

static int run_sensor_timing_helper(int fps)
{
    char text[3];
    char *arguments[3];
    pid_t child;
    pid_t waited;
    int status;
    int count = snprintf(text, sizeof(text), "%d", fps);
    if (count <= 0 || (size_t)count >= sizeof(text) ||
        write_night_preview_fps(fps) != 0)
        return -1;
    arguments[0] = (char *)SENSOR_TIMING_HELPER;
    arguments[1] = text;
    arguments[2] = NULL;
    child = fork();
    if (child < 0) {
        (void)unlink(NIGHT_PREVIEW_FPS_PATH);
        return -1;
    }
    if (child == 0) {
        execv(SENSOR_TIMING_HELPER, arguments);
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* The helper restores the physical 30 fps baseline on an internal
         * error. Keep AIQ's frame-rate policy consistent with that recovery. */
        (void)unlink(NIGHT_PREVIEW_FPS_PATH);
        return -1;
    }
    return 0;
}

static int parse_white_balance(const char *value, unsigned int *kelvin)
{
    static const char *const names[] = {
        "daylight", "cloudy", "shadow", "fluorescent", "tungsten",
    };
    static const unsigned int values[] = {
        5200U, 6000U, 7000U, 4000U, 3200U,
    };
    char *end = NULL;
    long numeric;
    size_t index;
    if (strcmp(value, "auto") == 0) {
        *kelvin = 0U;
        return 0;
    }
    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (strcmp(value, names[index]) == 0) {
            *kelvin = values[index];
            return 0;
        }
    }
    numeric = strtol(value, &end, 10);
    if (end == value || *end != '\0' || numeric < 1000 || numeric > 10000)
        return -1;
    *kelvin = (unsigned int)numeric;
    return 0;
}

static int parse_flicker(const char *value,
                         enum ngcd_rk_flicker_control *control)
{
    static const char *const names[] = {"off", "auto", "50hz", "60hz"};
    size_t index;
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(value, names[index]) == 0) {
            *control = (enum ngcd_rk_flicker_control)index;
            return 0;
        }
    }
    return -1;
}

static int parse_effect(const char *value, unsigned int *effect)
{
    static const char *const names[] = {
        "none", "blackwhite", "negative", "sepia", "emboss", "sketch",
    };
    size_t index;
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(value, names[index]) == 0) {
            *effect = (unsigned int)index;
            return 0;
        }
    }
    return -1;
}

static uint32_t greatest_common_divisor(uint32_t left, uint32_t right)
{
    while (right != 0U) {
        uint32_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int exposure_rational(float seconds, uint32_t *numerator,
                             uint32_t *denominator)
{
    uint32_t divisor;
    if (!(seconds > 0.0f) || seconds > 3600.0f || numerator == NULL ||
        denominator == NULL)
        return -1;
    *numerator = (uint32_t)(seconds * 1000000.0f + 0.5f);
    *denominator = 1000000U;
    if (*numerator == 0U)
        return -1;
    divisor = greatest_common_divisor(*numerator, *denominator);
    *numerator /= divisor;
    *denominator /= divisor;
    return 0;
}

static uint32_t image_dirty_bit_for_type(const char *type)
{
    if (strcmp(type, "exp") == 0)
        return IMAGE_DIRTY_EXPOSURE;
    if (strcmp(type, "iso") == 0)
        return IMAGE_DIRTY_ISO;
    if (strcmp(type, "wb") == 0)
        return IMAGE_DIRTY_WHITE_BALANCE;
    if (strcmp(type, "ev") == 0)
        return IMAGE_DIRTY_EV;
    if (strcmp(type, "brightness") == 0)
        return IMAGE_DIRTY_BRIGHTNESS;
    if (strcmp(type, "contrast") == 0)
        return IMAGE_DIRTY_CONTRAST;
    if (strcmp(type, "saturation") == 0)
        return IMAGE_DIRTY_SATURATION;
    if (strcmp(type, "hue") == 0)
        return IMAGE_DIRTY_HUE;
    if (strcmp(type, "sharpness") == 0)
        return IMAGE_DIRTY_SHARPNESS;
    if (strcmp(type, "3dnr") == 0)
        return IMAGE_DIRTY_NOISE_REDUCTION;
    if (strcmp(type, "antiflicker") == 0)
        return IMAGE_DIRTY_ANTI_FLICKER;
    if (strcmp(type, "imgeffect") == 0)
        return IMAGE_DIRTY_EFFECT;
    return 0U;
}

static int target_start(struct ngcd_backend *backend)
{
    struct target_state *state;
    char error[128];
    state = malloc(sizeof(*state));
    if (state == NULL)
        return -1;
    memset(state, 0, sizeof(*state));
    state->night_preview_fps = 30;
    (void)unlink(NIGHT_PREVIEW_FPS_PATH);
    if (target_graph_watchdog_start(state) != 0) {
        free(state);
        return -1;
    }
    target_recover_recordings();
    if (ngcd_rk_target_open(&state->target, &state->api) != 0) {
        fprintf(stderr, "ngcd: Rockchip ABI loading failed\n");
        goto fail;
    }
    if (state->api.system_init(state->target) != 0) {
        fprintf(stderr, "ngcd: Rockchip SYS initialization failed\n");
        goto fail;
    }
    state->system_started = true;
    if (ngcd_rk_audio_output_start(&state->audio_output, &state->api,
                                   state->target) != 0) {
        fprintf(stderr, "ngcd: audio output initialization failed\n");
        goto fail;
    }
    if (ngcd_audio_control_open(&state->audio_control) != 0) {
        fprintf(stderr, "ngcd: ALSA capture control initialization failed\n");
        goto fail;
    }
    if (ngcd_rk_display_start(&state->display, &state->api,
                              state->target) != 0) {
        fprintf(stderr, "ngcd: LCD display initialization failed\n");
        goto fail;
    }
    if (ngcd_profile_load("/local/ngcd-vr180-pic.yaml", &state->profile,
                          error, sizeof(error)) != 0) {
        fprintf(stderr, "ngcd: camera profile loading failed: %s\n", error);
        goto fail;
    }
    if (ngcd_rk_graph_start_in_system(&state->graph, &state->api,
                                      state->target, &state->profile,
                                      &state->display) != 0) {
        fprintf(stderr, "ngcd: camera graph initialization failed\n");
        goto fail;
    }
    {
        int input = backend->state.audio_input;
        if (backend->state.audio_auto != 0 &&
            ngcd_audio_control_detect_input(state->audio_control,
                                            &input) != 0)
            goto fail;
        if (ngcd_audio_control_apply(
                state->audio_control, input,
                backend->state.audio_volume[input]) != 0 ||
            ngcd_rk_graph_set_audio_input(&state->graph, input) != 0)
            goto fail;
        backend->state.audio_input = input;
        state->next_audio_detect_ns =
            target_monotonic_nanoseconds() + UINT64_C(2000000000);
    }
    if (ngcd_imu_open(&state->imu) != 0)
        fprintf(stderr,
                "ngcd: IMU initialization failed; continuing without motion data\n");
    backend->private_data = state;
    backend->state.camera_running = true;
    state->image_desired = backend->state.image;
    target_apply_profile_encoders(backend, &state->profile);
    if (ngcd_backlight_read(&backend->state.backlight) == 0 &&
        backend->state.backlight > 0)
        backend->state.backlight_saved = backend->state.backlight;
    memcpy(backend->state.camera_mode, "VR180_PIC", sizeof("VR180_PIC"));
    return 0;

fail:
    ngcd_imu_close(state->imu);
    target_graph_stop(state);
    ngcd_rk_display_stop(&state->display);
    ngcd_rk_audio_output_stop(&state->audio_output);
    ngcd_audio_control_close(state->audio_control);
    if (state->system_started)
        (void)state->api.system_exit(state->target);
    ngcd_rk_target_close(state->target);
    target_graph_watchdog_stop(state);
    free(state);
    return -1;
}

static void target_stop(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    if (state == NULL)
        return;
    if (backend->state.recording) {
        if (target_finish_recording(backend) != 0)
            target_abort_recording(backend);
    }
    (void)ngcd_rk_display_histogram_suspend(&state->display);
    ngcd_rk_playback_close(state->playback);
    state->playback = NULL;
    ngcd_imu_close(state->imu);
    target_graph_stop(state);
    ngcd_rk_display_stop(&state->display);
    ngcd_rk_audio_output_stop(&state->audio_output);
    ngcd_audio_control_close(state->audio_control);
    if (state->system_started)
        (void)state->api.system_exit(state->target);
    ngcd_rk_target_close(state->target);
    target_graph_watchdog_stop(state);
    free(state);
    backend->private_data = NULL;
    backend->state.camera_running = false;
}

static int target_tick(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    struct ngcd_camm_gyro_sample gyro;
    int display_result;
    int graph_result;
    if (state == NULL)
        return -1;
    if (backend->state.audio_auto != 0 && !backend->state.recording) {
        uint64_t now = target_monotonic_nanoseconds();
        if (now >= state->next_audio_detect_ns) {
            int input;
            state->next_audio_detect_ns = now + UINT64_C(2000000000);
            if (ngcd_audio_control_detect_input(state->audio_control,
                                                &input) == 0 &&
                input != backend->state.audio_input &&
                ngcd_audio_control_apply(
                    state->audio_control, input,
                    backend->state.audio_volume[input]) == 0 &&
                ngcd_rk_graph_set_audio_input(&state->graph, input) == 0)
                backend->state.audio_input = input;
        }
    }
    if (state->playback != NULL) {
        int playback_result = ngcd_rk_playback_tick(
            state->playback, target_monotonic_nanoseconds() / 1000U);
        backend->state.playback_paused =
            ngcd_rk_playback_is_paused(state->playback);
        backend->state.playback_sample_index =
            (int)ngcd_rk_playback_sample_index(state->playback);
        backend->state.playback_decoder_received =
            ngcd_rk_playback_decoder_received(state->playback);
        backend->state.playback_decoder_decoded =
            ngcd_rk_playback_decoder_decoded(state->playback);
        backend->state.playback_decoder_pending_stream =
            ngcd_rk_playback_decoder_pending_stream(state->playback);
        backend->state.playback_decoder_pending_pictures =
            ngcd_rk_playback_decoder_pending_pictures(state->playback);
        backend->state.playback_decoder_errors =
            ngcd_rk_playback_decoder_errors(state->playback);
        backend->state.playback_presented_frames =
            ngcd_rk_playback_presented_frames(state->playback);
        backend->state.playback_output_errors =
            ngcd_rk_playback_output_errors(state->playback);
        if (playback_result != 0)
            return playback_result;
    }
    display_result = ngcd_rk_display_tick(&state->display);
    graph_result = ngcd_rk_graph_tick(&state->graph);
    while (state->imu != NULL &&
           ngcd_imu_pop_camm_gyro(state->imu, &gyro) == 0) {
        if (backend->state.recording &&
            ngcd_rk_graph_record_camm_gyro(
                &state->graph, gyro.monotonic_ns,
                gyro.x_radians_per_second, gyro.y_radians_per_second,
                gyro.z_radians_per_second) != 0) {
            target_abort_recording(backend);
            break;
        }
    }
    if (backend->state.recording && state->graph.recording_failed) {
        target_abort_recording(backend);
    } else if (backend->state.recording) {
        uint64_t now = target_monotonic_nanoseconds();
        uint64_t size = 0U;
        uint64_t duration = 0U;
        bool split = false;
        if (state->recording_split_type == RECORDING_SPLIT_SIZE &&
            ngcd_rk_graph_record_size(&state->graph, &size) == 0 &&
            size >= (state->recording_size_limit > RECORDING_FINALIZE_RESERVE
                         ? state->recording_size_limit -
                               RECORDING_FINALIZE_RESERVE
                         : state->recording_size_limit))
            split = true;
        else if (state->recording_split_type == RECORDING_SPLIT_TIME &&
                 ngcd_rk_graph_record_duration(&state->graph, &duration) == 0 &&
                 duration / UINT64_C(1000000) >=
                     state->recording_time_limit)
            split = true;
        if (split && target_split_recording(backend) != 0) {
            target_abort_recording(backend);
            return display_result != 0 ? display_result : -1;
        }
        if (now != 0U && now >= state->recording_storage_check_ns) {
            struct ngcd_storage_info storage;
            state->recording_storage_check_ns =
                now + RECORDING_STORAGE_CHECK_NS;
            if (ngcd_storage_read_status(&storage) != 0 ||
                storage.read_only) {
                target_abort_recording(backend);
            } else if (storage.free_bytes < RECORDING_FREE_RESERVE &&
                       target_finish_recording(backend) != 0) {
                target_abort_recording(backend);
            }
        }
    }
    if (state->encoder_update_pending) {
        if (state->graph.validation_complete) {
            state->encoder_update_pending = false;
        } else if (state->graph.validation_failed) {
            target_store_encoder(backend, state,
                                 state->encoder_pending_channel,
                                 &state->encoder_previous);
            state->encoder_update_pending = false;
        }
    }
    return display_result != 0 ? display_result : graph_result;
}

static int target_graphics_control_id(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    return state != NULL ? ngcd_rk_display_control_id(&state->display) : -1;
}

static void target_display_retry_delay(void)
{
    struct timespec delay = {0, 100000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int target_histogram(struct ngcd_backend *backend,
                            uint32_t bins[NGCD_HISTOGRAM_BINS])
{
    struct target_state *state = backend->private_data;
    int attempt;
    if (state == NULL || (!backend->state.camera_running &&
                          state->playback == NULL))
        return -1;
    for (attempt = 0; attempt < 2; ++attempt) {
        if (ngcd_rk_display_histogram(&state->display, bins) == 0)
            return 0;
        target_display_retry_delay();
    }
    return -1;
}

static int target_lcd_screenshot(struct ngcd_backend *backend,
                                 const unsigned char **data, size_t *size)
{
    struct target_state *state = backend->private_data;
    int pipeline_was_started;
    int attempt;
    int result = -1;
    if (state == NULL || data == NULL || size == NULL ||
        (!backend->state.camera_running && state->playback == NULL))
        return -1;
    pipeline_was_started = state->display.histogram_wbc_vpss_bound;
    /* The first device-source frame can time out while a newly-created WBC
     * pipeline drains its initial layer-source queue. A complete failed call
     * restores the video source; retain that warmed pipeline for one bounded
     * retry, then tear it down if histogram polling did not already own it. */
    for (attempt = 0; attempt < 2; ++attempt) {
        if (ngcd_rk_display_screenshot_bmp(
                &state->display, data, size) == 0) {
            result = 0;
            break;
        }
        target_display_retry_delay();
    }
    if (!pipeline_was_started)
        ngcd_rk_display_auxiliary_stop(&state->display);
    return result;
}

static int target_set_image(struct ngcd_backend *backend,
                            const char *type, const char *value, bool fixed);
static int target_reapply_image_state(struct ngcd_backend *backend,
                                      uint32_t dirty);

static int target_wait_sensor_outputs(struct target_state *state,
                                      int timeout_ms)
{
    unsigned int input;
    if (state == NULL || state->graph.api == NULL ||
        state->graph.api->wait_output == NULL || timeout_ms <= 0)
        return -1;
    for (input = 0U; input < state->graph.sensor_count; ++input) {
        unsigned int drained = 0U;
        /* A queued VPSS frame predates the failed WB transaction and cannot
         * prove that group AWB has advanced. Drain bounded stale output, then
         * require the next frame produced after the drain. */
        while (drained < 32U && state->graph.api->wait_output(
                   state->graph.api_context, false, (int)input, 0, 0) == 0)
            ++drained;
        if (drained == 32U)
            return -1;
        if (state->graph.api->wait_output(
                state->graph.api_context, false, (int)input, 0,
                timeout_ms) != 0)
            return -1;
    }
    return 0;
}

static int target_restore_graph(struct ngcd_backend *backend,
                                struct target_state *state,
                                const struct ngcd_profile *profile,
                                uint32_t dirty)
{
    if (ngcd_rk_graph_start_in_system(&state->graph, &state->api,
                                      state->target, profile,
                                      &state->display) != 0)
        return -1;
    if (ngcd_rk_graph_set_audio_input(&state->graph,
                                      backend->state.audio_input) != 0) {
        target_graph_stop(state);
        return -1;
    }
    backend->state.camera_running = true;
    if (target_reapply_image_state(backend, dirty) == 0)
        return 0;
    target_graph_stop(state);
    backend->state.camera_running = false;
    return -1;
}

static int target_image_accepted(struct ngcd_backend *backend,
                                 struct target_state *state, uint32_t dirty)
{
    const struct ngcd_image_state *image = &backend->state.image;
    struct ngcd_image_state *desired = &state->image_desired;
    if (dirty == IMAGE_DIRTY_EXPOSURE)
        memcpy(desired->exposure, image->exposure,
               strlen(image->exposure) + 1U);
    else if (dirty == IMAGE_DIRTY_ISO)
        memcpy(desired->iso, image->iso, strlen(image->iso) + 1U);
    else if (dirty == IMAGE_DIRTY_WHITE_BALANCE)
        memcpy(desired->white_balance, image->white_balance,
               strlen(image->white_balance) + 1U);
    else if (dirty == IMAGE_DIRTY_EV)
        memcpy(desired->exposure_compensation,
               image->exposure_compensation,
               strlen(image->exposure_compensation) + 1U);
    else if (dirty == IMAGE_DIRTY_BRIGHTNESS)
        desired->brightness = image->brightness;
    else if (dirty == IMAGE_DIRTY_CONTRAST)
        desired->contrast = image->contrast;
    else if (dirty == IMAGE_DIRTY_SATURATION)
        desired->saturation = image->saturation;
    else if (dirty == IMAGE_DIRTY_HUE)
        desired->hue = image->hue;
    else if (dirty == IMAGE_DIRTY_SHARPNESS)
        desired->sharpness = image->sharpness;
    else if (dirty == IMAGE_DIRTY_NOISE_REDUCTION)
        desired->noise_reduction = image->noise_reduction;
    else if (dirty == IMAGE_DIRTY_ANTI_FLICKER)
        memcpy(desired->anti_flicker, image->anti_flicker,
               strlen(image->anti_flicker) + 1U);
    else if (dirty == IMAGE_DIRTY_EFFECT)
        memcpy(desired->effect, image->effect,
               strlen(image->effect) + 1U);
    else
        return -1;
    state->image_dirty |= dirty;
    return 0;
}

static int target_camera_mode(struct ngcd_backend *backend,
                              const char *mode, bool start)
{
    struct target_state *state = backend->private_data;
    struct ngcd_profile next;
    struct ngcd_profile previous;
    const char *path;
    char error[128];
    bool was_running;
    if (state == NULL || backend->state.recording || backend->state.playback ||
        state->encoder_update_pending)
        return -1;
    if (!start) {
        state->transient_exposure_valid = false;
        state->transient_iso_valid = false;
        state->night_preview_fps = 30;
        (void)unlink(NIGHT_PREVIEW_FPS_PATH);
        if (ngcd_rk_display_histogram_suspend(&state->display) != 0)
            return -1;
        target_graph_stop(state);
        backend->state.camera_running = false;
        return 0;
    }
    if (backend->state.camera_running &&
        strcmp(backend->state.camera_mode, mode) == 0)
        return 0;
    path = profile_path(mode);
    if (path == NULL || ngcd_profile_load(path, &next, error, sizeof(error)) != 0)
        return -1;
    state->transient_exposure_valid = false;
    state->transient_iso_valid = false;
    state->night_preview_fps = 30;
    (void)unlink(NIGHT_PREVIEW_FPS_PATH);
    previous = state->profile;
    was_running = backend->state.camera_running;
    if (was_running) {
        if (ngcd_rk_display_histogram_suspend(&state->display) != 0)
            return -1;
        target_graph_stop(state);
    }
    backend->state.camera_running = false;
    if (target_restore_graph(backend, state, &next,
                             state->image_dirty) != 0) {
        backend->state.camera_running = false;
        if (was_running)
            (void)target_restore_graph(backend, state, &previous,
                                       state->image_dirty);
        return -1;
    }
    state->profile = next;
    target_apply_profile_encoders(backend, &state->profile);
    backend->state.camera_running = true;
    memcpy(backend->state.camera_mode, mode, strlen(mode) + 1);
    return 0;
}

static int target_apply_preview_pair(struct target_state *state,
                                     float exposure, unsigned int iso)
{
    float exposure_readback = 0.0f;
    unsigned int iso_readback = 0U;
    int result = ngcd_rk_image_set_exposure_iso(
        &state->graph, exposure, false, iso,
        &exposure_readback, &iso_readback);
    if (result != 0 || exposure_readback != exposure || iso_readback != iso)
        return result != 0 ? result : -1;
    return wait_for_exposure_iso(state, exposure, iso, 24U, 50000000L,
                                 &exposure_readback, &iso_readback);
}

static int target_night_preview(struct ngcd_backend *backend, int fps,
                                const char *exposure_value,
                                const char *iso_value)
{
    struct target_state *state = backend->private_data;
    float exposure;
    float rollback_exposure = 0.0f;
    unsigned int iso;
    unsigned int rollback_iso = 0U;
    bool exposure_automatic;
    bool rollback_exposure_automatic = false;
    bool rollback_pair_valid = false;
    bool old_exposure_valid;
    bool old_iso_valid;
    float old_transient_exposure;
    unsigned int old_transient_iso;
    bool old_transient_automatic;
    int old_fps;
    int failure = -1;
    int rollback_result = 0;

    if (state == NULL || !backend->state.camera_running ||
        backend->state.recording || backend->state.playback ||
        (fps != 4 && fps != 8 && fps != 15 && fps != 30) ||
        parse_exposure(exposure_value, &exposure, &exposure_automatic) != 0 ||
        exposure_automatic || parse_iso(iso_value, &iso) != 0 || iso == 0U)
        return -10;

    old_fps = state->night_preview_fps != 0 ? state->night_preview_fps : 30;
    old_exposure_valid = state->transient_exposure_valid;
    old_iso_valid = state->transient_iso_valid;
    old_transient_exposure = state->transient_exposure;
    old_transient_iso = state->transient_iso;
    old_transient_automatic = state->transient_exposure_automatic;
    if (old_exposure_valid && old_iso_valid && !old_transient_automatic) {
        rollback_exposure = old_transient_exposure;
        rollback_iso = old_transient_iso;
        rollback_pair_valid = true;
    } else if (parse_exposure(state->image_desired.exposure,
                              &rollback_exposure,
                              &rollback_exposure_automatic) == 0 &&
               !rollback_exposure_automatic &&
               parse_iso(state->image_desired.iso, &rollback_iso) == 0 &&
               rollback_iso != 0U) {
        rollback_pair_valid = true;
    }

    /* The snapshot coordinator temporarily owns the same physical sensor
     * controls outside this process and restores a 30 fps baseline before the
     * UI asks us to restore Night preview. Reassert timing even when our last
     * successful logical FPS equals the request; cached state cannot prove
     * that the external capture transaction left the sensors there. */
    if (run_sensor_timing_helper(fps) != 0) {
        failure = -20;
        goto rollback;
    }
    if (fps == 4 && old_fps > 4) {
        int result = target_apply_preview_pair(state, 0.12f, iso);
        if (result != 0) {
            fprintf(stderr,
                    "ngcd: Night preview bridge failed at 0.12 s ISO %u (%d)\n",
                    iso, result);
            failure = result != -1 ? result : -30;
            goto rollback;
        }
    }
    {
        int result = target_apply_preview_pair(state, exposure, iso);
        if (result != 0) {
            fprintf(stderr,
                    "ngcd: Night preview target failed at %.7g s ISO %u (%d)\n",
                    (double)exposure, iso, result);
            failure = result != -1 ? result : -40;
            goto rollback;
        }
    }

    state->night_preview_fps = fps;
    if (fps == 30) {
        state->transient_exposure_valid = false;
        state->transient_iso_valid = false;
    } else {
        state->transient_exposure = exposure;
        state->transient_iso = iso;
        state->transient_exposure_automatic = false;
        state->transient_exposure_valid = true;
        state->transient_iso_valid = true;
    }
    return 0;

rollback:
    if (run_sensor_timing_helper(old_fps) != 0)
        rollback_result = -1;
    if (rollback_pair_valid &&
        target_apply_preview_pair(state, rollback_exposure,
                                  rollback_iso) != 0)
        rollback_result = -1;
    state->night_preview_fps = old_fps;
    state->transient_exposure = old_transient_exposure;
    state->transient_iso = old_transient_iso;
    state->transient_exposure_automatic = old_transient_automatic;
    state->transient_exposure_valid = old_exposure_valid;
    state->transient_iso_valid = old_iso_valid;
    return rollback_result == 0 ? failure : failure - 100;
}

static int target_set_image(struct ngcd_backend *backend,
                            const char *type, const char *value,
                            bool fixed)
{
    struct target_state *state = backend->private_data;
    struct ngcd_image_state *image = &backend->state.image;
    enum ngcd_rk_acp_control control;
    int *accepted_state;
    char *end = NULL;
    long requested;
    unsigned int iso = 0;
    unsigned int iso_readback;
    float exposure;
    float exposure_readback;
    bool exposure_auto;
    unsigned int white_balance;
    unsigned int white_balance_readback;
    unsigned int white_balance_attempt;
    enum ngcd_rk_flicker_control flicker;
    enum ngcd_rk_flicker_control flicker_readback;
    unsigned int effect;
    unsigned int effect_readback;
    uint32_t dirty;
    int readback;
    if (state == NULL || !backend->state.camera_running || value == NULL)
        return -1;
    dirty = image_dirty_bit_for_type(type);
    if (dirty == 0U)
        return -1;
    if (fixed && dirty != IMAGE_DIRTY_EXPOSURE &&
        dirty != IMAGE_DIRTY_ISO)
        return -1;
    if (strcmp(type, "iso") == 0) {
        float paired_exposure;
        float paired_exposure_readback;
        bool paired_exposure_automatic;
        int pair_result;
        if (parse_iso(value, &iso) != 0)
            return -1;
        if (fixed && state->transient_exposure_valid) {
            paired_exposure = state->transient_exposure;
            paired_exposure_automatic =
                state->transient_exposure_automatic;
        } else if (parse_exposure(state->image_desired.exposure,
                                  &paired_exposure,
                                  &paired_exposure_automatic) != 0) {
            return -1;
        }
        pair_result = ngcd_rk_image_set_exposure_iso(
            &state->graph, paired_exposure,
            paired_exposure_automatic, iso,
            &paired_exposure_readback, &iso_readback);
        if (pair_result != 0)
            return pair_result;
        if (iso_readback != iso ||
            (paired_exposure_automatic && paired_exposure_readback != -1.0f) ||
            (!paired_exposure_automatic &&
             paired_exposure_readback != paired_exposure))
            return -1;
        if (!fixed) {
            state->transient_exposure_valid = false;
            state->transient_iso_valid = false;
            memcpy(image->iso, value, strlen(value) + 1U);
            return target_image_accepted(backend, state, dirty);
        }
        state->transient_iso = iso;
        state->transient_iso_valid = true;
        /* The setter readback above verifies AIQ's requested range, not the
         * value already applied to a sensor frame.  A Night preview update is
         * complete only when the query result has reached the paired manual
         * exposure and ISO. */
        if (!paired_exposure_automatic &&
            wait_for_exposure_iso(state, paired_exposure, iso,
                                  20U, 50000000L, &exposure_readback,
                                  &iso_readback) != 0) {
            fprintf(stderr,
                    "ngcd: Night preview did not apply %.7g s ISO %u "
                    "(last %.7g s ISO %u)\n",
                    (double)paired_exposure, iso,
                    (double)exposure_readback, iso_readback);
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "exp") == 0) {
        unsigned int paired_iso;
        int pair_result;
        if (parse_exposure(value, &exposure, &exposure_auto) != 0 ||
            ((!fixed || !state->transient_iso_valid) &&
             parse_iso(state->image_desired.iso, &paired_iso) != 0))
            return -1;
        if (fixed && state->transient_iso_valid)
            paired_iso = state->transient_iso;
        pair_result = ngcd_rk_image_set_exposure_iso(
            &state->graph, exposure, exposure_auto, paired_iso,
            &exposure_readback, &iso_readback);
        if (pair_result != 0)
            return pair_result;
        if ((exposure_auto && exposure_readback != -1.0f) ||
            (!exposure_auto && exposure_readback != exposure) ||
            iso_readback != paired_iso)
            return -1;
        if (!fixed) {
            state->transient_exposure_valid = false;
            state->transient_iso_valid = false;
            memcpy(image->exposure, value, strlen(value) + 1U);
            return target_image_accepted(backend, state, dirty);
        }
        /* AIQ's immediate attribute readback only confirms that it accepted
         * the requested range. A slower Night transition may need a sensor
         * frame before a subsequent range expansion is valid (8 fps -> 4 fps
         * reasserts 1/8, then expands to 240 ms). Complete this endpoint only
         * when the paired manual values are observable on an applied frame. */
        if (!exposure_auto && paired_iso != 0U &&
            wait_for_exposure_iso(state, exposure, paired_iso,
                                  20U, 50000000L, &exposure_readback,
                                  &iso_readback) != 0) {
            fprintf(stderr,
                    "ngcd: Night exposure did not apply %.7g s ISO %u "
                    "(last %.7g s ISO %u)\n",
                    (double)exposure, paired_iso,
                    (double)exposure_readback, iso_readback);
            return -1;
        }
        state->transient_exposure = exposure;
        state->transient_exposure_automatic = exposure_auto;
        state->transient_exposure_valid = true;
        return 0;
    }
    if (strcmp(type, "ev") == 0) {
        requested = strtol(value, &end, 10);
        if (end == value || *end != '\0' || requested < -3 ||
            requested > 3 ||
            ngcd_rk_image_set_exposure_compensation(
                &state->graph, (int)requested, &readback) != 0 ||
            readback != requested)
            return -1;
        memcpy(image->exposure_compensation, value, strlen(value) + 1U);
        return target_image_accepted(backend, state, dirty);
    }
    if (strcmp(type, "wb") == 0) {
        if (parse_white_balance(value, &white_balance) != 0)
            return -1;
        for (white_balance_attempt = 0U;
             white_balance_attempt < 4U; ++white_balance_attempt) {
            if (ngcd_rk_image_set_white_balance(
                    &state->graph, white_balance,
                    &white_balance_readback) == 0 &&
                white_balance_readback == white_balance) {
                memcpy(image->white_balance, value, strlen(value) + 1U);
                return target_image_accepted(backend, state, dirty);
            }
            fprintf(stderr,
                    "ngcd: white balance apply failed value=%s attempt=%u\n",
                    value, white_balance_attempt + 1U);
            /* A fresh camera-group AWB context can require its first output
             * before a child-context readback reflects the group update.
             * Advance by observed frames and retry the verified transaction
             * inside this request instead of exposing a transient API error. */
            if (white_balance_attempt + 1U >= 4U ||
                target_wait_sensor_outputs(state, 3000) != 0)
                break;
        }
        return -1;
    }
    if (strcmp(type, "antiflicker") == 0) {
        int result;
        if (parse_flicker(value, &flicker) != 0)
            return -1;
        result = ngcd_rk_image_set_flicker(&state->graph, flicker,
                                           &flicker_readback);
        if (result != 0)
            return result;
        if (flicker_readback != flicker)
            return -7;
        memcpy(image->anti_flicker, value, strlen(value) + 1U);
        return target_image_accepted(backend, state, dirty);
    }
    if (strcmp(type, "imgeffect") == 0) {
        if (parse_effect(value, &effect) != 0 ||
            ngcd_rk_image_set_effect(&state->graph, effect,
                                     &effect_readback) != 0 ||
            effect_readback != effect)
            return -1;
        memcpy(image->effect, value, strlen(value) + 1U);
        return target_image_accepted(backend, state, dirty);
    }
    requested = strtol(value, &end, 10);
    if (end == value || *end != '\0' || requested < 0 || requested > 20)
        return -1;
    if (strcmp(type, "sharpness") == 0) {
        if (ngcd_rk_image_set_sharpness(&state->graph, (int)requested,
                                        &readback) != 0)
            return -1;
        image->sharpness = readback;
        return target_image_accepted(backend, state, dirty);
    }
    if (strcmp(type, "3dnr") == 0) {
        if (ngcd_rk_image_set_noise_reduction(&state->graph, (int)requested,
                                              &readback) != 0)
            return -1;
        image->noise_reduction = readback;
        return target_image_accepted(backend, state, dirty);
    }
    if (strcmp(type, "brightness") == 0) {
        control = NGCD_RK_ACP_BRIGHTNESS;
        accepted_state = &image->brightness;
    } else if (strcmp(type, "contrast") == 0) {
        control = NGCD_RK_ACP_CONTRAST;
        accepted_state = &image->contrast;
    } else if (strcmp(type, "saturation") == 0) {
        control = NGCD_RK_ACP_SATURATION;
        accepted_state = &image->saturation;
    } else if (strcmp(type, "hue") == 0) {
        control = NGCD_RK_ACP_HUE;
        accepted_state = &image->hue;
    } else {
        return -1;
    }
    if (ngcd_rk_image_set_acp(&state->graph, control, (int)requested,
                              &readback) != 0)
        return -1;
    *accepted_state = readback;
    return target_image_accepted(backend, state, dirty);
}

static int target_read_image(struct ngcd_backend *backend)
{
    static const char *const flicker_names[] = {
        "off", "auto", "50hz", "60hz",
    };
    static const char *const effect_names[] = {
        "none", "blackwhite", "negative", "sepia", "emboss", "sketch",
    };
    static const unsigned int white_balance_values[] = {
        5200U, 6000U, 7000U, 4000U, 3200U,
    };
    static const char *const white_balance_names[] = {
        "daylight", "cloudy", "shadow", "fluorescent", "tungsten",
    };
    struct target_state *state = backend->private_data;
    struct ngcd_rk_image_readback hardware;
    struct ngcd_image_state image;
    size_t index;
    int count;
    if (state == NULL || !backend->state.camera_running ||
        ngcd_rk_image_read(&state->graph, &hardware) != 0)
        return -1;
    memset(&image, 0, sizeof(image));
    if (hardware.exposure_automatic) {
        memcpy(image.exposure, "-1", sizeof("-1"));
    } else {
        count = snprintf(image.exposure, sizeof(image.exposure), "%.7g",
                         (double)hardware.exposure_seconds);
        if (count <= 0 || (size_t)count >= sizeof(image.exposure))
            return -1;
    }
    if (hardware.iso_automatic) {
        memcpy(image.iso, "auto", sizeof("auto"));
    } else {
        count = snprintf(image.iso, sizeof(image.iso), "iso%u", hardware.iso);
        if (count <= 0 || (size_t)count >= sizeof(image.iso))
            return -1;
    }
    if (hardware.white_balance_automatic) {
        memcpy(image.white_balance, "auto", sizeof("auto"));
    } else {
        for (index = 0U;
             index < sizeof(white_balance_values) /
                         sizeof(white_balance_values[0]);
             ++index)
            if (hardware.white_balance_kelvin == white_balance_values[index])
                break;
        if (index < sizeof(white_balance_values) /
                        sizeof(white_balance_values[0])) {
            memcpy(image.white_balance, white_balance_names[index],
                   strlen(white_balance_names[index]) + 1U);
        } else {
            count = snprintf(image.white_balance, sizeof(image.white_balance),
                             "%u", hardware.white_balance_kelvin);
            if (count <= 0 || (size_t)count >= sizeof(image.white_balance))
                return -1;
        }
    }
    count = snprintf(image.exposure_compensation,
                     sizeof(image.exposure_compensation), "%d",
                     hardware.exposure_compensation);
    if (count <= 0 ||
        (size_t)count >= sizeof(image.exposure_compensation) ||
        hardware.flicker < NGCD_RK_FLICKER_OFF ||
        hardware.flicker > NGCD_RK_FLICKER_60HZ || hardware.effect > 5U)
        return -1;
    image.brightness = hardware.brightness;
    image.contrast = hardware.contrast;
    image.saturation = hardware.saturation;
    image.hue = hardware.hue;
    image.sharpness = hardware.sharpness;
    image.noise_reduction = hardware.noise_reduction;
    memcpy(image.anti_flicker, flicker_names[hardware.flicker],
           strlen(flicker_names[hardware.flicker]) + 1U);
    memcpy(image.effect, effect_names[hardware.effect],
           strlen(effect_names[hardware.effect]) + 1U);
    /* A compensated Night preview deliberately differs from the accepted
     * capture settings.  Keep GET semantic while that transient override is
     * active; otherwise a poll can feed the preview ISO/exposure back into
     * persistent UI state. */
    if (state->transient_exposure_valid)
        memcpy(image.exposure, state->image_desired.exposure,
               strlen(state->image_desired.exposure) + 1U);
    if (state->transient_iso_valid)
        memcpy(image.iso, state->image_desired.iso,
               strlen(state->image_desired.iso) + 1U);
    backend->state.image = image;
    return 0;
}

static int target_reapply_integer(struct ngcd_backend *backend,
                                  uint32_t dirty, uint32_t bit,
                                  const char *type, int value)
{
    char text[16];
    int length;
    if ((dirty & bit) == 0U)
        return 0;
    length = snprintf(text, sizeof(text), "%d", value);
    if (length < 1 || (size_t)length >= sizeof(text))
        return -1;
    return target_set_image(backend, type, text, false);
}

static int target_reapply_image_state(struct ngcd_backend *backend,
                                      uint32_t dirty)
{
    struct target_state *state = backend->private_data;
    const struct ngcd_image_state image = state->image_desired;
    if ((dirty & IMAGE_DIRTY_ISO) != 0U &&
        target_set_image(backend, "iso", image.iso, false) != 0)
        return -1;
    if ((dirty & IMAGE_DIRTY_EXPOSURE) != 0U &&
        target_set_image(backend, "exp", image.exposure, false) != 0)
        return -1;
    if ((dirty & IMAGE_DIRTY_EV) != 0U &&
        target_set_image(backend, "ev", image.exposure_compensation,
                         false) != 0)
        return -1;
    if ((dirty & IMAGE_DIRTY_WHITE_BALANCE) != 0U &&
        target_set_image(backend, "wb", image.white_balance, false) != 0)
        return -1;
    if (target_reapply_integer(backend, dirty, IMAGE_DIRTY_BRIGHTNESS,
                               "brightness", image.brightness) != 0 ||
        target_reapply_integer(backend, dirty, IMAGE_DIRTY_CONTRAST,
                               "contrast", image.contrast) != 0 ||
        target_reapply_integer(backend, dirty, IMAGE_DIRTY_SATURATION,
                               "saturation", image.saturation) != 0 ||
        target_reapply_integer(backend, dirty, IMAGE_DIRTY_HUE,
                               "hue", image.hue) != 0 ||
        target_reapply_integer(backend, dirty, IMAGE_DIRTY_SHARPNESS,
                               "sharpness", image.sharpness) != 0 ||
        target_reapply_integer(backend, dirty, IMAGE_DIRTY_NOISE_REDUCTION,
                               "3dnr", image.noise_reduction) != 0)
        return -1;
    if ((dirty & IMAGE_DIRTY_ANTI_FLICKER) != 0U &&
        target_set_image(backend, "antiflicker", image.anti_flicker,
                         false) != 0)
        return -1;
    if ((dirty & IMAGE_DIRTY_EFFECT) != 0U &&
        target_set_image(backend, "imgeffect", image.effect, false) != 0)
        return -1;
    return 0;
}

static int read_raw_capture_count(const char *path, int *count)
{
    char buffer[32];
    char *end;
    long value;
    int descriptor;
    ssize_t bytes;
    if (path == NULL || count == NULL)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            *count = 0;
            return 0;
        }
        return -1;
    }
    bytes = read(descriptor, buffer, sizeof(buffer) - 1U);
    (void)close(descriptor);
    if (bytes <= 0)
        return -1;
    buffer[bytes] = '\0';
    errno = 0;
    value = strtol(buffer, &end, 10);
    if (errno != 0 || end == buffer || value < 0 || value > 32)
        return -1;
    *count = (int)value;
    return 0;
}

static int pending_raw_capture(int *count)
{
    int first;
    int second;
    if (read_raw_capture_count("/tmp/.capture_cnt_c0", &first) != 0 ||
        read_raw_capture_count("/tmp/.capture_cnt_c1", &second) != 0 ||
        first != second)
        return -1;
    if (first == 0) {
        *count = 0;
        return 0;
    }
    *count = first;
    return 1;
}

static int consume_night_stack_count(int *count)
{
    static const char path[] = "/tmp/calf-night-stack-count";
    int result = read_raw_capture_count(path, count);
    if (result != 0)
        return -1;
    if (*count != 0 && *count != 2 && *count != 4 && *count != 8 &&
        *count != 16 && *count != 24)
        return -1;
    if (*count != 0 && unlink(path) != 0 && errno != ENOENT)
        return -1;
    return *count == 0 ? 0 : 1;
}

static void read_capture_trace(char trace[64])
{
    static const char path[] = "/tmp/calf-capture-trace";
    struct timespec now;
    ssize_t bytes;
    size_t index;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    trace[0] = '\0';
    if (descriptor >= 0) {
        bytes = read(descriptor, trace, 63U);
        (void)close(descriptor);
        if (bytes > 0) {
            trace[(size_t)bytes] = '\0';
            while (bytes > 0 &&
                   (trace[(size_t)bytes - 1U] == '\n' ||
                    trace[(size_t)bytes - 1U] == '\r'))
                trace[(size_t)--bytes] = '\0';
            for (index = 0U; index < (size_t)bytes; ++index) {
                char byte = trace[index];
                if (!((byte >= 'a' && byte <= 'z') ||
                      (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '-' ||
                      byte == '_' || byte == '.')) {
                    trace[0] = '\0';
                    break;
                }
            }
        }
    }
    if (trace[0] == '\0') {
        long seconds = clock_gettime(CLOCK_REALTIME, &now) == 0
                           ? now.tv_sec : 0L;
        (void)snprintf(trace, 64U, "direct-%ld-%ld",
                       (long)getpid(), seconds);
    }
}

static void capture_tracef(const char *trace, const char *stage,
                           const char *result, const char *format, ...)
{
    static const char log_path[] =
        "/mnt/mmcblk1p1/DCIM/calf-capture.log";
    struct timespec now;
    char line[768];
    size_t used = 0U;
    size_t length;
    va_list arguments;
    int count;
    int descriptor;
    long seconds = 0L;
    long milliseconds = 0L;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        seconds = now.tv_sec;
        milliseconds = now.tv_nsec / 1000000L;
    }
    count = snprintf(line, sizeof(line),
                     "ngcd: capture[%s] ts=%ld.%03ld stage=%s result=%s",
                     trace, seconds, milliseconds, stage, result);
    if (count < 0)
        return;
    used = (size_t)count < sizeof(line) ? (size_t)count : sizeof(line) - 1U;
    if (format != NULL && format[0] != '\0') {
        if (used + 1U < sizeof(line))
            line[used++] = ' ';
        if (used < sizeof(line) - 1U) {
            va_start(arguments, format);
            count = vsnprintf(line + used, sizeof(line) - used,
                              format, arguments);
            va_end(arguments);
            if (count > 0) {
                size_t added = (size_t)count;
                if (added >= sizeof(line) - used)
                    added = sizeof(line) - used - 1U;
                used += added;
            }
        }
    }
    if (used + 1U < sizeof(line))
        line[used++] = '\n';
    line[used] = '\0';
    (void)fwrite(line, 1U, used, stderr);
    (void)fflush(stderr);
    descriptor = open(log_path,
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                      0644);
    if (descriptor < 0)
        return;
    length = 0U;
    while (length < used) {
        ssize_t written = write(descriptor, line + length, used - length);
        if (written <= 0)
            break;
        length += (size_t)written;
    }
    (void)close(descriptor);
}

/* Replace the already-written post-ISP Night fallback with a JPEG produced
 * from the linear Bayer stack.  The fallback remains untouched unless the
 * complete offline AIQ/AVS transaction and live-preview restoration succeed. */
static int target_offline_raw_snapshot(
    struct ngcd_backend *backend, struct target_state *state,
    unsigned int count, unsigned int iso, char directory[2][128],
    const char *temporary, const struct ngcd_rk_exif_metadata *metadata,
    const char *trace)
{
    static const char *const sensor_name[2] = {
        "m00_imx577 2-001a", "m01_imx577 3-001a"
    };
    struct ngcd_rkraw_buffer raw[2];
    struct ngcd_rkraw_metadata raw_metadata[2];
    struct ngcd_rk_aiq_wb_gain white_balance[2];
    uint32_t exposure_register[2] = {1U, 1U};
    uint32_t gain_register[2] = {1U, 1U};
    uint32_t format[2];
    void *raw_data[2];
    char located[2][128];
    char offline_path[NGCD_PATH_MAX];
    char rkraw_path[2][NGCD_PATH_MAX];
    unsigned int input;
    unsigned int pass;
    bool live_stopped = false;
    bool offline_started = false;
    bool offline_written = false;
    int written;
    int result = 1;

    memset(raw, 0, sizeof(raw));
    memset(raw_metadata, 0, sizeof(raw_metadata));
    memset(white_balance, 0, sizeof(white_balance));
    memset(located, 0, sizeof(located));
    memset(rkraw_path, 0, sizeof(rkraw_path));
    written = snprintf(offline_path, sizeof(offline_path), "%s.offline",
                       temporary);
    if (written <= 0 || (size_t)written >= sizeof(offline_path))
        return 1;
    if (ngcd_rk_graph_read_white_balance(&state->graph, white_balance) != 0) {
        capture_tracef(trace, "offline-metadata", "fallback",
                       "white balance readback failed", 0);
        return 1;
    }
    (void)ngcd_rk_image_query_sensor_registers(
        &state->graph, exposure_register, gain_register);
    for (input = 0U; input < 2U; ++input) {
        const char *selected = directory[input];
        if (selected[0] == '\0') {
            if (ngcd_rkraw_latest_capture(
                    "/tmp/capture_image", (int)input, located[input],
                    sizeof(located[input])) != 0) {
                capture_tracef(trace, "offline-raw-locate", "fallback",
                               "sensor=%u", input);
                goto done;
            }
            selected = located[input];
        }
        raw_metadata[input].frame_id = UINT32_C(0x40000000);
        raw_metadata[input].exposure_seconds = (float)count * 0.5f;
        raw_metadata[input].gain = (float)iso / 100.0f;
        raw_metadata[input].exposure_register = exposure_register[input];
        raw_metadata[input].gain_register = gain_register[input];
        raw_metadata[input].white_balance_red = white_balance[input].red;
        raw_metadata[input].white_balance_blue = white_balance[input].blue;
        raw_metadata[input].iso = iso;
        if (ngcd_rkraw_build_stack(
                selected, count, sensor_name[input], &raw_metadata[input],
                &raw[input]) != 0) {
            capture_tracef(trace, "offline-raw-stack", "fallback",
                           "sensor=%u count=%u", input, count);
            goto done;
        }
        format[input] = raw[input].format;
        written = snprintf(rkraw_path[input], sizeof(rkraw_path[input]),
                           "/tmp/calf-offline-%ld-%u.rkraw", (long)getpid(),
                           input);
        if (written <= 0 || (size_t)written >= sizeof(rkraw_path[input]) ||
            ngcd_rkraw_write_file(&raw[input], rkraw_path[input]) != 0) {
            capture_tracef(trace, "offline-rkraw", "fallback",
                           "sensor=%u", input);
            goto done;
        }
        raw_data[input] = rkraw_path[input];
    }
    capture_tracef(trace, "offline-raw-stack", "ok",
                   "count=%u bytes=%zu+%zu", count,
                   raw[0].raw_size, raw[1].raw_size);
    if (ngcd_rk_display_histogram_suspend(&state->display) != 0) {
        capture_tracef(trace, "offline-transition", "fallback",
                       "histogram suspend failed", 0);
        goto done;
    }
    target_graph_stop(state);
    backend->state.camera_running = false;
    live_stopped = true;
    if (ngcd_rk_graph_start_offline_in_system(
            &state->graph, &state->api, state->target, &state->profile,
            format, white_balance) != 0) {
        capture_tracef(trace, "offline-graph", "failed", "start", 0);
        goto restore;
    }
    offline_started = true;
    /* The AVS mesh finishes asynchronously after group startup.  Prime it
     * with one non-blocking pair, then require and drain the second warm-up.
     * Only the third, identically exposed output is encoded. */
    for (pass = 0U; pass < 3U; ++pass) {
        for (input = 0U; input < 2U; ++input) {
            if (ngcd_rkraw_set_frame_id(
                    &raw[input], UINT32_C(0x40000000) + pass) != 0 ||
                ngcd_rkraw_write_file(
                    &raw[input], rkraw_path[input]) != 0) {
                capture_tracef(trace, "offline-rkraw", "failed",
                               "sensor=%u pass=%u", input, pass + 1U);
                goto restore;
            }
        }
        if (ngcd_rk_graph_offline_enqueue(
                &state->graph, raw_data, pass == 1U) != 0) {
            capture_tracef(trace, "offline-feed", "failed", "pass=%u",
                           pass + 1U);
            goto restore;
        }
    }
    if (ngcd_rk_graph_snapshot(&state->graph, offline_path, metadata) != 0) {
        capture_tracef(trace, "offline-jpeg", "failed", "", 0);
        goto restore;
    }
    offline_written = true;
    capture_tracef(trace, "offline-jpeg", "ok", "passes=3", 0);

restore:
    if (offline_started)
        target_graph_stop(state);
    if (live_stopped && target_restore_graph(
            backend, state, &state->profile, state->image_dirty) != 0) {
        uint32_t safe_dirty =
            state->image_dirty & ~IMAGE_DIRTY_WHITE_BALANCE;
        capture_tracef(trace, "offline-restore", "failed",
                       "retry_without_wb=1", 0);
        /* A control readback failure must not strand the camera with its live
         * graph stopped. Retry a degraded recovery without manual WB; keep
         * reporting the snapshot failure, but leave the API operational so
         * the UI can change WB or retry instead of returning backend errors
         * forever. */
        if (target_restore_graph(
                backend, state, &state->profile, safe_dirty) == 0)
            capture_tracef(trace, "offline-restore", "degraded",
                           "white_balance=auto", 0);
        else
            capture_tracef(trace, "offline-restore", "fatal", "", 0);
        result = -1;
        goto done;
    }
    if (offline_written) {
        if (rename(offline_path, temporary) != 0) {
            capture_tracef(trace, "offline-publish", "fallback",
                           "errno=%d", errno);
            (void)unlink(offline_path);
        } else {
            capture_tracef(trace, "offline-publish", "ok", "", 0);
            result = 0;
        }
    }
done:
    if (!offline_written || result != 0)
        (void)unlink(offline_path);
    for (input = 0U; input < 2U; ++input)
        ngcd_rkraw_free(&raw[input]);
    for (input = 0U; input < 2U; ++input) {
        if (rkraw_path[input][0] != '\0')
            (void)unlink(rkraw_path[input]);
    }
    return result;
}

static int target_snapshot(struct ngcd_backend *backend,
                           char *filename, size_t size)
{
    struct target_state *state = backend->private_data;
    struct ngcd_storage_info storage;
    struct timespec now;
    struct ngcd_rk_exif_metadata metadata;
    unsigned int iso;
    unsigned int measured_iso = 0U;
    float exposure;
    float measured_exposure = 0.0f;
    bool exposure_automatic;
    char temporary[NGCD_PATH_MAX];
    char final_path[NGCD_PATH_MAX];
    char basename[96];
    char exif_datetime[20];
    char raw_output_directory[2][128];
    struct tm local_time;
    int raw_count = 0;
    int raw_pending;
    int stack_count = 0;
    int stack_pending;
    char trace[64];
    read_capture_trace(trace);
    memset(&storage, 0, sizeof(storage));
    capture_tracef(trace, "request", "begin", "", 0);
    if (state == NULL || filename == NULL || size == 0U) {
        capture_tracef(trace, "preflight", "failed",
                       "invalid arguments state=%d filename=%d size=%zu",
                       state != NULL, filename != NULL, size);
        return -1;
    }
    if (!backend->state.camera_running || backend->state.recording ||
        backend->state.playback || state->transient_exposure_valid ||
        state->transient_iso_valid) {
        capture_tracef(trace, "preflight", "failed",
                       "running=%d recording=%d playback=%d transient_exp=%d transient_iso=%d",
                       backend->state.camera_running, backend->state.recording,
                       backend->state.playback,
                       state->transient_exposure_valid,
                       state->transient_iso_valid);
        return -1;
    }
    if (ngcd_storage_read_status(&storage) != 0 || storage.read_only ||
        storage.free_bytes < UINT64_C(64) * 1024U * 1024U) {
        capture_tracef(trace, "storage", "failed",
                       "read_only=%d free_bytes=%llu", storage.read_only,
                       (unsigned long long)storage.free_bytes);
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0 ||
        localtime_r(&now.tv_sec, &local_time) == NULL ||
        strftime(exif_datetime, sizeof(exif_datetime),
                 "%Y:%m:%d %H:%M:%S", &local_time) != 19U) {
        capture_tracef(trace, "clock", "failed", "errno=%d", errno);
        return -1;
    }
    memset(&metadata, 0, sizeof(metadata));
    memcpy(metadata.datetime, exif_datetime, sizeof(exif_datetime));
    if (parse_iso(backend->state.image.iso, &iso) != 0 ||
        parse_exposure(backend->state.image.exposure, &exposure,
                       &exposure_automatic) != 0) {
        capture_tracef(trace, "image-state", "failed", "exp=%s iso=%s",
                       backend->state.image.exposure,
                       backend->state.image.iso);
        return -1;
    }
    raw_pending = pending_raw_capture(&raw_count);
    stack_pending = consume_night_stack_count(&stack_count);
    if (raw_pending < 0 || stack_pending < 0 ||
        (stack_pending > 0 && iso == 0U) ||
        (raw_pending > 0 && stack_pending > 0 && raw_count != stack_count)) {
        capture_tracef(trace, "triggers", "failed",
                       "raw_pending=%d raw_count=%d stack_pending=%d stack_count=%d auto=%d iso=%u",
                       raw_pending, raw_count, stack_pending, stack_count,
                       exposure_automatic, iso);
        return -1;
    }
    if (stack_pending > 0) {
        /* The coordinator's one-shot stack marker owns the actual sensor
         * exposure.  Reassert it here so a concurrent UI preview refresh
         * cannot replace the half-second capture value with the saved total
         * exposure (for example 4 s) between setup and shutter execution. */
        exposure = 0.5f;
        exposure_automatic = false;
    }
    capture_tracef(trace, "triggers", "ok",
                   "raw_pending=%d raw_count=%d stack_count=%d exp=%.7g iso=%u",
                   raw_pending, raw_count, stack_count,
                   (double)exposure, iso);

    /* Reapply the complete manual pair now that the snapshot coordinator has
     * installed the slower sensor timing.  AIQ can accept a 0.5-second range
     * at 30 fps while its applied exposure remains clamped to about 1/30;
     * querying the live result prevents that preview exposure/ISO from
     * leaking into a Night stack. */
    if (!exposure_automatic && iso != 0U) {
        capture_tracef(trace, "exposure-apply", "begin",
                       "exp=%.7g iso=%u", (double)exposure, iso);
        if (ngcd_rk_image_set_exposure_iso(
                &state->graph, exposure, false, iso,
                &measured_exposure, &measured_iso) != 0 ||
            measured_exposure != exposure || measured_iso != iso ||
            wait_for_exposure_iso(state, exposure, iso,
                                  30U, 100000000L, &measured_exposure,
                                  &measured_iso) != 0) {
            fprintf(stderr,
                    "ngcd: snapshot exposure did not apply %.7g s ISO %u "
                    "(last %.7g s ISO %u)\n",
                    (double)exposure, iso,
                    (double)measured_exposure, measured_iso);
            capture_tracef(trace, "exposure-apply", "failed",
                           "wanted_exp=%.7g wanted_iso=%u measured_exp=%.7g measured_iso=%u",
                           (double)exposure, iso,
                           (double)measured_exposure, measured_iso);
            return -1;
        }
        capture_tracef(trace, "exposure-apply", "ok",
                       "measured_exp=%.7g measured_iso=%u",
                       (double)measured_exposure, measured_iso);
    }
    metadata.iso = iso;
    if (!exposure_automatic) {
        if (exposure_rational(exposure, &metadata.exposure_numerator,
                              &metadata.exposure_denominator) != 0) {
            capture_tracef(trace, "exif", "failed",
                           "invalid exposure=%.7g", (double)exposure);
            return -1;
        }
    }
    if (!exposure_automatic && iso != 0U) {
        metadata.iso = measured_iso;
        if (exposure_rational(measured_exposure,
                              &metadata.exposure_numerator,
                              &metadata.exposure_denominator) != 0) {
            capture_tracef(trace, "exif", "failed",
                           "invalid measured_exposure=%.7g",
                           (double)measured_exposure);
            return -1;
        }
    } else if ((iso == 0U || exposure_automatic) &&
        ngcd_rk_image_query_exposure(&state->graph, &measured_exposure,
                                     &measured_iso) == 0) {
        if (iso == 0U)
            metadata.iso = measured_iso;
        if (exposure_automatic)
            (void)exposure_rational(measured_exposure,
                                    &metadata.exposure_numerator,
                                    &metadata.exposure_denominator);
    }
    if (ngcd_storage_media_paths(
            storage.location, "jpg", &state->media_sequence,
            basename, sizeof(basename), temporary, sizeof(temporary),
            final_path, sizeof(final_path)) != 0 || strlen(basename) >= size) {
        capture_tracef(trace, "media-path", "failed", "", 0);
        return -1;
    }
    capture_tracef(trace, "media-path", "ok", "filename=%s", basename);
    if (stack_pending > 0 &&
        exposure_rational((float)stack_count * 0.5f,
                          &metadata.exposure_numerator,
                          &metadata.exposure_denominator) != 0) {
        capture_tracef(trace, "exif", "failed",
                       "invalid stack_count=%d", stack_count);
        return -1;
    }
    if (raw_pending > 0) {
        capture_tracef(trace, "raw-capture", "begin", "count=%d", raw_count);
        if (ngcd_rk_graph_capture_raw(&state->graph, raw_count, "/tmp",
                                     raw_output_directory) != 0) {
            capture_tracef(trace, "raw-capture", "failed", "count=%d",
                           raw_count);
            return -1;
        }
        capture_tracef(trace, "raw-capture", "ok", "count=%d", raw_count);
    }
    if (stack_pending > 0 && raw_pending == 0) {
        capture_tracef(trace, "frame-stack", "begin", "count=%d", stack_count);
        if (ngcd_rk_graph_stack_snapshot(&state->graph, stack_count) != 0) {
            capture_tracef(trace, "frame-stack", "failed", "count=%d",
                           stack_count);
            return -1;
        }
        capture_tracef(trace, "frame-stack", "ok", "count=%d", stack_count);
    }
    if (ngcd_rk_graph_activate_encoder(
            &state->graph, &backend->state.encoder[0]) != 0) {
        capture_tracef(trace, "jpeg-encoder", "failed", "", 0);
        return -1;
    }
    capture_tracef(trace, "jpeg-write", "begin", "temporary=%s", temporary);
    if (ngcd_rk_graph_snapshot(&state->graph, temporary, &metadata) != 0) {
        capture_tracef(trace, "jpeg-write", "failed", "temporary=%s",
                       temporary);
        return -1;
    }
    if (stack_pending > 0 && raw_pending > 0) {
        int offline_result = target_offline_raw_snapshot(
            backend, state, (unsigned int)stack_count, metadata.iso,
            raw_output_directory, temporary, &metadata, trace);
        if (offline_result < 0) {
            (void)unlink(temporary);
            capture_tracef(trace, "offline", "failed",
                           "live graph restoration failed", 0);
            return -1;
        }
        capture_tracef(trace, "offline", offline_result == 0 ? "ok" :
                       "fallback", "", 0);
    }
    if (publish_media(temporary, final_path) != 0) {
        (void)unlink(temporary);
        capture_tracef(trace, "publish", "failed", "final=%s errno=%d",
                       final_path, errno);
        return -1;
    }
    memcpy(filename, basename, strlen(basename) + 1U);
    capture_tracef(trace, "request", "ok", "filename=%s", basename);
    return 0;
}

static int target_finish_recording(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    uint64_t duration;
    if (state == NULL || !backend->state.recording)
        return -1;
    if (ngcd_rk_graph_record_duration(&state->graph, &duration) != 0)
        goto fail;
    /* A stop can race a just-completed split before the replacement segment
     * receives its first IDR.  The previous segment is already durable; an
     * empty successor is not an error and must not be published. */
    if (duration == 0U) {
        ngcd_rk_graph_record_abort(&state->graph);
        if (state->recording_temporary[0] != '\0')
            (void)unlink(state->recording_temporary);
        backend->state.recording = false;
        backend->state.recording_started_ns = 0U;
        state->recording_temporary[0] = '\0';
        state->recording_final[0] = '\0';
        state->recording_storage_check_ns = 0U;
        state->recording_size_limit = 0U;
        state->recording_time_limit = 0U;
        state->recording_split_type = RECORDING_SPLIT_NONE;
        return 0;
    }
    if (ngcd_rk_graph_record_stop(&state->graph) != 0)
        goto fail;
    backend->state.recording = false;
    backend->state.recording_started_ns = 0U;
    if (publish_media(state->recording_temporary,
                      state->recording_final) != 0)
        goto fail;
    state->recording_temporary[0] = '\0';
    state->recording_final[0] = '\0';
    state->recording_storage_check_ns = 0U;
    state->recording_size_limit = 0U;
    state->recording_time_limit = 0U;
    state->recording_split_type = RECORDING_SPLIT_NONE;
    return 0;
fail:
    backend->state.recording = false;
    backend->state.recording_started_ns = 0U;
    return -1;
}

static int target_split_recording(struct ngcd_backend *backend)
{
    struct target_state *state = backend->private_data;
    struct ngcd_storage_info storage;
    char basename[96];
    char temporary[NGCD_PATH_MAX];
    char final_path[NGCD_PATH_MAX];
    if (state == NULL || !backend->state.recording ||
        ngcd_storage_read_status(&storage) != 0 || storage.read_only ||
        storage.free_bytes < RECORDING_FREE_RESERVE ||
        ngcd_storage_media_paths(
            storage.location, "mp4", &state->media_sequence,
            basename, sizeof(basename), temporary, sizeof(temporary),
            final_path, sizeof(final_path)) != 0)
        return -1;
    if (ngcd_rk_graph_record_stop(&state->graph) != 0)
        return -1;
    if (publish_media(state->recording_temporary,
                      state->recording_final) != 0)
        return -1;
    memcpy(state->recording_temporary, temporary, strlen(temporary) + 1U);
    memcpy(state->recording_final, final_path, strlen(final_path) + 1U);
    if (ngcd_rk_graph_record_start(&state->graph,
                                   &backend->state.encoder[0],
                                   state->recording_temporary) != 0)
        return -1;
    return 0;
}

static int target_recording(struct ngcd_backend *backend,
                            const char *action, int split,
                            uint64_t size_limit, uint64_t time_limit)
{
    struct target_state *state = backend->private_data;
    struct ngcd_storage_info storage;
    char basename[96];
    bool start;
    if (state == NULL || action == NULL)
        return -1;
    if (strcmp(action, "toggle") == 0)
        start = !backend->state.recording;
    else if (strcmp(action, "start") == 0)
        start = true;
    else if (strcmp(action, "stop") == 0)
        start = false;
    else
        return -1;
    if (!start) {
        int result = target_finish_recording(backend);
        if (result != 0)
            target_abort_recording(backend);
        return result;
    }
    if (backend->state.recording || !backend->state.camera_running ||
        backend->state.playback || split < RECORDING_SPLIT_SIZE ||
        split > RECORDING_SPLIT_NONE || state->encoder_update_pending ||
        (strcmp(backend->state.encoder[0].codec, "H264") != 0 &&
         strcmp(backend->state.encoder[0].codec, "H265") != 0) ||
        ngcd_storage_read_status(&storage) != 0 || storage.read_only ||
        storage.free_bytes < UINT64_C(256) * 1024U * 1024U)
        return -1;
    if (ngcd_storage_media_paths(
            storage.location, "mp4", &state->media_sequence,
            basename, sizeof(basename), state->recording_temporary,
            sizeof(state->recording_temporary), state->recording_final,
            sizeof(state->recording_final)) != 0 ||
        ngcd_rk_graph_record_start(&state->graph,
                                   &backend->state.encoder[0],
                                   state->recording_temporary) != 0) {
        state->recording_temporary[0] = '\0';
        state->recording_final[0] = '\0';
        return -1;
    }
    backend->state.recording = true;
    state->recording_split_type = split;
    state->recording_size_limit =
        split == RECORDING_SPLIT_SIZE
            ? (size_limit != 0U ? size_limit : RECORDING_DEFAULT_SIZE_LIMIT)
            : 0U;
    state->recording_time_limit =
        split == RECORDING_SPLIT_TIME
            ? (time_limit != 0U ? time_limit : RECORDING_DEFAULT_TIME_LIMIT)
            : 0U;
    state->recording_storage_check_ns =
        target_monotonic_nanoseconds() + RECORDING_STORAGE_CHECK_NS;
    return 0;
}

static int target_clear_playback_media(struct ngcd_backend *backend,
                                       struct target_state *state)
{
    if (ngcd_rk_display_histogram_suspend(&state->display) != 0)
        return -1;
    ngcd_rk_playback_close(state->playback);
    state->playback = NULL;
    backend->state.playback_paused = true;
    backend->state.playback_sample_index = 0;
    backend->state.playback_sample_count = 0;
    backend->state.playback_decoder_received = 0U;
    backend->state.playback_decoder_decoded = 0U;
    backend->state.playback_decoder_pending_stream = 0U;
    backend->state.playback_decoder_pending_pictures = 0U;
    backend->state.playback_decoder_errors = 0U;
    backend->state.playback_presented_frames = 0U;
    backend->state.playback_output_errors = 0U;
    backend->state.playback_file_size = 0U;
    backend->state.playback_create_time = 0U;
    backend->state.playback_duration_us = 0U;
    backend->state.playback_codec[0] = '\0';
    backend->state.playback_width = 0;
    backend->state.playback_height = 0;
    backend->state.playback_picture = false;
    return 0;
}

static int target_playback_path_allowed(struct ngcd_backend *backend,
                                        const char *path)
{
    struct ngcd_storage_info storage;
    char prefix[NGCD_PATH_MAX];
    size_t prefix_length;
    off_t file_size;
    int descriptor;
    int count;
    if (path == NULL || path[0] != '/' || strstr(path, "/../") != NULL ||
        backend->ops->storage_status(backend, &storage) != 0)
        return 0;
    count = snprintf(prefix, sizeof(prefix), "%s/DCIM/", storage.location);
    if (count <= 0 || (size_t)count >= sizeof(prefix))
        return 0;
    prefix_length = (size_t)count;
    if (strncmp(path, prefix, prefix_length) != 0 ||
        path[prefix_length] == '\0')
        return 0;
    descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        return 0;
    file_size = lseek(descriptor, 0, SEEK_END);
    return close(descriptor) == 0 && file_size > 0;
}

static void target_publish_playback_media(struct ngcd_backend *backend,
                                          struct target_state *state)
{
    const char *codec = ngcd_rk_playback_codec_name(state->playback);
    size_t samples = ngcd_rk_playback_sample_count(state->playback);
    backend->state.playback_paused =
        ngcd_rk_playback_is_paused(state->playback);
    backend->state.playback_sample_index =
        (int)ngcd_rk_playback_sample_index(state->playback);
    backend->state.playback_sample_count =
        samples > INT_MAX ? INT_MAX : (int)samples;
    backend->state.playback_decoder_received =
        ngcd_rk_playback_decoder_received(state->playback);
    backend->state.playback_decoder_decoded =
        ngcd_rk_playback_decoder_decoded(state->playback);
    backend->state.playback_decoder_pending_stream =
        ngcd_rk_playback_decoder_pending_stream(state->playback);
    backend->state.playback_decoder_pending_pictures =
        ngcd_rk_playback_decoder_pending_pictures(state->playback);
    backend->state.playback_decoder_errors =
        ngcd_rk_playback_decoder_errors(state->playback);
    backend->state.playback_presented_frames =
        ngcd_rk_playback_presented_frames(state->playback);
    backend->state.playback_output_errors =
        ngcd_rk_playback_output_errors(state->playback);
    backend->state.playback_file_size =
        ngcd_rk_playback_file_size(state->playback);
    backend->state.playback_create_time =
        ngcd_rk_playback_create_time(state->playback);
    backend->state.playback_duration_us =
        ngcd_rk_playback_duration_us(state->playback);
    memcpy(backend->state.playback_codec, codec, strlen(codec) + 1U);
    backend->state.playback_width =
        (int)ngcd_rk_playback_width(state->playback);
    backend->state.playback_height =
        (int)ngcd_rk_playback_height(state->playback);
    backend->state.playback_picture =
        ngcd_rk_playback_is_picture(state->playback);
}

static int target_playback(struct ngcd_backend *backend,
                           const char *action, const char *path,
                           int first, int second)
{
    struct target_state *state = backend->private_data;
    uint64_t now = target_monotonic_nanoseconds() / 1000U;
    (void)second;
    if (state == NULL || action == NULL || backend->state.recording)
        return -1;
    if (strcmp(action, "start") == 0) {
        if (backend->state.playback)
            return -1;
        if (backend->state.camera_running) {
            if (ngcd_rk_display_histogram_suspend(&state->display) != 0)
                return -1;
            target_graph_stop(state);
            backend->state.camera_running = false;
        }
        backend->state.playback = true;
        backend->state.playback_paused = true;
        state->playback_restore_pending = true;
        return 0;
    }
    if (strcmp(action, "stop") == 0) {
        int result = 0;
        if (target_clear_playback_media(backend, state) != 0)
            return -1;
        backend->state.playback = false;
        if (state->playback_restore_pending) {
            result = target_restore_graph(backend, state, &state->profile,
                                          state->image_dirty);
            if (result == 0)
                state->playback_restore_pending = false;
        }
        return result;
    }
    if (!backend->state.playback)
        return -1;
    if (strcmp(action, "close") == 0) {
        return target_clear_playback_media(backend, state);
    }
    if (strcmp(action, "open") == 0) {
        struct ngcd_rk_playback *opened = NULL;
        if (!target_playback_path_allowed(backend, path))
            return -1;
        if (target_clear_playback_media(backend, state) != 0)
            return -1;
        if (ngcd_rk_playback_open(&opened, &state->api, state->target,
                                  &state->audio_output, path) != 0) {
            fprintf(stderr, "ngcd: playback open failed: %s\n", path);
            return -1;
        }
        state->playback = opened;
        target_publish_playback_media(backend, state);
        return 0;
    }
    if (state->playback == NULL)
        return -1;
    if (strcmp(action, "pause") == 0)
        return ngcd_rk_playback_pause(state->playback, true, now);
    if (strcmp(action, "resume") == 0)
        return ngcd_rk_playback_pause(state->playback, false, now);
    if (strcmp(action, "toggle") == 0)
        return ngcd_rk_playback_pause(
            state->playback,
            !ngcd_rk_playback_is_paused(state->playback), now);
    if (strcmp(action, "seek") == 0 && first >= 0) {
        size_t requested = (size_t)first;
        size_t sample_count = ngcd_rk_playback_sample_count(state->playback);
        int result;
        if (second > 0) {
            uint64_t scaled = (uint64_t)(unsigned int)first * sample_count /
                              (unsigned int)second;
            requested = scaled >= sample_count && sample_count > 0U
                            ? sample_count - 1U : (size_t)scaled;
        }
        result = ngcd_rk_playback_seek(state->playback, requested, now);
        if (result == 0)
            target_publish_playback_media(backend, state);
        return result;
    }
    return -1;
}

static int target_unavailable_stream(struct ngcd_backend *backend,
                                     const char *service, const char *action,
                                     const char *url)
{
    (void)backend;
    (void)service;
    (void)action;
    (void)url;
    return -1;
}

static int target_unavailable_uvc(struct ngcd_backend *backend, bool enable)
{
    (void)backend;
    (void)enable;
    return -1;
}

static int target_read_imu(struct ngcd_backend *backend,
                           struct ngcd_imu_sample *sample)
{
    struct target_state *state = backend->private_data;
    return state != NULL ? ngcd_imu_read(state->imu, sample) : -1;
}

static int target_calibrate_imu(struct ngcd_backend *backend, int type,
                                bool save, int count)
{
    struct target_state *state = backend->private_data;
    return state != NULL
               ? ngcd_imu_start_calibration(state->imu, type, save, count)
               : -1;
}

static int target_imu_calibration_state(struct ngcd_backend *backend,
                                        int *calibration_state)
{
    struct target_state *state = backend->private_data;
    return state != NULL
               ? ngcd_imu_calibration_state(state->imu, calibration_state)
               : -1;
}

static int target_set_encoder(
    struct ngcd_backend *backend, int channel,
    const struct ngcd_encoder_state *encoder, uint32_t fields)
{
    struct target_state *state = backend->private_data;
    struct ngcd_rk_venc_chn_attr attribute;
    struct ngcd_rk_venc_rc_param rate_control;
    bool validate_hardware;
    if (state == NULL || encoder == NULL || channel < 0 || channel >= 3 ||
        fields == 0U || backend->state.recording ||
        state->encoder_update_pending ||
        ngcd_rk_encoder_attributes(encoder, &attribute, &rate_control) != 0)
        return -1;
    if (memcmp(&backend->state.encoder[channel], encoder,
               sizeof(*encoder)) == 0)
        return 0;
    validate_hardware = channel == 0 && backend->state.camera_running &&
                        state->graph.avs_group_started &&
                        state->graph.avs_channel_started;
    if (validate_hardware &&
        ngcd_rk_graph_validate_encoder(&state->graph, encoder) != 0)
        return -1;
    state->encoder_previous = backend->state.encoder[channel];
    state->encoder_pending_channel = channel;
    target_store_encoder(backend, state, channel, encoder);
    state->encoder_update_pending = validate_hardware;
    return 0;
}

static int target_set_backlight(struct ngcd_backend *backend, int value)
{
    if (ngcd_backlight_write(value) != 0)
        return -1;
    backend->state.backlight = value;
    if (value > 0)
        backend->state.backlight_saved = value;
    return 0;
}

static int target_set_audio(struct ngcd_backend *backend, int input,
                            bool automatic, int volume,
                            bool set_volume)
{
    struct target_state *state = backend->private_data;
    int selected;
    if (state == NULL || input < 0 || input > 2 ||
        volume < 0 || volume > 100)
        return -1;
    if (set_volume) {
        if (input == backend->state.audio_input &&
            ngcd_audio_control_apply(state->audio_control, input,
                                     volume) != 0)
            return -1;
        backend->state.audio_volume[input] = volume;
        return 0;
    }
    selected = input;
    if (automatic &&
        ngcd_audio_control_detect_input(state->audio_control,
                                        &selected) != 0)
        return -1;
    if (backend->state.recording && selected != backend->state.audio_input)
        return -1;
    if (selected != backend->state.audio_input) {
        if (ngcd_audio_control_apply(
                state->audio_control, selected,
                backend->state.audio_volume[selected]) != 0 ||
            ngcd_rk_graph_set_audio_input(&state->graph, selected) != 0)
            return -1;
    }
    backend->state.audio_auto = automatic ? 1 : 0;
    backend->state.audio_input = selected;
    state->next_audio_detect_ns =
        target_monotonic_nanoseconds() + UINT64_C(2000000000);
    return 0;
}

static int target_set_speaker(struct ngcd_backend *backend, int volume)
{
    struct target_state *state = backend->private_data;
    int readback;
    if (state == NULL ||
        ngcd_rk_audio_output_set_volume(&state->audio_output, volume,
                                        &readback) != 0 ||
        readback != volume)
        return -1;
    backend->state.speaker_volume = readback;
    return 0;
}

static int target_unavailable_storage(struct ngcd_backend *backend,
                                      const char *action, const char *argument,
                                      int first, int second, int *result)
{
    struct ngcd_storage_info info;
    char path[NGCD_PATH_MAX];
    int length;
    (void)backend;
    (void)argument;
    if (strcmp(action, "iotest_stor") != 0 ||
        ngcd_storage_read_status(&info) != 0 || info.read_only)
        return -1;
    length = snprintf(path, sizeof(path), "%s/.ngcd-iotest-%d.tmp",
                      info.location, getpid());
    if (length < 0 || (size_t)length >= sizeof(path))
        return -1;
    return ngcd_storage_io_test_file(path, first, second, info.free_bytes,
                                     result);
}

static int target_storage_status(struct ngcd_backend *backend,
                                 struct ngcd_storage_info *info)
{
    (void)backend;
    return ngcd_storage_read_status(info);
}

static int target_wifi_status(struct ngcd_backend *backend,
                              struct ngcd_wifi_info *info)
{
    (void)backend;
    return ngcd_wifi_read_status(info);
}

static int target_wifi_scan(struct ngcd_backend *backend,
                            struct ngcd_wifi_network *networks,
                            size_t capacity, size_t *count)
{
    unsigned int elapsed_ms;
    if (ngcd_wifi_scan_begin() != 0)
        return -1;
    for (elapsed_ms = 0; elapsed_ms < 3000U; elapsed_ms += 5U) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000L};
        (void)target_tick(backend);
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    return ngcd_wifi_scan_results(networks, capacity, count);
}

static int target_usb_ethernet(struct ngcd_backend *backend, const char *port,
                               const char *operating_system, bool enable)
{
    (void)backend;
    return enable ? ngcd_usb_ethernet_set(port, operating_system)
                  : ngcd_usb_ethernet_close();
}

static int target_ethernet_status(struct ngcd_backend *backend,
                                  struct ngcd_ethernet_info *info)
{
    (void)backend;
    return ngcd_ethernet_read_status(info);
}

static int target_power_status(struct ngcd_backend *backend,
                               struct ngcd_power_info *info)
{
    (void)backend;
    return ngcd_power_read_status(info);
}

static int target_system_action(struct ngcd_backend *backend,
                                const char *action)
{
    (void)backend;
    return strcmp(action, "sysinfo") == 0 || strcmp(action, "poweroff") == 0
               ? 0 : -1;
}

static const struct ngcd_backend_ops TARGET_OPS = {
    .start = target_start,
    .stop = target_stop,
    .tick = target_tick,
    .graphics_control_id = target_graphics_control_id,
    .histogram = target_histogram,
    .lcd_screenshot = target_lcd_screenshot,
    .camera_mode = target_camera_mode,
    .set_image = target_set_image,
    .night_preview = target_night_preview,
    .read_image = target_read_image,
    .snapshot = target_snapshot,
    .recording = target_recording,
    .playback = target_playback,
    .stream = target_unavailable_stream,
    .uvc = target_unavailable_uvc,
    .read_imu = target_read_imu,
    .calibrate_imu = target_calibrate_imu,
    .imu_calibration_state = target_imu_calibration_state,
    .set_encoder = target_set_encoder,
    .set_backlight = target_set_backlight,
    .set_audio = target_set_audio,
    .set_speaker = target_set_speaker,
    .storage = target_unavailable_storage,
    .storage_status = target_storage_status,
    .wifi_status = target_wifi_status,
    .wifi_scan = target_wifi_scan,
    .usb_ethernet = target_usb_ethernet,
    .ethernet_status = target_ethernet_status,
    .power_status = target_power_status,
    .system_action = target_system_action,
};

const struct ngcd_backend_ops *ngcd_target_backend_ops(void)
{
    return &TARGET_OPS;
}
