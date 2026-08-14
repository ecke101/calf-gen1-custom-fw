#include "target_internal.h"

void gallery_init(gallery_state_t *gallery)
{
    gallery->paths = (char **)0;
    gallery->count = 0;
    gallery->capacity = 0;
    gallery->index = 0;
    gallery->active = 0;
    gallery->graph_suspended = 0;
    gallery->playback_started = 0;
    gallery->playing = 0;
    gallery->preview_pixels = (uint32_t *)0;
    gallery->preview_width = 0;
    gallery->preview_height = 0;
    gallery->play_ready_ms = 0;
}

static void gallery_clear_paths(gallery_state_t *gallery)
{
    int index;
    for(index = 0; index < gallery->count; ++index)
        free(gallery->paths[index]);
    if(gallery->paths != (char **)0) free(gallery->paths);
    gallery->paths = (char **)0;
    gallery->count = 0;
    gallery->capacity = 0;
    gallery->index = 0;
}

void gallery_destroy(gallery_state_t *gallery)
{
    gallery_clear_paths(gallery);
    if(gallery->preview_pixels != (uint32_t *)0)
        free(gallery->preview_pixels);
    gallery->preview_pixels = (uint32_t *)0;
    gallery->preview_width = 0;
    gallery->preview_height = 0;
}

static int string_compare(const char *left, const char *right)
{
    size_t index = 0;
    while(left[index] != '\0' && left[index] == right[index]) ++index;
    return (unsigned char)left[index] - (unsigned char)right[index];
}

int gallery_path_is_video(const char *path)
{
    size_t length = string_length(path);
    return length >= 4 && path[length - 4] == '.' &&
           (path[length - 3] == 'm' || path[length - 3] == 'M') &&
           (path[length - 2] == 'p' || path[length - 2] == 'P') &&
           path[length - 1] == '4';
}

static const char *gallery_filename(const char *path)
{
    const char *result = path;
    size_t index;
    for(index = 0; path[index] != '\0'; ++index)
        if(path[index] == '/') result = path + index + 1;
    return result;
}

void gallery_clear_preview(calf_ui_t *ui, gallery_state_t *gallery)
{
    calf_ui_set_gallery_preview(ui, (const uint32_t *)0, 0, 0);
    calf_ui_set_gallery_histogram(ui, (const uint32_t *)0, 0);
    if(gallery->preview_pixels != (uint32_t *)0)
        free(gallery->preview_pixels);
    gallery->preview_pixels = (uint32_t *)0;
    gallery->preview_width = 0;
    gallery->preview_height = 0;
}

static int gallery_reserve(gallery_state_t *gallery, int capacity)
{
    char **paths;
    int new_capacity;
    int index;
    if(capacity <= gallery->capacity) return 0;
    new_capacity = gallery->capacity > 0
                       ? gallery->capacity : GALLERY_INITIAL_CAPACITY;
    while(new_capacity < capacity) {
        if(new_capacity > 1073741823) return -1;
        new_capacity *= 2;
    }
    if((size_t)new_capacity > (size_t)-1 / sizeof(*paths)) return -1;
    paths = (char **)malloc((size_t)new_capacity * sizeof(*paths));
    if(paths == (char **)0) return -1;
    for(index = 0; index < gallery->count; ++index)
        paths[index] = gallery->paths[index];
    if(gallery->paths != (char **)0) free(gallery->paths);
    gallery->paths = paths;
    gallery->capacity = new_capacity;
    return 0;
}

int gallery_add_path(gallery_state_t *gallery, const char *path)
{
    char *copy;
    size_t length = string_length(path);
    size_t index;
    if(length + 1 > GALLERY_PATH_SIZE ||
       gallery_reserve(gallery, gallery->count + 1) != 0)
        return -1;
    copy = (char *)malloc(length + 1);
    if(copy == (char *)0) return -1;
    for(index = 0; index <= length; ++index) copy[index] = path[index];
    gallery->paths[gallery->count] = copy;
    ++gallery->count;
    return 0;
}

int gallery_sort_paths(gallery_state_t *gallery)
{
    char **source;
    char **destination;
    char **scratch;
    int width;
    int start;
    int index;
    if(gallery->count < 2) return 0;
    if((size_t)gallery->count > (size_t)-1 / sizeof(*scratch)) return -1;
    scratch = (char **)malloc((size_t)gallery->count * sizeof(*scratch));
    if(scratch == (char **)0) return -1;
    source = gallery->paths;
    destination = scratch;
    width = 1;
    while(width < gallery->count) {
        for(start = 0; start < gallery->count; start += width * 2) {
            int left = start;
            int middle = start + width;
            int right = middle;
            int end = start + width * 2;
            int output = start;
            if(middle > gallery->count) middle = gallery->count;
            if(right > gallery->count) right = gallery->count;
            if(end > gallery->count) end = gallery->count;
            while(left < middle && right < end) {
                if(string_compare(source[left], source[right]) >= 0)
                    destination[output++] = source[left++];
                else
                    destination[output++] = source[right++];
            }
            while(left < middle) destination[output++] = source[left++];
            while(right < end) destination[output++] = source[right++];
        }
        {
            char **swap = source;
            source = destination;
            destination = swap;
        }
        if(width > gallery->count / 2) break;
        width *= 2;
    }
    if(source != gallery->paths)
        for(index = 0; index < gallery->count; ++index)
            gallery->paths[index] = source[index];
    free(scratch);
    return 0;
}

void gallery_remove_path(gallery_state_t *gallery, int index)
{
    int move;
    if(index < 0 || index >= gallery->count) return;
    free(gallery->paths[index]);
    for(move = index; move + 1 < gallery->count; ++move)
        gallery->paths[move] = gallery->paths[move + 1];
    --gallery->count;
    gallery->paths[gallery->count] = (char *)0;
    if(gallery->count == 0) gallery->index = 0;
    else if(gallery->index >= gallery->count)
        gallery->index = gallery->count - 1;
}

static int gallery_scan(gallery_state_t *gallery)
{
    char response[HTTP_BUFFER_SIZE];
    char request_path[96];
    int offset = 0;
    int total = 0;
    gallery_clear_paths(gallery);
    do {
        const char *cursor;
        int page_count = 0;
        size_t request_used = 0;
        request_path[0] = '\0';
        buffer_append(request_path, sizeof(request_path), &request_used,
                      "/camera/v2/media?offset=");
        buffer_append_uint(request_path, sizeof(request_path), &request_used,
                           (unsigned)offset);
        buffer_append(request_path, sizeof(request_path), &request_used,
                      "&limit=64");
        if(request_used + 1 >= sizeof(request_path) ||
           http_request("GET", request_path, (const char *)0,
                        response, sizeof(response)) != 0 ||
           !response_code_ok(response) ||
           parse_integer_after(response, "\"total\"", &total) != 0 ||
           total < 0 || total > 10000)
            goto fail;
        cursor = find_text(response, "\"items\":[");
        if(cursor == (const char *)0) goto fail;
        cursor += string_length("\"items\":[");
        while((cursor = find_text(cursor, "\"path\"")) !=
              (const char *)0) {
            char path[GALLERY_PATH_SIZE];
            const char *end = find_text(cursor, "}");
            if(end == (const char *)0 ||
               parse_scalar_after(cursor, "\"path\"", path,
                                  sizeof(path)) != 0 ||
               gallery_add_path(gallery, path) != 0)
                goto fail;
            ++page_count;
            cursor = end + 1;
        }
        if(page_count == 0 && offset < total) goto fail;
        offset += page_count;
    } while(offset < total);
    return gallery->count == total ? 0 : -1;
fail:
    gallery_clear_paths(gallery);
    return -1;
}

void gallery_sync_ui(calf_ui_t *ui, const gallery_state_t *gallery)
{
    if(gallery->count <= 0) {
        calf_ui_set_gallery(ui, "", 0, 0, 0, 0);
        return;
    }
    calf_ui_set_gallery(ui, gallery_filename(gallery->paths[gallery->index]),
                        gallery_path_is_video(
                            gallery->paths[gallery->index]),
                        gallery->index, gallery->count, gallery->playing);
}


static int api_playback_action(const char *action)
{
    char body[64];
    size_t used = 0;
    body[0] = '\0';
    buffer_append(body, sizeof(body), &used, "{\"action\":\"");
    buffer_append(body, sizeof(body), &used, action);
    buffer_append(body, sizeof(body), &used, "\"}");
    return api_post_action("/camera/v2/playback", body);
}

static int api_playback_open(const char *path)
{
    char body[384];
    size_t used = 0;
    body[0] = '\0';
    buffer_append(body, sizeof(body), &used,
                  "{\"action\":\"open\",\"filepath\":\"");
    buffer_append(body, sizeof(body), &used, path);
    buffer_append(body, sizeof(body), &used, "\",\"preview\":1}");
    if(used + 1 >= sizeof(body)) return -1;
    return api_post_action("/camera/v2/playback", body);
}

typedef struct {
    int running;
    int paused;
    int sample_index;
    int sample_count;
} playback_state_t;

static int api_playback_state(playback_state_t *state)
{
    char response[HTTP_BUFFER_SIZE];
    state->running = -1;
    state->paused = -1;
    state->sample_index = -1;
    state->sample_count = -1;
    if(http_request("GET", "/camera/v2/playback", (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response) ||
       parse_integer_after(response, "\"running\"", &state->running) != 0)
        return -1;
    (void)parse_integer_after(response, "\"is_pause\"", &state->paused);
    (void)parse_integer_after(response, "\"sample_index\"",
                              &state->sample_index);
    (void)parse_integer_after(response, "\"sample_count\"",
                              &state->sample_count);
    return 0;
}

void api_poll_gallery_info(calf_ui_t *ui, gallery_state_t *gallery)
{
    char response[HTTP_BUFFER_SIZE];
    int running;
    int paused;
    int sample_index;
    int sample_count;
    int duration_milliseconds;
    int playing = gallery->playing;
    int timing_known = 0;
    int position_seconds = 0;
    int duration_seconds = 0;
    if(gallery->count > 0 &&
       gallery_path_is_video(gallery->paths[gallery->index]) &&
       http_request("GET", "/camera/v2/playback", (const char *)0,
                    response, sizeof(response)) == 0 &&
       response_code_ok(response)) {
        if(parse_integer_after(response, "\"running\"", &running) == 0 &&
           parse_integer_after(response, "\"is_pause\"", &paused) == 0)
            playing = running != 0 && paused == 0;
        if(parse_integer_after(response, "\"sample_index\"",
                               &sample_index) == 0 &&
           parse_integer_after(response, "\"sample_count\"",
                               &sample_count) == 0 &&
           parse_milliseconds_after(response, "\"duration\"",
                                    &duration_milliseconds) == 0 &&
           sample_index >= 0 && sample_count > 0) {
            position_seconds = (int)(
                ((uint64_t)(unsigned)sample_index *
                 (uint64_t)(unsigned)duration_milliseconds) /
                (uint64_t)(unsigned)sample_count / 1000u);
            duration_seconds = (duration_milliseconds + 500) / 1000;
            timing_known = 1;
        }
    }
    gallery->playing = playing;
    calf_ui_set_gallery_playback(ui, playing, position_seconds,
                                 duration_seconds, timing_known);

}

int gallery_enter_backend(gallery_state_t *gallery)
{
    gallery->active = 0;
    gallery->graph_suspended = 0;
    gallery->playback_started = 0;
    gallery->playing = 0;
    if(gallery_scan(gallery) != 0) return -1;
    if(gallery->count == 0) {
        gallery->active = 1;
        return 0;
    }
    if(api_playback_action("start") != 0) return -1;
    gallery->graph_suspended = 1;
    gallery->playback_started = 1;
    if(api_playback_open(gallery->paths[gallery->index]) != 0) return -1;
    gallery->active = 1;
    return 0;
}

int gallery_close_backend(gallery_state_t *gallery)
{
    playback_state_t state;
    int result = 0;
    if(gallery->playback_started && gallery->count > 0 &&
       api_playback_action("close") != 0)
        result = -1;
    if(gallery->playback_started && api_playback_action("stop") != 0)
        result = -1;
    gallery->playback_started = 0;
    gallery->playing = 0;
    gallery->active = 0;
    /*
     * Playback close/stop are not reliably idempotent around end-of-file.
     * If either request reports failure but authoritative readback says the
     * player is stopped, teardown has reached the state needed by the camera
     * graph and Gallery exit must not strand the UI.
     */
    if(result != 0 && api_playback_state(&state) == 0 &&
       state.running == 0)
        result = 0;
    return result;
}

int gallery_offset_index(const gallery_state_t *gallery, int offset)
{
    int next;
    if(gallery->count <= 0) return 0;
    offset %= gallery->count;
    next = gallery->index + offset;
    while(next < 0) next += gallery->count;
    while(next >= gallery->count) next -= gallery->count;
    return next;
}

int gallery_move(gallery_state_t *gallery, int offset)
{
    int next;
    if(!gallery->active || gallery->count <= 0 ||
       !gallery->playback_started)
        return -1;
    next = gallery_offset_index(gallery, offset);
    if(next == gallery->index) return 0;
    if(api_playback_action("close") != 0) return -1;
    if(api_playback_open(gallery->paths[next]) != 0) {
        (void)api_playback_open(gallery->paths[gallery->index]);
        return -1;
    }
    gallery->index = next;
    gallery->playing = 0;
    return 0;
}

int gallery_toggle_playback(gallery_state_t *gallery)
{
    playback_state_t state;
    int restart_from_beginning = 0;
    if(!gallery->active || gallery->count <= 0 ||
       !gallery->playback_started ||
       !gallery_path_is_video(gallery->paths[gallery->index]))
        return -1;
    if(api_playback_state(&state) == 0 &&
       (state.running == 0 ||
        (state.paused != 0 && state.sample_count > 0 &&
         state.sample_index >= state.sample_count - 1))) {
        restart_from_beginning = 1;
        (void)api_playback_action("close");
        if(api_playback_open(gallery->paths[gallery->index]) != 0)
            return -1;
    }
    if(api_playback_action("toggle") != 0) return -1;
    gallery->playing = restart_from_beginning ? 1 : !gallery->playing;
    return 0;
}

int gallery_delete_current(gallery_state_t *gallery)
{
    char raw_left[GALLERY_PATH_SIZE];
    char raw_right[GALLERY_PATH_SIZE];
    const char *media_path;
    size_t media_length;
    size_t prefix_length;
    int remove_index;
    if(!gallery->active || gallery->count <= 0 ||
       gallery->index < 0 || gallery->index >= gallery->count)
        return -1;
    remove_index = gallery->index;
    media_path = gallery->paths[remove_index];
    media_length = string_length(media_path);
    raw_left[0] = '\0';
    raw_right[0] = '\0';
    if(!gallery_path_is_video(media_path) && media_length >= 4 &&
       media_length + 3 <= GALLERY_PATH_SIZE) {
        static const char left_suffix[] = "-L.dng";
        static const char right_suffix[] = "-R.dng";
        size_t index;
        prefix_length = media_length - 4;
        for(index = 0; index < prefix_length; ++index) {
            raw_left[index] = media_path[index];
            raw_right[index] = media_path[index];
        }
        string_copy(raw_left + prefix_length,
                    GALLERY_PATH_SIZE - prefix_length, left_suffix);
        string_copy(raw_right + prefix_length,
                    GALLERY_PATH_SIZE - prefix_length, right_suffix);
    }
    if(gallery->playback_started && api_playback_action("close") != 0)
        return -1;
    if(unlink(media_path) != 0) {
        if(gallery->playback_started)
            (void)api_playback_open(media_path);
        return -1;
    }
    if(raw_left[0] != '\0') {
        (void)unlink(raw_left);
        (void)unlink(raw_right);
    }
    sync();
    gallery_remove_path(gallery, remove_index);
    if(gallery->count > 0) {
        if(gallery->playback_started &&
           api_playback_open(gallery->paths[gallery->index]) != 0)
            return -1;
    }
    gallery->playing = 0;
    return 0;
}
