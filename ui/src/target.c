#include "target_internal.h"

#include <stdatomic.h>

static void secure_zero(void *memory, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)memory;
    while(length-- > 0) *bytes++ = 0;
}

_Static_assert(CALF_ACTION_SET_DNR - CALF_ACTION_SET_BRIGHTNESS ==
                   CALF_IMAGE_LEVEL_COUNT - 1,
               "image actions must remain contiguous");
_Static_assert(CALF_ACTION_SET_USB_MIC_VOLUME -
                   CALF_ACTION_SET_BUILTIN_MIC_VOLUME ==
                   CALF_AUDIO_INPUT_VOLUME_COUNT - 1,
               "audio volume actions must remain contiguous");

volatile int g_running = 1;
static volatile int g_supervisor_stop = 0;

#define GRAPH_RECOVERY_RETRY_MS 5000u

size_t string_length(const char *text)
{
    size_t length = 0;
    while(text != (const char *)0 && text[length] != '\0') ++length;
    return length;
}

void string_copy(char *destination, size_t capacity,
                 const char *source)
{
    size_t index = 0;
    if(capacity == 0) return;
    while(source != (const char *)0 && source[index] != '\0' &&
          index + 1 < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static int starts_with(const char *text, const char *prefix)
{
    size_t i = 0;
    while(prefix[i] != '\0') {
        if(text[i] != prefix[i]) return 0;
        ++i;
    }
    return 1;
}

int string_equal(const char *left, const char *right)
{
    size_t index = 0;
    if(left == (const char *)0 || right == (const char *)0) return 0;
    while(left[index] != '\0' && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

const char *find_text(const char *text, const char *needle)
{
    size_t i;
    size_t needle_length = string_length(needle);
    if(needle_length == 0) return text;
    for(i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while(j < needle_length && text[i + j] == needle[j]) ++j;
        if(j == needle_length) return text + i;
    }
    return (const char *)0;
}

static int run_program(const char *path, char *const arguments[])
{
    pid_t process = fork();
    int status = 0;
    int descriptor;
    if(process < 0) return -1;
    if(process == 0) {
        for(descriptor = 3; descriptor < 256; ++descriptor)
            (void)close(descriptor);
        (void)execv(path, arguments);
        _exit(127);
    }
    if(waitpid(process, &status, 0) != process) return -1;
    if((status & 0x7f) != 0) return -1;
    return (status >> 8) & 0xff;
}

void buffer_append(char *buffer, size_t capacity, size_t *used,
                   const char *text)
{
    size_t index = 0;
    while(text[index] != '\0' && *used + 1 < capacity)
        buffer[(*used)++] = text[index++];
    buffer[*used] = '\0';
}

void buffer_append_uint(char *buffer, size_t capacity, size_t *used,
                        unsigned value)
{
    char reversed[16];
    size_t count = 0;
    if(value == 0) {
        buffer_append(buffer, capacity, used, "0");
        return;
    }
    while(value != 0 && count < sizeof(reversed)) {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    while(count != 0) {
        char one[2];
        one[0] = reversed[--count];
        one[1] = '\0';
        buffer_append(buffer, capacity, used, one);
    }
}

static void buffer_append_ulong(char *buffer, size_t capacity, size_t *used,
                                unsigned long value)
{
    char reversed[24];
    size_t count = 0;
    if(value == 0) {
        buffer_append(buffer, capacity, used, "0");
        return;
    }
    while(value != 0 && count < sizeof(reversed)) {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    while(count != 0) {
        char one[2];
        one[0] = reversed[--count];
        one[1] = '\0';
        buffer_append(buffer, capacity, used, one);
    }
}

static uint16_t network_short(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t network_long(uint32_t value)
{
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

static int send_all(int descriptor, const char *buffer, size_t length)
{
    size_t sent = 0;
    while(sent < length) {
        ssize_t result = send(descriptor, buffer + sent, length - sent, 0);
        if(result <= 0) return -1;
        sent += (size_t)result;
    }
    return 0;
}

static int http_request_port_with_timeout(const char *method, const char *path,
                                          const char *body, char *response,
                                          size_t response_capacity,
                                          int timeout_seconds, int port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[2048];
    size_t request_length = 0;
    size_t body_length = body == (const char *)0 ? 0 : string_length(body);
    size_t received = 0;
    int descriptor;
    int result = -1;
    size_t i;

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if(descriptor < 0) return -1;
    if(timeout_seconds > 0) {
        timeout.seconds = timeout_seconds;
        timeout.microseconds = 0;
        (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         (socklen_t)sizeof(timeout));
        (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         (socklen_t)sizeof(timeout));
    }
    address.family = AF_INET;
    if(port <= 0 || port > 65535) goto done;
    address.port = network_short((uint16_t)port);
    address.address = network_long(0x7f000001u);
    for(i = 0; i < sizeof(address.zero); ++i) address.zero[i] = 0;
    if(connect(descriptor, (const struct sockaddr *)&address,
               (socklen_t)sizeof(address)) != 0)
        goto done;

    request[0] = '\0';
    buffer_append(request, sizeof(request), &request_length, method);
    buffer_append(request, sizeof(request), &request_length, " ");
    buffer_append(request, sizeof(request), &request_length, path);
    buffer_append(request, sizeof(request), &request_length,
                  " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n");
    if(body != (const char *)0)
        buffer_append(request, sizeof(request), &request_length,
                      "Content-Type: application/json\r\n");
    buffer_append(request, sizeof(request), &request_length, "Content-Length: ");
    buffer_append_uint(request, sizeof(request), &request_length,
                       (unsigned)body_length);
    buffer_append(request, sizeof(request), &request_length, "\r\n\r\n");
    if(body != (const char *)0)
        buffer_append(request, sizeof(request), &request_length, body);
    if(send_all(descriptor, request, request_length) != 0) goto done;

    while(received + 1 < response_capacity) {
        ssize_t count = recv(descriptor, response + received,
                             response_capacity - received - 1, 0);
        if(count == 0) break;
        if(count < 0) goto done;
        received += (size_t)count;
    }
    response[received] = '\0';
    if(!starts_with(response, "HTTP/1.1 200") &&
       !starts_with(response, "HTTP/1.0 200"))
        goto done;
    result = 0;
done:
    close(descriptor);
    return result;
}

static int http_request_with_timeout(const char *method, const char *path,
                                     const char *body, char *response,
                                     size_t response_capacity,
                                     int timeout_seconds)
{
    return http_request_port_with_timeout(method, path, body, response,
                                          response_capacity,
                                          timeout_seconds, 80);
}

int http_request(const char *method, const char *path, const char *body,
                 char *response, size_t response_capacity)
{
    return http_request_with_timeout(method, path, body, response,
                                     response_capacity, 3);
}

int parse_integer_after(const char *text, const char *key, int *value)
{
    const char *cursor = find_text(text, key);
    int sign = 1;
    int parsed = 0;
    int found_digit = 0;
    if(cursor == (const char *)0) return -1;
    cursor += string_length(key);
    while(*cursor == ' ' || *cursor == '\t' || *cursor == ':' || *cursor == '"') ++cursor;
    if(*cursor == '-') {
        sign = -1;
        ++cursor;
    }
    while(*cursor >= '0' && *cursor <= '9') {
        found_digit = 1;
        parsed = parsed * 10 + (*cursor - '0');
        ++cursor;
    }
    if(!found_digit) return -1;
    *value = parsed * sign;
    return 0;
}

int parse_milliseconds_after(const char *text, const char *key, int *value)
{
    const char *cursor = find_text(text, key);
    int seconds = 0;
    int fraction = 0;
    int fraction_digits = 0;
    int found_digit = 0;
    if(cursor == (const char *)0) return -1;
    cursor += string_length(key);
    while(*cursor == ' ' || *cursor == '\t' || *cursor == ':' ||
          *cursor == '"')
        ++cursor;
    while(*cursor >= '0' && *cursor <= '9') {
        found_digit = 1;
        seconds = seconds * 10 + (*cursor - '0');
        ++cursor;
    }
    if(!found_digit || seconds > 2147483) return -1;
    if(*cursor == '.') {
        ++cursor;
        while(*cursor >= '0' && *cursor <= '9' && fraction_digits < 3) {
            fraction = fraction * 10 + (*cursor - '0');
            ++fraction_digits;
            ++cursor;
        }
    }
    while(fraction_digits < 3) {
        fraction *= 10;
        ++fraction_digits;
    }
    *value = seconds * 1000 + fraction;
    return 0;
}

int response_code_ok(const char *response)
{
    int code;
    return parse_integer_after(response, "\"code\"", &code) == 0 && code == 0;
}

int parse_scalar_after(const char *text, const char *key,
                       char *value, size_t capacity)
{
    const char *cursor = find_text(text, key);
    size_t used = 0;
    int quoted = 0;
    if(cursor == (const char *)0 || capacity == 0) return -1;
    cursor += string_length(key);
    while(*cursor == ' ' || *cursor == '\t') ++cursor;
    if(*cursor != ':') return -1;
    ++cursor;
    while(*cursor == ' ' || *cursor == '\t') ++cursor;
    if(starts_with(cursor, "null")) return -1;
    if(*cursor == '"') {
        quoted = 1;
        ++cursor;
    }
    while(*cursor != '\0' &&
          ((quoted && *cursor != '"') ||
           (!quoted && *cursor != ',' && *cursor != '}' &&
            *cursor != ' ' && *cursor != '\t' &&
            *cursor != '\r' && *cursor != '\n'))) {
        if(used + 1 >= capacity) return -1;
        value[used++] = *cursor++;
    }
    if(used == 0 || (quoted && *cursor != '"')) return -1;
    value[used] = '\0';
    return 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return (uint64_t)time.seconds * 1000u +
           (uint64_t)time.nanoseconds / 1000000u;
}

static void supervisor_log_append(const char *text, size_t length)
{
    int descriptor = open(SUPERVISOR_LOG_PATH,
                          O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    size_t written = 0;
    if(descriptor < 0) return;
    while(written < length) {
        ssize_t count = write(descriptor, text + written, length - written);
        if(count <= 0) break;
        written += (size_t)count;
    }
    (void)fsync(descriptor);
    (void)close(descriptor);
}

static void supervisor_log_event(const char *event, const char *detail,
                                 int result)
{
    char line[256];
    size_t used = 0;
    line[0] = '\0';
    buffer_append(line, sizeof(line), &used, "CALF_UI_TRACE uptime_ms=");
    buffer_append_ulong(line, sizeof(line), &used,
                        (unsigned long)monotonic_milliseconds());
    buffer_append(line, sizeof(line), &used, " event=");
    buffer_append(line, sizeof(line), &used, event);
    if(detail != (const char *)0 && detail[0] != '\0') {
        buffer_append(line, sizeof(line), &used, " detail=");
        buffer_append(line, sizeof(line), &used, detail);
    }
    buffer_append(line, sizeof(line), &used, " result=");
    if(result < 0) buffer_append(line, sizeof(line), &used, "-");
    buffer_append_uint(line, sizeof(line), &used,
                       (unsigned)(result < 0 ? -result : result));
    buffer_append(line, sizeof(line), &used, "\n");
    supervisor_log_append(line, used);
}

static void supervisor_log_night_preview(const char *stage,
                                         const char *exposure,
                                         const char *iso, int preview_iso,
                                         int preview_fps, int attempt,
                                         int result)
{
    char detail[160];
    size_t used = 0;
    detail[0] = '\0';
    buffer_append(detail, sizeof(detail), &used, "stage:");
    buffer_append(detail, sizeof(detail), &used, stage);
    buffer_append(detail, sizeof(detail), &used, ",exp:");
    buffer_append(detail, sizeof(detail), &used, exposure);
    buffer_append(detail, sizeof(detail), &used, ",iso:");
    buffer_append(detail, sizeof(detail), &used, iso);
    buffer_append(detail, sizeof(detail), &used, ",preview_iso:");
    buffer_append_uint(detail, sizeof(detail), &used, (unsigned)preview_iso);
    buffer_append(detail, sizeof(detail), &used, ",fps:");
    buffer_append_uint(detail, sizeof(detail), &used, (unsigned)preview_fps);
    buffer_append(detail, sizeof(detail), &used, ",attempt:");
    buffer_append_uint(detail, sizeof(detail), &used, (unsigned)attempt);
    supervisor_log_event("NIGHT_PREVIEW", detail, result);
}

static void supervisor_log_ui_start(void)
{
    char line[128];
    size_t used = 0;
    line[0] = '\0';
    buffer_append(line, sizeof(line), &used, "CALF_UI_START pid=");
    buffer_append_ulong(line, sizeof(line), &used, (unsigned long)getpid());
    buffer_append(line, sizeof(line), &used, " uptime_ms=");
    buffer_append_ulong(line, sizeof(line), &used,
                        (unsigned long)monotonic_milliseconds());
    buffer_append(line, sizeof(line), &used, "\n");
    supervisor_log_append(line, used);
}

static void supervisor_log_ui_exit(int status)
{
    char line[96];
    size_t used = 0;
    line[0] = '\0';
    buffer_append(line, sizeof(line), &used, "CALF_UI_EXIT pid=");
    buffer_append_ulong(line, sizeof(line), &used, (unsigned long)getpid());
    buffer_append(line, sizeof(line), &used, " status=");
    buffer_append_uint(line, sizeof(line), &used, (unsigned)status);
    buffer_append(line, sizeof(line), &used, "\n");
    supervisor_log_append(line, used);
}

static const char *top_level_section_end(const char *section);

static int read_text_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    size_t used = 0;
    if(descriptor < 0 || capacity == 0) return -1;
    while(used + 1 < capacity) {
        size_t available = capacity - used - 1u;
        ssize_t count = read(descriptor, buffer + used, available);
        if(count == 0) break;
        if(count < 0) {
            close(descriptor);
            return -1;
        }
        if(count > (ssize_t)available) {
            close(descriptor);
            return -1;
        }
        used += (size_t)count;
    }
    if(used + 1 == capacity) {
        char extra;
        if(read(descriptor, &extra, 1) > 0) {
            close(descriptor);
            return -1;
        }
    }
    if(close(descriptor) != 0 || used >= capacity) return -1;
    buffer[used] = '\0';
    return 0;
}

static int write_device_text(const char *path, const char *text)
{
    int descriptor = open(path, O_WRONLY | O_NOFOLLOW);
    size_t length = string_length(text);
    size_t written = 0;
    int result = 0;
    if(descriptor < 0) return -1;
    while(written < length) {
        ssize_t count = write(descriptor, text + written, length - written);
        if(count <= 0) {
            result = -1;
            break;
        }
        written += (size_t)count;
    }
    if(close(descriptor) != 0) result = -1;
    return result;
}

int read_exact(int descriptor, unsigned char *buffer, size_t count)
{
    size_t used = 0;
    while(used < count) {
        ssize_t result = read(descriptor, buffer + used, count - used);
        if(result <= 0) return -1;
        used += (size_t)result;
    }
    return 0;
}

static int write_atomic_profile(const char *text, size_t length)
{
    return target_write_atomic_file(STOCK_PROFILE, STOCK_PROFILE_TEMP,
                                    text, length, 0640u, 1002, 1002);
}

static int request_stock_ui_session(void)
{
    static const char marker[] = "stock\n";
    int descriptor = open(STOCK_UI_SESSION_MARKER,
                          O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    ssize_t written;
    int result = 0;
    if(descriptor < 0) return -1;
    (void)fchmod(descriptor, 0600u);
    written = write(descriptor, marker, sizeof(marker) - 1u);
    if(written != (ssize_t)(sizeof(marker) - 1u)) result = -1;
    if(fsync(descriptor) != 0) result = -1;
    if(close(descriptor) != 0) result = -1;
    return result;
}

static int write_profile_edit(const char *source, const char *start,
                              const char *end, const char *replacement)
{
    static char output[PROFILE_BUFFER_SIZE];
    size_t prefix = (size_t)(start - source);
    size_t replacement_length = string_length(replacement);
    size_t suffix = string_length(end);
    size_t index;
    size_t used = 0;
    if(prefix + replacement_length + suffix + 1 > sizeof(output)) return -1;
    for(index = 0; index < prefix; ++index) output[used++] = source[index];
    for(index = 0; index < replacement_length; ++index)
        output[used++] = replacement[index];
    for(index = 0; index < suffix; ++index) output[used++] = end[index];
    output[used] = '\0';
    return write_atomic_profile(output, used);
}

static int save_stock_top_level_value(const char *key, const char *value)
{
    static char source[PROFILE_BUFFER_SIZE];
    char pattern[40];
    char appended[64];
    size_t pattern_used = 0;
    size_t appended_used = 0;
    const char *line;
    const char *value_start;
    const char *value_end;
    int at_start = 0;
    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0) return -1;
    pattern[0] = '\0';
    buffer_append(pattern, sizeof(pattern), &pattern_used, "\n");
    buffer_append(pattern, sizeof(pattern), &pattern_used, key);
    buffer_append(pattern, sizeof(pattern), &pattern_used, ":");
    if(starts_with(source, key) && source[string_length(key)] == ':') {
        line = source;
        at_start = 1;
    }
    else line = find_text(source, pattern);
    if(line != (const char *)0) {
        value_start = at_start ? line + string_length(key) + 1
                               : line + string_length(pattern);
        while(*value_start == ' ' || *value_start == '\t') ++value_start;
        value_end = value_start;
        while(*value_end != '\0' && *value_end != '\r' && *value_end != '\n')
            ++value_end;
        return write_profile_edit(source, value_start, value_end, value);
    }
    appended[0] = '\0';
    if(string_length(source) != 0 &&
       source[string_length(source) - 1] != '\n')
        buffer_append(appended, sizeof(appended), &appended_used, "\n");
    buffer_append(appended, sizeof(appended), &appended_used, key);
    buffer_append(appended, sizeof(appended), &appended_used, ": ");
    buffer_append(appended, sizeof(appended), &appended_used, value);
    buffer_append(appended, sizeof(appended), &appended_used, "\n");
    return write_profile_edit(source, source + string_length(source),
                              source + string_length(source), appended);
}

static int save_stock_auto_time(int enabled)
{
    static char source[PROFILE_BUFFER_SIZE];
    const char *section;
    const char *section_end;
    const char *line;
    const char *value_start;
    const char *value_end;
    const char *value = enabled ? "true" : "false";
    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0) return -1;
    if(starts_with(source, "datetime:\n")) section = source;
    else {
        section = find_text(source, "\ndatetime:\n");
        if(section != (const char *)0) ++section;
    }
    if(section == (const char *)0) {
        char addition[40];
        size_t used = 0;
        addition[0] = '\0';
        if(string_length(source) != 0 &&
           source[string_length(source) - 1] != '\n')
            buffer_append(addition, sizeof(addition), &used, "\n");
        buffer_append(addition, sizeof(addition), &used,
                      enabled ? "datetime:\n  autoset: true\n"
                              : "datetime:\n  autoset: false\n");
        return write_profile_edit(source, source + string_length(source),
                                  source + string_length(source), addition);
    }
    section_end = top_level_section_end(section);
    line = find_text(section, "\n  autoset:");
    if(line != (const char *)0 && line < section_end) {
        value_start = line + string_length("\n  autoset:");
        while(*value_start == ' ' || *value_start == '\t') ++value_start;
        value_end = value_start;
        while(*value_end != '\0' && *value_end != '\r' && *value_end != '\n')
            ++value_end;
        return write_profile_edit(source, value_start, value_end, value);
    }
    return write_profile_edit(source, section_end, section_end,
                              enabled ? "  autoset: true\n"
                                      : "  autoset: false\n");
}

static int parse_bool_after(const char *text, const char *key, int *value)
{
    const char *cursor = find_text(text, key);
    if(cursor == (const char *)0) return -1;
    cursor += string_length(key);
    while(*cursor == ' ' || *cursor == '\t' || *cursor == ':') ++cursor;
    if(starts_with(cursor, "true") || *cursor == '1') *value = 1;
    else if(starts_with(cursor, "false") || *cursor == '0') *value = 0;
    else return -1;
    return 0;
}

static void load_time_settings(time_settings_t *settings)
{
    static char profile[PROFILE_BUFFER_SIZE];
    const char *datetime;
    string_copy(settings->timezone, sizeof(settings->timezone), "UTC");
    settings->automatic = 0;
    if(read_text_file(STOCK_PROFILE, profile, sizeof(profile)) != 0) return;
    (void)parse_scalar_after(profile, "timezone",
                             settings->timezone, sizeof(settings->timezone));
    datetime = starts_with(profile, "datetime:\n")
                   ? profile : find_text(profile, "\ndatetime:\n");
    if(datetime != (const char *)0)
        (void)parse_bool_after(datetime, "autoset", &settings->automatic);
}

static int parse_time_floor(const char *text, unsigned long *value)
{
    unsigned long parsed = 0;
    int found = 0;
    while(*text == ' ' || *text == '\t') ++text;
    while(*text >= '0' && *text <= '9') {
        found = 1;
        parsed = parsed * 10ul + (unsigned long)(*text - '0');
        ++text;
    }
    if(!found) return -1;
    *value = parsed;
    return 0;
}

static int persist_time_floor(void)
{
    char buffer[32];
    size_t used = 0;
    long now = time((long *)0);
    if(now < 1577836800l || now > 2524607999l) return -1;
    buffer[0] = '\0';
    buffer_append_ulong(buffer, sizeof(buffer), &used, (unsigned long)now);
    buffer_append(buffer, sizeof(buffer), &used, "\n");
    if(target_write_atomic_file(TIME_FLOOR_CONFIG,
                                TIME_FLOOR_CONFIG_TEMP,
                                buffer, used, 0640u, 1002, 1002) != 0)
        return -1;
    /* The stock UI also writes the hardware clock in UTC regardless of the
     * selected display offset. Keep the TZ override scoped to this command. */
    return system("TZ=UTC /sbin/hwclock -w >/dev/null 2>&1");
}

static int restore_time_floor(void)
{
    char buffer[64];
    unsigned long floor;
    long now = time((long *)0);
    struct timeval restored;
    if(read_text_file(TIME_FLOOR_CONFIG, buffer, sizeof(buffer)) != 0 ||
       parse_time_floor(buffer, &floor) != 0 ||
       floor < 1577836800ul || floor > 2524607999ul ||
       floor <= (unsigned long)now)
        return 0;
    restored.seconds = (long)floor;
    restored.microseconds = 0;
    if(settimeofday(&restored, (const void *)0) != 0) return -1;
    (void)system("TZ=UTC /sbin/hwclock -w >/dev/null 2>&1");
    return 1;
}

static int save_stock_image_parameter(const char *key, const char *value)
{
    static char source[PROFILE_BUFFER_SIZE];
    static char output[PROFILE_BUFFER_SIZE];
    char addition[80];
    const char *section;
    const char *section_end;
    const char *line;
    const char *value_start;
    const char *value_end;
    size_t prefix;
    size_t value_length = string_length(value);
    size_t suffix;
    size_t output_length;
    char pattern[24];
    size_t pattern_used = 0;

    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0) return -1;
    if(starts_with(source, "image_params:\n")) section = source;
    else {
        section = find_text(source, "\nimage_params:\n");
        if(section != (const char *)0) ++section;
    }
    if(section == (const char *)0) {
        size_t used = 0;
        const char *end = source + string_length(source);
        addition[0] = '\0';
        if(end != source && end[-1] != '\n')
            buffer_append(addition, sizeof(addition), &used, "\n");
        buffer_append(addition, sizeof(addition), &used, "image_params:\n  ");
        buffer_append(addition, sizeof(addition), &used, key);
        buffer_append(addition, sizeof(addition), &used, ": ");
        buffer_append(addition, sizeof(addition), &used, value);
        buffer_append(addition, sizeof(addition), &used, "\n");
        return write_profile_edit(source, end, end, addition);
    }
    section_end = top_level_section_end(section);
    pattern[0] = '\0';
    buffer_append(pattern, sizeof(pattern), &pattern_used, "\n  ");
    buffer_append(pattern, sizeof(pattern), &pattern_used, key);
    buffer_append(pattern, sizeof(pattern), &pattern_used, ":");
    line = find_text(section, pattern);
    if(line == (const char *)0 || line >= section_end) {
        size_t used = (size_t)(section_end - source);
        size_t index;
        suffix = string_length(section_end);
        if(used + string_length(key) + value_length + suffix + 8 >
           sizeof(output))
            return -1;
        for(index = 0; index < used; ++index) output[index] = source[index];
        output[used] = '\0';
        if(used > 0 && output[used - 1] != '\n')
            buffer_append(output, sizeof(output), &used, "\n");
        buffer_append(output, sizeof(output), &used, "  ");
        buffer_append(output, sizeof(output), &used, key);
        buffer_append(output, sizeof(output), &used, ": ");
        buffer_append(output, sizeof(output), &used, value);
        buffer_append(output, sizeof(output), &used, "\n");
        buffer_append(output, sizeof(output), &used, section_end);
        return write_atomic_profile(output, used);
    }
    value_start = line + string_length(pattern);
    while(*value_start == ' ' || *value_start == '\t') ++value_start;
    value_end = value_start;
    while(*value_end != '\0' && *value_end != '\n' && *value_end != '\r')
        ++value_end;
    prefix = (size_t)(value_start - source);
    suffix = string_length(value_end);
    if(prefix + value_length + suffix + 1 > sizeof(output)) return -1;
    for(output_length = 0; output_length < prefix; ++output_length)
        output[output_length] = source[output_length];
    {
        size_t index;
        for(index = 0; index < value_length; ++index)
            output[output_length++] = value[index];
        for(index = 0; index < suffix; ++index)
            output[output_length++] = value_end[index];
    }
    output[output_length] = '\0';
    return write_atomic_profile(output, output_length);
}

static int parse_decimal_string(const char *text, int *value)
{
    int parsed = 0;
    int found = 0;
    if(text == (const char *)0 || *text == '\0') return -1;
    while(*text >= '0' && *text <= '9') {
        if(parsed > 214748364) return -1;
        parsed = parsed * 10 + (*text - '0');
        found = 1;
        ++text;
    }
    if(!found || *text != '\0') return -1;
    *value = parsed;
    return 0;
}

static int locate_encoder_entry(const char *source, const char *profile,
                                int channel, const char **entry_start,
                                const char **entry_end)
{
    char profile_pattern[56];
    size_t used = 0;
    const char *configs;
    const char *configs_end;
    const char *profile_start;
    const char *profile_end;
    const char *venc;
    const char *cursor;
    int current = -1;
    if(channel < 0 || channel > 2) return -1;
    configs = starts_with(source, "cam_configs:\n")
                  ? source : find_text(source, "\ncam_configs:\n");
    if(configs == (const char *)0) return -1;
    if(*configs == '\n') ++configs;
    configs_end = top_level_section_end(configs);
    profile_pattern[0] = '\0';
    buffer_append(profile_pattern, sizeof(profile_pattern), &used, "\n  ");
    buffer_append(profile_pattern, sizeof(profile_pattern), &used, profile);
    buffer_append(profile_pattern, sizeof(profile_pattern), &used, ":\n");
    profile_start = find_text(configs, profile_pattern);
    if(profile_start == (const char *)0 || profile_start >= configs_end)
        return -1;
    ++profile_start;
    profile_end = profile_start;
    while(*profile_end != '\0') {
        const char *next = find_text(profile_end, "\n  ");
        if(next == (const char *)0 || next >= configs_end) {
            profile_end = configs_end;
            break;
        }
        if(next[3] != ' ' && next[3] != '\t') {
            profile_end = next;
            break;
        }
        profile_end = next + 1;
    }
    venc = find_text(profile_start, "\n    venc:\n");
    if(venc == (const char *)0 || venc >= profile_end) return -1;
    cursor = venc + string_length("\n    venc:\n");
    while(cursor < profile_end && *cursor != '\0') {
        const char *marker = starts_with(cursor, "      - ")
                                 ? cursor : find_text(cursor, "\n      - ");
        if(marker == (const char *)0 || marker >= profile_end) break;
        if(*marker == '\n') ++marker;
        ++current;
        if(current == channel) {
            const char *next = find_text(marker, "\n      - ");
            *entry_start = marker;
            *entry_end = next != (const char *)0 && next < profile_end
                             ? next : profile_end;
            return 0;
        }
        cursor = marker + string_length("      - ");
    }
    return -1;
}

static int load_stock_encoder_parameter(const char *profile, int channel,
                                        const char *key, char *value,
                                        size_t capacity)
{
    static char source[PROFILE_BUFFER_SIZE];
    char pattern[40];
    size_t used = 0;
    const char *entry;
    const char *end;
    const char *line;
    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0 ||
       locate_encoder_entry(source, profile, channel, &entry, &end) != 0)
        return -1;
    pattern[0] = '\0';
    if(string_equal(key, "vcodec"))
        buffer_append(pattern, sizeof(pattern), &used, "      - vcodec:");
    else {
        buffer_append(pattern, sizeof(pattern), &used, "\n        ");
        buffer_append(pattern, sizeof(pattern), &used, key);
        buffer_append(pattern, sizeof(pattern), &used, ":");
    }
    line = find_text(entry, pattern);
    if(line == (const char *)0 || line >= end) return -1;
    if(*line == '\n') ++line;
    return parse_scalar_after(line, key, value, capacity);
}

static int save_stock_encoder_parameter(const char *profile, int channel,
                                        const char *key, const char *value)
{
    static char source[PROFILE_BUFFER_SIZE];
    char pattern[40];
    char addition[80];
    size_t used = 0;
    size_t addition_used = 0;
    const char *entry;
    const char *end;
    const char *line;
    const char *value_start;
    const char *value_end;
    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0 ||
       locate_encoder_entry(source, profile, channel, &entry, &end) != 0)
        return -1;
    pattern[0] = '\0';
    if(string_equal(key, "vcodec"))
        buffer_append(pattern, sizeof(pattern), &used, "      - vcodec:");
    else {
        buffer_append(pattern, sizeof(pattern), &used, "\n        ");
        buffer_append(pattern, sizeof(pattern), &used, key);
        buffer_append(pattern, sizeof(pattern), &used, ":");
    }
    line = find_text(entry, pattern);
    if(line != (const char *)0 && line < end) {
        value_start = line + string_length(pattern);
        while(*value_start == ' ' || *value_start == '\t') ++value_start;
        value_end = value_start;
        while(*value_end != '\0' && *value_end != '\r' &&
              *value_end != '\n')
            ++value_end;
        return write_profile_edit(source, value_start, value_end, value);
    }
    addition[0] = '\0';
    buffer_append(addition, sizeof(addition), &addition_used, "\n        ");
    buffer_append(addition, sizeof(addition), &addition_used, key);
    buffer_append(addition, sizeof(addition), &addition_used, ": ");
    buffer_append(addition, sizeof(addition), &addition_used, value);
    return write_profile_edit(source, end, end, addition);
}

static int save_stock_resolution(calf_capture_mode_t mode, const char *value)
{
    static char source[PROFILE_BUFFER_SIZE];
    const char *section;
    const char *section_end;
    const char *line;
    const char *value_start;
    const char *value_end;
    if(mode != CALF_CAPTURE_VIDEO)
        return save_stock_top_level_value("cam_video_format", value);
    if(read_text_file(STOCK_PROFILE, source, sizeof(source)) != 0) return -1;
    section = starts_with(source, "resolution:\n")
                  ? source : find_text(source, "\nresolution:\n");
    if(section == (const char *)0) return -1;
    if(*section == '\n') ++section;
    section_end = top_level_section_end(section);
    line = find_text(section, "\n  video:");
    if(line == (const char *)0 || line >= section_end) return -1;
    value_start = line + string_length("\n  video:");
    while(*value_start == ' ' || *value_start == '\t') ++value_start;
    value_end = value_start;
    while(*value_end != '\0' && *value_end != '\r' && *value_end != '\n')
        ++value_end;
    return write_profile_edit(source, value_start, value_end, value);
}

static const char *image_profile_key(calf_action_kind_t kind)
{
    static const char *const numeric_keys[] = {
        "brightness", "contrast", "saturation", "sharpness", "_3dnr",
    };
    if(kind == CALF_ACTION_SET_EXPOSURE) return "exp";
    if(kind == CALF_ACTION_SET_ISO) return "iso";
    if(kind == CALF_ACTION_SET_WHITE_BALANCE) return "wb";
    if(kind == CALF_ACTION_SET_EV) return "ev";
    if(kind >= CALF_ACTION_SET_BRIGHTNESS && kind <= CALF_ACTION_SET_DNR)
        return numeric_keys[(int)kind - (int)CALF_ACTION_SET_BRIGHTNESS];
    if(kind == CALF_ACTION_SET_ANTIFLICKER) return "antiflicker";
    if(kind == CALF_ACTION_SET_EFFECT) return "effect";
    return (const char *)0;
}

static int copy_profile_value(const char *line, char *value, size_t capacity)
{
    size_t used = 0;
    while(*line == ' ' || *line == '\t' || *line == ':') ++line;
    while(*line != '\0' && *line != '\r' && *line != '\n' &&
          *line != ' ' && *line != '\t') {
        char one[2];
        if(!((*line >= 'A' && *line <= 'Z') ||
             (*line >= '0' && *line <= '9') || *line == '_'))
            return -1;
        one[0] = *line++;
        one[1] = '\0';
        buffer_append(value, capacity, &used, one);
    }
    return used > 0 && used + 1 < capacity ? 0 : -1;
}

static int load_camera_profiles(camera_profiles_t *profiles)
{
    static char text[PROFILE_BUFFER_SIZE];
    const char *mode_line;
    const char *profile_line;
    const char *resolution;
    const char *resolution_end;
    string_copy(profiles->photo, sizeof(profiles->photo), "VR180_PIC");
    string_copy(profiles->video, sizeof(profiles->video), "VR180_6K");
    profiles->mode = CALF_CAPTURE_PHOTO;
    if(read_text_file(STOCK_PROFILE, text, sizeof(text)) != 0) return -1;
    mode_line = starts_with(text, "cam_mode:")
                    ? text : find_text(text, "\ncam_mode:");
    if(mode_line != (const char *)0) {
        mode_line += starts_with(mode_line, "cam_mode:")
                         ? string_length("cam_mode:")
                         : string_length("\ncam_mode:");
        while(*mode_line == ' ' || *mode_line == '\t') ++mode_line;
        if(starts_with(mode_line, "recording") ||
           starts_with(mode_line, "video"))
            profiles->mode = CALF_CAPTURE_VIDEO;
    }

    profile_line = starts_with(text, "cam_video_format:")
                       ? text : find_text(text, "\ncam_video_format:");
    if(profile_line != (const char *)0) {
        profile_line += starts_with(profile_line, "cam_video_format:")
                            ? string_length("cam_video_format:")
                            : string_length("\ncam_video_format:");
        (void)copy_profile_value(profile_line, profiles->photo,
                                 sizeof(profiles->photo));
    }
    resolution = find_text(text, "\nresolution:\n");
    if(resolution != (const char *)0) {
        ++resolution;
        resolution_end = top_level_section_end(resolution);
        profile_line = find_text(resolution, "\n  video:");
        if(profile_line != (const char *)0 && profile_line < resolution_end) {
            profile_line += string_length("\n  video:");
            (void)copy_profile_value(profile_line, profiles->video,
                                     sizeof(profiles->video));
        }
    }
    return 0;
}

static const char *top_level_section_end(const char *section)
{
    const char *cursor = section;
    while(*cursor != '\0') {
        if(*cursor == '\n' && cursor[1] != '\0' && cursor[1] != ' ' &&
           cursor[1] != '\t')
            return cursor + 1;
        ++cursor;
    }
    return cursor;
}

static int speaker_volume_valid(int value)
{
    return value >= 0 && value <= 140 && value % 10 == 0;
}

static int load_stock_speaker_volume(void)
{
    static char profile[PROFILE_BUFFER_SIZE];
    const char *audio;
    const char *audio_end;
    const char *output;
    const char *volume_line;
    int value;
    if(read_text_file(STOCK_PROFILE, profile, sizeof(profile)) != 0)
        return 50;
    if(parse_integer_after(profile, "audio.output.volumn", &value) == 0 &&
       speaker_volume_valid(value))
        return value;
    if(starts_with(profile, "audio:\n")) audio = profile;
    else {
        audio = find_text(profile, "\naudio:\n");
        if(audio != (const char *)0) ++audio;
    }
    if(audio == (const char *)0) return 50;
    audio_end = top_level_section_end(audio);
    output = find_text(audio, "\n  output:\n");
    if(output == (const char *)0 || output >= audio_end) return 50;
    volume_line = find_text(output, "\n    volumn:");
    if(volume_line == (const char *)0 || volume_line >= audio_end ||
       parse_integer_after(volume_line, "volumn", &value) != 0 ||
       !speaker_volume_valid(value))
        return 50;
    return value;
}

static int load_speaker_volume(void)
{
    char buffer[64];
    int value;
    if(read_text_file(SPEAKER_VOLUME_CONFIG, buffer, sizeof(buffer)) == 0 &&
       parse_integer_after(buffer, "\"value\"", &value) == 0 &&
       speaker_volume_valid(value))
        return value;
    return load_stock_speaker_volume();
}

static int save_speaker_volume(int value)
{
    char buffer[32];
    size_t used = 0;
    if(!speaker_volume_valid(value)) return -1;
    buffer[0] = '\0';
    buffer_append(buffer, sizeof(buffer), &used, "{\"value\":");
    buffer_append_uint(buffer, sizeof(buffer), &used, (unsigned)value);
    buffer_append(buffer, sizeof(buffer), &used, "}\n");
    return target_write_atomic_file(SPEAKER_VOLUME_CONFIG,
                                    SPEAKER_VOLUME_CONFIG_TEMP,
                                    buffer, used, 0640u, 1002, 1002);
}

static int load_stock_wifi_enabled(void)
{
    static char profile[PROFILE_BUFFER_SIZE];
    const char *wlan;
    const char *end;
    const char *enable;
    int enabled = 1;
    if(read_text_file(STOCK_PROFILE, profile, sizeof(profile)) != 0)
        return enabled;
    if(starts_with(profile, "wlan:\n")) wlan = profile;
    else {
        wlan = find_text(profile, "\nwlan:\n");
        if(wlan != (const char *)0) ++wlan;
    }
    if(wlan == (const char *)0) return enabled;
    end = top_level_section_end(wlan);
    enable = find_text(wlan, "\n  enable:");
    if(enable == (const char *)0 || enable >= end ||
       parse_bool_after(enable, "enable", &enabled) != 0)
        return 1;
    return enabled;
}

static int load_stock_display_off_seconds(const char *profile, int *seconds)
{
    const char *display;
    const char *off;
    const char *delay;
    const char *end;
    if(starts_with(profile, "display:\n")) display = profile;
    else {
        display = find_text(profile, "\ndisplay:\n");
        if(display != (const char *)0) ++display;
    }
    if(display == (const char *)0) return -1;
    end = top_level_section_end(display);
    off = find_text(display, "\n  off:\n");
    if(off == (const char *)0 || off >= end) return -1;
    delay = find_text(off, "\n    delay:");
    if(delay == (const char *)0 || delay >= end) return -1;
    return parse_integer_after(delay, "delay", seconds);
}

static int load_display_off_selection(void)
{
    static char buffer[PROFILE_BUFFER_SIZE];
    int seconds;
    int index;
    if(read_text_file(DISPLAY_OFF_CONFIG, buffer, sizeof(buffer)) == 0 &&
       parse_integer_after(buffer, "", &seconds) == 0) {
        index = calf_display_off_index_from_seconds(seconds);
        if(index >= 0) return index;
    }
    if(read_text_file(STOCK_PROFILE, buffer, sizeof(buffer)) == 0 &&
       load_stock_display_off_seconds(buffer, &seconds) == 0) {
        index = calf_display_off_index_from_seconds(seconds);
        if(index >= 0) return index;
    }
    return 0;
}

static int save_display_off_selection(int selection)
{
    char buffer[24];
    size_t used = 0;
    int seconds = calf_display_off_seconds((size_t)selection);
    buffer[0] = '\0';
    if(seconds < 0) buffer_append(buffer, sizeof(buffer), &used, "-1");
    else buffer_append_uint(buffer, sizeof(buffer), &used, (unsigned)seconds);
    buffer_append(buffer, sizeof(buffer), &used, "\n");
    return target_write_atomic_file(DISPLAY_OFF_CONFIG,
                                    DISPLAY_OFF_CONFIG_TEMP,
                                    buffer, used, 0640u, 1002, 1002);
}

static int load_language_selection(void)
{
    char buffer[16];
    size_t length;
    int selection;
    if(read_text_file(LANGUAGE_CONFIG, buffer, sizeof(buffer)) != 0)
        return CALF_LANGUAGE_ENGLISH;
    length = string_length(buffer);
    while(length > 0u &&
          (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r'))
        buffer[--length] = '\0';
    selection = calf_language_index_from_value(buffer);
    return selection >= 0 ? selection : CALF_LANGUAGE_ENGLISH;
}

static int save_language_selection(int selection)
{
    char buffer[16];
    const char *value;
    size_t length;
    if(selection < 0 || selection >= (int)calf_language_count()) return -1;
    value = calf_language_value((size_t)selection);
    length = string_length(value);
    if(length + 2u > sizeof(buffer)) return -1;
    string_copy(buffer, sizeof(buffer), value);
    buffer[length++] = '\n';
    buffer[length] = '\0';
    return target_write_atomic_file(LANGUAGE_CONFIG, LANGUAGE_CONFIG_TEMP,
                                    buffer, length, 0640u, 1002, 1002);
}

static int load_indicator_led_selection(void)
{
    char buffer[16];
    if(read_text_file(INDICATOR_LED_CONFIG, buffer, sizeof(buffer)) != 0)
        return 0;
    if(starts_with(buffer, "normal\n") || string_equal(buffer, "normal"))
        return 0;
    if(starts_with(buffer, "stealth\n") || string_equal(buffer, "stealth"))
        return 1;
    return 0;
}

static int save_indicator_led_selection(int selection)
{
    static const char normal[] = "normal\n";
    static const char stealth[] = "stealth\n";
    const char *value;
    size_t length;
    if(selection == 0) {
        value = normal;
        length = sizeof(normal) - 1u;
    }
    else if(selection == 1) {
        value = stealth;
        length = sizeof(stealth) - 1u;
    }
    else return -1;
    return target_write_atomic_file(INDICATOR_LED_CONFIG,
                                    INDICATOR_LED_CONFIG_TEMP,
                                    value, length, 0640u, 1002, 1002);
}

static void indicator_led_force_off(void)
{
    (void)write_device_text(BLUE_LED_TRIGGER, "none\n");
    (void)write_device_text(BLUE_LED_BRIGHTNESS, "0\n");
}

static void indicator_led_restore_recording_blink(void)
{
    (void)write_device_text(BLUE_LED_TRIGGER, "timer\n");
}

static int load_raw_capture_enabled(void)
{
    char buffer[8];
    if(read_text_file(RAW_CAPTURE_CONFIG, buffer, sizeof(buffer)) != 0)
        return 0;
    return buffer[0] == '1' &&
           (buffer[1] == '\0' || buffer[1] == '\n');
}

static int save_raw_capture_enabled(int enabled)
{
    static const char disabled[] = "0\n";
    static const char enabled_text[] = "1\n";
    const char *text = enabled ? enabled_text : disabled;
    return target_write_atomic_file(RAW_CAPTURE_CONFIG,
                                    RAW_CAPTURE_CONFIG_TEMP,
                                    text, 2, 0640u, 1002, 1002);
}

static int load_drive_mode_selection(void)
{
    char buffer[32];
    size_t length;
    int selection;
    if(read_text_file(DRIVE_MODE_CONFIG, buffer, sizeof(buffer)) != 0)
        return 0;
    length = string_length(buffer);
    while(length > 0u &&
          (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r'))
        buffer[--length] = '\0';
    selection = calf_drive_mode_index_from_value(buffer);
    return selection >= 0 ? selection : 0;
}

static int save_drive_mode_selection(int selection)
{
    char buffer[32];
    const char *value;
    size_t length;
    if(selection < 0 || selection >= (int)calf_drive_mode_count())
        return -1;
    value = calf_drive_mode_value((size_t)selection);
    length = string_length(value);
    if(length + 2u > sizeof(buffer)) return -1;
    string_copy(buffer, sizeof(buffer), value);
    buffer[length++] = '\n';
    buffer[length] = '\0';
    return target_write_atomic_file(DRIVE_MODE_CONFIG,
                                    DRIVE_MODE_CONFIG_TEMP,
                                    buffer, length, 0640u, 1002, 1002);
}

static int load_small_value(const char *path, char *value, size_t capacity)
{
    size_t length;
    if(read_text_file(path, value, capacity) != 0) return -1;
    length = string_length(value);
    while(length > 0u &&
          (value[length - 1u] == '\n' || value[length - 1u] == '\r'))
        value[--length] = '\0';
    return length > 0u ? 0 : -1;
}

static int save_small_value(const char *path, const char *temporary,
                            const char *value)
{
    char buffer[32];
    size_t length = string_length(value);
    if(length == 0u || length + 2u > sizeof(buffer)) return -1;
    string_copy(buffer, sizeof(buffer), value);
    buffer[length++] = '\n';
    buffer[length] = '\0';
    return target_write_atomic_file(path, temporary, buffer, length,
                                    0640u, 1002, 1002);
}

static calf_capture_mode_t load_capture_mode(calf_capture_mode_t fallback)
{
    char value[16];
    if(load_small_value(CAPTURE_MODE_CONFIG, value, sizeof(value)) != 0)
        return fallback;
    if(string_equal(value, "photo")) return CALF_CAPTURE_PHOTO;
    if(string_equal(value, "night")) return CALF_CAPTURE_NIGHT;
    if(string_equal(value, "recording") || string_equal(value, "video"))
        return CALF_CAPTURE_VIDEO;
    return fallback;
}

static int save_capture_mode(calf_capture_mode_t mode)
{
    const char *value = mode == CALF_CAPTURE_NIGHT ? "night"
                        : mode == CALF_CAPTURE_VIDEO ? "recording"
                                                    : "photo";
    return save_small_value(CAPTURE_MODE_CONFIG, CAPTURE_MODE_CONFIG_TEMP,
                            value);
}

static int save_mode_image_values(calf_capture_mode_t mode,
                                  const char *exposure, const char *iso)
{
    if(mode == CALF_CAPTURE_NIGHT)
        return save_small_value(NIGHT_EXPOSURE_CONFIG,
                                NIGHT_EXPOSURE_CONFIG_TEMP, exposure) == 0 &&
               save_small_value(NIGHT_ISO_CONFIG, NIGHT_ISO_CONFIG_TEMP,
                                iso) == 0 ? 0 : -1;
    return save_small_value(PHOTO_EXPOSURE_CONFIG,
                            PHOTO_EXPOSURE_CONFIG_TEMP, exposure) == 0 &&
           save_small_value(PHOTO_ISO_CONFIG, PHOTO_ISO_CONFIG_TEMP,
                            iso) == 0 ? 0 : -1;
}

static void load_mode_image_values(calf_capture_mode_t mode,
                                   char exposure[16], char iso[16])
{
    const char *exposure_path = mode == CALF_CAPTURE_NIGHT
                                    ? NIGHT_EXPOSURE_CONFIG
                                    : PHOTO_EXPOSURE_CONFIG;
    const char *iso_path = mode == CALF_CAPTURE_NIGHT
                               ? NIGHT_ISO_CONFIG : PHOTO_ISO_CONFIG;
    if(load_small_value(exposure_path, exposure, 16) != 0 ||
       !calf_exposure_allowed(mode, exposure))
        string_copy(exposure, 16,
                    mode == CALF_CAPTURE_NIGHT ? "0.5" : "-1");
    if(load_small_value(iso_path, iso, 16) != 0 ||
       !calf_iso_allowed(mode, iso))
        string_copy(iso, 16,
                    mode == CALF_CAPTURE_NIGHT ? "iso400" : "auto");
}

static int audio_input_state_valid(const audio_input_state_t *state)
{
    int index;
    if((state->automatic != 0 && state->automatic != 1) ||
       state->input_type < 0 || state->input_type > 2)
        return 0;
    for(index = 0; index < CALF_AUDIO_INPUT_VOLUME_COUNT; ++index)
        if(state->volume[index] < 0 || state->volume[index] > 100)
            return 0;
    return 1;
}

static int audio_input_state_equal(const audio_input_state_t *left,
                                   const audio_input_state_t *right)
{
    int index;
    if(!left->valid || !right->valid ||
       left->automatic != right->automatic ||
       left->input_type != right->input_type)
        return 0;
    for(index = 0; index < CALF_AUDIO_INPUT_VOLUME_COUNT; ++index)
        if(left->volume[index] != right->volume[index]) return 0;
    return 1;
}

static int load_audio_input_state(audio_input_state_t *state)
{
    char buffer[256];
    if(read_text_file(AUDIO_INPUT_CONFIG, buffer, sizeof(buffer)) != 0 ||
       parse_integer_after(buffer, "\"autoinput\"", &state->automatic) != 0 ||
       parse_integer_after(buffer, "\"inputtype\"", &state->input_type) != 0 ||
       parse_integer_after(buffer, "\"inputvol0\"", &state->volume[0]) != 0 ||
       parse_integer_after(buffer, "\"inputvol1\"", &state->volume[1]) != 0 ||
       parse_integer_after(buffer, "\"inputvol2\"", &state->volume[2]) != 0 ||
       !audio_input_state_valid(state)) {
        state->valid = 0;
        return -1;
    }
    state->valid = 1;
    return 0;
}

static int load_stock_audio_input_state(audio_input_state_t *state)
{
    static char profile[PROFILE_BUFFER_SIZE];
    char source[32];
    const char *audio;
    const char *audio_end;
    const char *input;
    const char *volume;
    if(read_text_file(STOCK_PROFILE, profile, sizeof(profile)) != 0)
        return -1;
    if(starts_with(profile, "audio:\n")) audio = profile;
    else {
        audio = find_text(profile, "\naudio:\n");
        if(audio != (const char *)0) ++audio;
    }
    if(audio == (const char *)0) return -1;
    audio_end = top_level_section_end(audio);
    input = find_text(audio, "\n  input:\n");
    if(input == (const char *)0 || input >= audio_end) return -1;
    if(parse_scalar_after(input, "src", source, sizeof(source)) != 0)
        return -1;
    volume = find_text(input, "\n    volumn:\n");
    if(volume == (const char *)0 || volume >= audio_end ||
       parse_integer_after(volume, "builtin_mic", &state->volume[0]) != 0 ||
       parse_integer_after(volume, "35mm_linein", &state->volume[1]) != 0 ||
       parse_integer_after(volume, "usb_mic", &state->volume[2]) != 0)
        return -1;
    state->automatic = string_equal(source, "auto");
    if(state->automatic || string_equal(source, "builtin_mic"))
        state->input_type = 0;
    else if(string_equal(source, "35mm_linein")) state->input_type = 1;
    else if(string_equal(source, "usb_mic")) state->input_type = 2;
    else return -1;
    if(!audio_input_state_valid(state)) return -1;
    state->valid = 1;
    return 0;
}

static int save_audio_input_state(const audio_input_state_t *state)
{
    static const char *const keys[] = {
        "autoinput", "inputtype", "inputvol0", "inputvol1", "inputvol2",
    };
    unsigned values[5];
    char buffer[192];
    size_t used = 0;
    size_t index;
    if(!state->valid || !audio_input_state_valid(state)) return -1;
    values[0] = (unsigned)state->automatic;
    values[1] = (unsigned)state->input_type;
    values[2] = (unsigned)state->volume[0];
    values[3] = (unsigned)state->volume[1];
    values[4] = (unsigned)state->volume[2];
    buffer[0] = '\0';
    buffer_append(buffer, sizeof(buffer), &used, "{");
    for(index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        if(index != 0) buffer_append(buffer, sizeof(buffer), &used, ",");
        buffer_append(buffer, sizeof(buffer), &used, "\"");
        buffer_append(buffer, sizeof(buffer), &used, keys[index]);
        buffer_append(buffer, sizeof(buffer), &used, "\":");
        buffer_append_uint(buffer, sizeof(buffer), &used, values[index]);
    }
    buffer_append(buffer, sizeof(buffer), &used, "}\n");
    return target_write_atomic_file(AUDIO_INPUT_CONFIG,
                                    AUDIO_INPUT_CONFIG_TEMP,
                                    buffer, used, 0640u, 1002, 1002);
}

static int parse_fixed_decimal(const char *text, int offset, int digits,
                               int *value)
{
    int index;
    int parsed = 0;
    for(index = 0; index < digits; ++index) {
        char character = text[offset + index];
        if(character < '0' || character > '9') return -1;
        parsed = parsed * 10 + character - '0';
    }
    *value = parsed;
    return 0;
}

static int apply_manual_datetime(const char *value)
{
    struct tm local;
    struct timeval clock;
    long epoch;
    size_t index;
    if(value == (const char *)0 || string_length(value) != 19 ||
       value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
       value[13] != ':' || value[16] != ':')
        return -1;
    for(index = 0; index < sizeof(local); ++index)
        ((unsigned char *)&local)[index] = 0;
    if(parse_fixed_decimal(value, 0, 4, &local.year) != 0 ||
       parse_fixed_decimal(value, 5, 2, &local.month) != 0 ||
       parse_fixed_decimal(value, 8, 2, &local.month_day) != 0 ||
       parse_fixed_decimal(value, 11, 2, &local.hour) != 0 ||
       parse_fixed_decimal(value, 14, 2, &local.minute) != 0 ||
       parse_fixed_decimal(value, 17, 2, &local.second) != 0)
        return -1;
    local.year -= 1900;
    local.month -= 1;
    local.is_dst = -1;
    epoch = mktime(&local);
    if(epoch < 1577836800l || epoch > 2524607999l) return -1;
    clock.seconds = epoch;
    clock.microseconds = 0;
    if(settimeofday(&clock, (const void *)0) != 0) return -1;
    return persist_time_floor();
}

static void sync_local_datetime(calf_ui_t *ui)
{
    long now = time((long *)0);
    struct tm local;
    if(now < 1577836800l || localtime_r(&now, &local) == (struct tm *)0)
        return;
    calf_ui_sync_datetime(ui, local.year + 1900, local.month + 1,
                          local.month_day, local.hour, local.minute,
                          local.second);
}

static int perform_time_action(calf_action_t action,
                               time_settings_t *settings)
{
    if(action.kind == CALF_ACTION_SET_TIMEZONE) {
        if(save_stock_top_level_value("timezone", action.value) != 0 ||
           setenv("TZ", action.value, 1) != 0)
            return -1;
        tzset();
        string_copy(settings->timezone, sizeof(settings->timezone), action.value);
        return 0;
    }
    if(action.kind == CALF_ACTION_SET_AUTO_TIME) {
        int enabled = string_equal(action.value, "1");
        if((!enabled && !string_equal(action.value, "0")) ||
           save_stock_auto_time(enabled) != 0)
            return -1;
        if(system(enabled ? "/app/bin/ntpservice start"
                          : "/app/bin/ntpservice stop") != 0)
            return -1;
        settings->automatic = enabled;
        return 0;
    }
    if(action.kind == CALF_ACTION_SET_DATETIME) {
        if(settings->automatic) return -1;
        return apply_manual_datetime(action.value);
    }
    return -1;
}

typedef struct {
    char vcodec[16];
    char profile[16];
    int bitrate;
    int gop;
    int color_range;
} encoder_attr_t;

static int api_read_encoder_attr(int channel, encoder_attr_t *attr)
{
    char path[48];
    char response[HTTP_BUFFER_SIZE];
    size_t used = 0;
    path[0] = '\0';
    buffer_append(path, sizeof(path), &used,
                  "/camera/v2/vencattr?channel=");
    buffer_append_uint(path, sizeof(path), &used, (unsigned)channel);
    if(http_request("GET", path, (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response) ||
       parse_scalar_after(response, "\"vcodec\"",
                          attr->vcodec, sizeof(attr->vcodec)) != 0 ||
       parse_scalar_after(response, "\"profile\"",
                          attr->profile, sizeof(attr->profile)) != 0 ||
       parse_integer_after(response, "\"bitrate\"", &attr->bitrate) != 0 ||
       parse_integer_after(response, "\"gop\"", &attr->gop) != 0)
        return -1;
    attr->color_range = 0;
    return 0;
}

static int api_write_encoder_attr(int channel, const encoder_attr_t *attr)
{
    char body[256];
    char response[HTTP_BUFFER_SIZE];
    encoder_attr_t observed;
    size_t used = 0;
    body[0] = '\0';
    buffer_append(body, sizeof(body), &used, "{\"channel\":");
    buffer_append_uint(body, sizeof(body), &used, (unsigned)channel);
    buffer_append(body, sizeof(body), &used, ",\"vcodec\":\"");
    buffer_append(body, sizeof(body), &used, attr->vcodec);
    buffer_append(body, sizeof(body), &used, "\",\"profile\":\"");
    buffer_append(body, sizeof(body), &used, attr->profile);
    buffer_append(body, sizeof(body), &used, "\",\"bitrate\":");
    buffer_append_uint(body, sizeof(body), &used, (unsigned)attr->bitrate);
    buffer_append(body, sizeof(body), &used, ",\"gop\":");
    buffer_append_uint(body, sizeof(body), &used, (unsigned)attr->gop);
    buffer_append(body, sizeof(body), &used, ",\"color_range\":");
    buffer_append_uint(body, sizeof(body), &used,
                       (unsigned)attr->color_range);
    buffer_append(body, sizeof(body), &used, "}");
    if(http_request("POST", "/camera/v2/vencattr", body,
                    response, sizeof(response)) != 0)
        return -1;
    if(response_code_ok(response)) return 0;
    /* This backend applies venc changes and then incorrectly returns code -1.
     * Treat the request as successful only when an immediate authoritative
     * readback confirms every field that GET exposes. Color range is omitted
     * by GET, so its value remains profile-backed just as in the stock UI. */
    if(api_read_encoder_attr(channel, &observed) != 0 ||
       !string_equal(observed.vcodec, attr->vcodec) ||
       !string_equal(observed.profile, attr->profile) ||
       observed.bitrate != attr->bitrate || observed.gop != attr->gop)
        return -1;
    return 0;
}

static int load_encoder_color_range(const char *camera_profile, int channel)
{
    char value[16];
    int parsed;
    if(load_stock_encoder_parameter(camera_profile, channel, "color_range",
                                    value, sizeof(value)) != 0 ||
       parse_decimal_string(value, &parsed) != 0 ||
       (parsed != 0 && parsed != 1))
        return 0;
    return parsed;
}

static void recording_codec_value(const encoder_attr_t *attr,
                                  char *value, size_t capacity)
{
    value[0] = '\0';
    if(string_equal(attr->vcodec, "H265"))
        string_copy(value, capacity, "H265_MAIN");
    else if(string_equal(attr->profile, "MAIN"))
        string_copy(value, capacity, "H264_MAIN");
    else if(string_equal(attr->profile, "BASE") ||
            string_equal(attr->profile, "BASELINE"))
        string_copy(value, capacity, "H264_BASE");
    else
        string_copy(value, capacity, "H264_HIGH");
}

static int api_sync_encoder_state(calf_ui_t *ui, const char *camera_profile)
{
    encoder_attr_t main_attr;
    encoder_attr_t recording_attr;
    char value[24];
    int found = 0;
    if(api_read_encoder_attr(0, &main_attr) == 0) {
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_ENCODING_CODEC, main_attr.vcodec);
        if(main_attr.bitrate == 150000)
            string_copy(value, sizeof(value), "higher");
        else if(main_attr.bitrate == 100000)
            string_copy(value, sizeof(value), "high");
        else if(main_attr.bitrate == 60000)
            string_copy(value, sizeof(value), "medium");
        else if(main_attr.bitrate == 20000)
            string_copy(value, sizeof(value), "low");
        else if(load_stock_encoder_parameter(camera_profile, 0, "iq",
                                             value, sizeof(value)) != 0)
            value[0] = '\0';
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_IMAGE_QUALITY, value);
        main_attr.color_range = load_encoder_color_range(camera_profile, 0);
        string_copy(value, sizeof(value), main_attr.color_range ? "1" : "0");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_ENCODING_COLOR_RANGE, value);
        found = 1;
    }
    if(api_read_encoder_attr(1, &recording_attr) == 0) {
        recording_codec_value(&recording_attr, value, sizeof(value));
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_CODEC, value);
        value[0] = '\0';
        {
            size_t used = 0;
            buffer_append_uint(value, sizeof(value), &used,
                               (unsigned)recording_attr.bitrate);
        }
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_BITRATE, value);
        value[0] = '\0';
        {
            size_t used = 0;
            buffer_append_uint(value, sizeof(value), &used,
                               (unsigned)recording_attr.gop);
        }
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_GOP, value);
        recording_attr.color_range = load_encoder_color_range(
            camera_profile, 1);
        string_copy(value, sizeof(value),
                    recording_attr.color_range ? "1" : "0");
        (void)calf_ui_sync_encoder_value(
            ui, CALF_ACTION_SET_RECORDING_COLOR_RANGE, value);
        found = 1;
    }
    return found ? 0 : -1;
}

static void encoder_profile_override(const char *camera_profile, int channel,
                                     const char *key, char *destination,
                                     size_t capacity)
{
    char value[24];
    if(load_stock_encoder_parameter(camera_profile, channel, key,
                                    value, sizeof(value)) == 0)
        string_copy(destination, capacity, value);
}

static int api_apply_encoder_profile(const char *camera_profile)
{
    int channel;
    for(channel = 0; channel <= 1; ++channel) {
        encoder_attr_t attr;
        char value[24];
        int numeric;
        if(api_read_encoder_attr(channel, &attr) != 0) return -1;
        encoder_profile_override(camera_profile, channel, "vcodec",
                                 attr.vcodec, sizeof(attr.vcodec));
        encoder_profile_override(camera_profile, channel, "profile",
                                 attr.profile, sizeof(attr.profile));
        if(load_stock_encoder_parameter(camera_profile, channel, "bitrate",
                                        value, sizeof(value)) == 0 &&
           parse_decimal_string(value, &numeric) == 0)
            attr.bitrate = numeric;
        if(load_stock_encoder_parameter(camera_profile, channel, "gop",
                                        value, sizeof(value)) == 0 &&
           parse_decimal_string(value, &numeric) == 0)
            attr.gop = numeric;
        attr.color_range = load_encoder_color_range(camera_profile, channel);
        if(api_write_encoder_attr(channel, &attr) != 0) return -1;
    }
    return 0;
}

static int perform_encoder_action(calf_action_t action,
                                  const char *camera_profile,
                                  int *profile_saved)
{
    encoder_attr_t attr;
    int channel = action.kind >= CALF_ACTION_SET_RECORDING_CODEC ? 1 : 0;
    int numeric;
    *profile_saved = 1;
    if(api_read_encoder_attr(channel, &attr) != 0) return -1;
    attr.color_range = load_encoder_color_range(camera_profile, channel);
    if(action.kind == CALF_ACTION_SET_ENCODING_CODEC)
        string_copy(attr.vcodec, sizeof(attr.vcodec), action.value);
    else if(action.kind == CALF_ACTION_SET_IMAGE_QUALITY) {
        attr.bitrate = string_equal(action.value, "higher") ? 150000
                       : string_equal(action.value, "high") ? 100000
                       : string_equal(action.value, "medium") ? 60000
                       : string_equal(action.value, "low") ? 20000 : -1;
        if(attr.bitrate < 0) return -1;
    }
    else if(action.kind == CALF_ACTION_SET_ENCODING_COLOR_RANGE ||
            action.kind == CALF_ACTION_SET_RECORDING_COLOR_RANGE) {
        if(parse_decimal_string(action.value, &numeric) != 0 ||
           (numeric != 0 && numeric != 1))
            return -1;
        attr.color_range = numeric;
    }
    else if(action.kind == CALF_ACTION_SET_RECORDING_CODEC) {
        if(string_equal(action.value, "H264_HIGH")) {
            string_copy(attr.vcodec, sizeof(attr.vcodec), "H264");
            string_copy(attr.profile, sizeof(attr.profile), "HIGH");
        }
        else if(string_equal(action.value, "H264_MAIN")) {
            string_copy(attr.vcodec, sizeof(attr.vcodec), "H264");
            string_copy(attr.profile, sizeof(attr.profile), "MAIN");
        }
        else if(string_equal(action.value, "H264_BASE")) {
            string_copy(attr.vcodec, sizeof(attr.vcodec), "H264");
            string_copy(attr.profile, sizeof(attr.profile), "BASE");
        }
        else if(string_equal(action.value, "H265_MAIN")) {
            string_copy(attr.vcodec, sizeof(attr.vcodec), "H265");
            string_copy(attr.profile, sizeof(attr.profile), "MAIN");
        }
        else return -1;
    }
    else if(action.kind == CALF_ACTION_SET_RECORDING_BITRATE) {
        if(parse_decimal_string(action.value, &numeric) != 0) return -1;
        attr.bitrate = numeric;
    }
    else if(action.kind == CALF_ACTION_SET_RECORDING_GOP) {
        if(parse_decimal_string(action.value, &numeric) != 0) return -1;
        attr.gop = numeric;
    }
    else return -1;
    if(api_write_encoder_attr(channel, &attr) != 0) return -1;
    if(action.kind == CALF_ACTION_SET_ENCODING_CODEC)
        *profile_saved = save_stock_encoder_parameter(
                             camera_profile, 0, "vcodec", attr.vcodec) == 0;
    else if(action.kind == CALF_ACTION_SET_IMAGE_QUALITY)
        *profile_saved =
            save_stock_encoder_parameter(camera_profile, 0, "iq",
                                         action.value) == 0 &&
            save_stock_encoder_parameter(camera_profile, 0, "bitrate",
                                         attr.bitrate == 150000 ? "150000" :
                                         attr.bitrate == 100000 ? "100000" :
                                         attr.bitrate == 60000 ? "60000" :
                                                                  "20000") == 0;
    else if(action.kind == CALF_ACTION_SET_ENCODING_COLOR_RANGE ||
            action.kind == CALF_ACTION_SET_RECORDING_COLOR_RANGE)
        *profile_saved = save_stock_encoder_parameter(
                             camera_profile, channel, "color_range",
                             action.value) == 0;
    else if(action.kind == CALF_ACTION_SET_RECORDING_CODEC)
        *profile_saved =
            save_stock_encoder_parameter(camera_profile, 1, "cp",
                                         action.value) == 0 &&
            save_stock_encoder_parameter(camera_profile, 1, "vcodec",
                                         attr.vcodec) == 0 &&
            save_stock_encoder_parameter(camera_profile, 1, "profile",
                                         attr.profile) == 0;
    else if(action.kind == CALF_ACTION_SET_RECORDING_BITRATE)
        *profile_saved = save_stock_encoder_parameter(
                             camera_profile, 1, "bitrate", action.value) == 0;
    else if(action.kind == CALF_ACTION_SET_RECORDING_GOP)
        *profile_saved = save_stock_encoder_parameter(
                             camera_profile, 1, "gop", action.value) == 0;
    return 0;
}

static int api_perform_action(calf_action_t action)
{
    char body[256];
    char response[HTTP_BUFFER_SIZE];
    size_t used = 0;
    const char *path = (const char *)0;
    const char *type = (const char *)0;
    const char *fixed_body = (const char *)0;
    int numeric_value = 0;
    body[0] = '\0';
    if(action.kind == CALF_ACTION_SET_SPEAKER_VOLUME) {
        int requested;
        if(action.selection < 0 || action.selection > 14) return -1;
        requested = action.selection * 10;
        buffer_append(body, sizeof(body), &used,
                      "{\"action\":\"volume\",\"value\":");
        buffer_append_uint(body, sizeof(body), &used, (unsigned)requested);
        buffer_append(body, sizeof(body), &used, "}");
        if(http_request("POST", "/camera/v2/aplay", body,
                        response, sizeof(response)) != 0 ||
           !response_code_ok(response))
            return -1;
        return 0;
    }
    if(action.kind == CALF_ACTION_SET_EXPOSURE) {
        path = "/camera/v2/imgparams";
        type = "exp";
    }
    else if(action.kind == CALF_ACTION_SET_ISO) {
        path = "/camera/v2/imgparams";
        type = "iso";
    }
    else if(action.kind == CALF_ACTION_SNAPSHOT) {
        path = "/camera/v2/snapshot";
    }
    else if(action.kind == CALF_ACTION_RECORD_TOGGLE) {
        path = "/camera/v2/recording";
        fixed_body = string_equal(action.value, "stop")
                         ? "{\"action\":\"stop\"}"
                         : string_equal(action.value, "start")
                               ? "{\"action\":\"start\"}"
                               : "{\"action\":\"toggle\"}";
    }
    else if(action.kind == CALF_ACTION_SET_CAMERA_MODE) {
        path = "/camera/v2/cameramode";
        buffer_append(body, sizeof(body), &used, "{\"action\":\"start\",\"mode\":\"");
        buffer_append(body, sizeof(body), &used, action.value);
        buffer_append(body, sizeof(body), &used, "\"}");
        fixed_body = body;
    }
    else if(action.kind == CALF_ACTION_SET_WHITE_BALANCE) {
        path = "/camera/v2/imgparams";
        type = "wb";
    }
    else if(action.kind == CALF_ACTION_SET_EV) {
        path = "/camera/v2/imgparams";
        type = "ev";
    }
    else if(action.kind == CALF_ACTION_SET_ANTIFLICKER) {
        path = "/camera/v2/imgparams";
        type = "antiflicker";
    }
    else if(action.kind >= CALF_ACTION_SET_BRIGHTNESS &&
            action.kind <= CALF_ACTION_SET_DNR) {
        static const char *const types[] = {
            "brightness", "contrast", "saturation", "sharpness", "3dnr",
        };
        path = "/camera/v2/imgparams";
        type = types[(int)action.kind - (int)CALF_ACTION_SET_BRIGHTNESS];
        numeric_value = 1;
    }
    else if(action.kind == CALF_ACTION_SET_EFFECT) {
        path = "/camera/v2/imgparams";
        type = "imgeffect";
    }
    else if(action.kind == CALF_ACTION_SET_BACKLIGHT) {
        path = "/camera/v2/lcd/backlight";
        buffer_append(body, sizeof(body), &used,
                      "{\"action\":\"set_brightness\",\"brightness\":");
        buffer_append(body, sizeof(body), &used, action.value);
        buffer_append(body, sizeof(body), &used, "}");
        fixed_body = body;
    }
    else if(action.kind == CALF_ACTION_SET_LCD_POWER) {
        path = "/camera/v2/lcd/backlight";
        buffer_append(body, sizeof(body), &used, "{\"action\":\"");
        buffer_append(body, sizeof(body), &used, action.value);
        buffer_append(body, sizeof(body), &used, "\"}");
        fixed_body = body;
    }
    else if(action.kind == CALF_ACTION_SET_AUDIO_INPUT) {
        path = "/camera/v2/audioctrl";
        if(string_equal(action.value, "auto"))
            fixed_body = "{\"action\":\"input\",\"auto\":1}";
        else if(string_equal(action.value, "builtin_mic"))
            fixed_body = "{\"action\":\"input\",\"auto\":0,\"input\":0}";
        else if(string_equal(action.value, "35mm_linein"))
            fixed_body = "{\"action\":\"input\",\"auto\":0,\"input\":1}";
        else if(string_equal(action.value, "usb_mic"))
            fixed_body = "{\"action\":\"input\",\"auto\":0,\"input\":2}";
        else return -1;
    }
    else if(action.kind >= CALF_ACTION_SET_BUILTIN_MIC_VOLUME &&
            action.kind <= CALF_ACTION_SET_USB_MIC_VOLUME) {
        int input = (int)action.kind -
                    (int)CALF_ACTION_SET_BUILTIN_MIC_VOLUME;
        path = "/camera/v2/audioctrl";
        buffer_append(body, sizeof(body), &used,
                      "{\"action\":\"volume\",\"input\":");
        buffer_append_uint(body, sizeof(body), &used, (unsigned)input);
        buffer_append(body, sizeof(body), &used, ",\"value\":");
        buffer_append(body, sizeof(body), &used, action.value);
        buffer_append(body, sizeof(body), &used, "}");
        fixed_body = body;
    }
    else if(action.kind == CALF_ACTION_SET_USB_ETHERNET) {
        path = "/camera/v2/wifi";
        if(string_equal(action.value, "off"))
            fixed_body = "{\"action\":\"closeusbdc\"}";
        else if(string_equal(action.value, "win:USB1"))
            fixed_body = "{\"action\":\"setusbdc\","
                         "\"usb_port_name\":\"USB1\",\"os\":\"win\"}";
        else if(string_equal(action.value, "mac:USB1"))
            fixed_body = "{\"action\":\"setusbdc\","
                         "\"usb_port_name\":\"USB1\",\"os\":\"mac\"}";
        else if(string_equal(action.value, "win:USB2"))
            fixed_body = "{\"action\":\"setusbdc\","
                         "\"usb_port_name\":\"USB2\",\"os\":\"win\"}";
        else if(string_equal(action.value, "mac:USB2"))
            fixed_body = "{\"action\":\"setusbdc\","
                         "\"usb_port_name\":\"USB2\",\"os\":\"mac\"}";
        else return -1;
    }
    else if(action.kind == CALF_ACTION_FIRMWARE_INSTALL) {
        path = "/camera/v2/upgrade";
        fixed_body = "{\"action\":\"upgrade_from_sdcard\"}";
    }
    else return -1;

    if(type != (const char *)0) {
        buffer_append(body, sizeof(body), &used, "{\"type\":\"");
        buffer_append(body, sizeof(body), &used, type);
        buffer_append(body, sizeof(body), &used,
                      numeric_value ? "\",\"value\":" : "\",\"value\":\"");
        buffer_append(body, sizeof(body), &used, action.value);
        buffer_append(body, sizeof(body), &used, numeric_value ? "}" : "\"}");
        fixed_body = body;
    }
    {
        int stopping_recording =
            action.kind == CALF_ACTION_RECORD_TOGGLE &&
            string_equal(action.value, "stop");
        int image_attempt = 1;
        int request_result = stopping_recording
            ? http_request_with_timeout("POST", path, fixed_body, response,
                                        sizeof(response), 120)
            : action.kind == CALF_ACTION_SNAPSHOT
                  ? http_request_port_with_timeout(
                        "POST", path, fixed_body, response,
                        sizeof(response), 120, 8990)
                  : http_request("POST", path, fixed_body, response,
                                 sizeof(response));
        /* The image-state coordinator uses a one-shot netcat listener. A
         * back-to-back request can reach nginx while that listener is being
         * recreated, and a low-rate Night graph may need one frame before
         * AIQ accepts the next pair. Image setters are idempotent, so retry
         * this narrowly scoped transient instead of rejecting a setting or
         * mode change. */
        while(type != (const char *)0 && image_attempt < 3 &&
              (request_result != 0 || !response_code_ok(response))) {
            usleep(250000u);
            request_result = http_request("POST", path, fixed_body, response,
                                          sizeof(response));
            ++image_attempt;
        }
        if(request_result != 0) return -1;
    }
    return response_code_ok(response) ? 0 : -1;
}

#define API_ACTION_WORKER_VALUE_CAPACITY 256U

typedef struct {
    calf_wifi_network_t networks[CALF_WIFI_MAX_NETWORKS];
    int count;
    char current_ssid[CALF_WIFI_SSID_CAPACITY];
    char ip_address[16];
} wifi_scan_result_t;

typedef enum {
    API_WORKER_BACKEND_ACTION,
    API_WORKER_WIFI_SCAN,
} api_worker_job_t;

static int wifi_scan_collect(wifi_scan_result_t *scan);

typedef struct {
    pthread_t thread;
    calf_action_t action;
    char value[API_ACTION_WORKER_VALUE_CAPACITY];
    int result;
    int scheduled_snapshot;
    api_worker_job_t job;
    wifi_scan_result_t wifi_scan;
    int active;
    _Atomic int complete;
} api_action_worker_t;

static void *api_action_worker_main(void *argument)
{
    api_action_worker_t *worker = (api_action_worker_t *)argument;
    worker->result = worker->job == API_WORKER_WIFI_SCAN
                         ? wifi_scan_collect(&worker->wifi_scan)
                         : api_perform_action(worker->action);
    atomic_store_explicit(&worker->complete, 1, memory_order_release);
    return (void *)0;
}

static int api_action_worker_submit(api_action_worker_t *worker,
                                    calf_action_t action,
                                    int scheduled_snapshot,
                                    api_worker_job_t job)
{
    size_t value_length = string_length(action.value);
    if(worker->active || value_length >= sizeof(worker->value)) return -1;
    worker->action = action;
    if(action.value != (const char *)0) {
        string_copy(worker->value, sizeof(worker->value), action.value);
        worker->action.value = worker->value;
    }
    else {
        worker->value[0] = '\0';
        worker->action.value = (const char *)0;
    }
    worker->scheduled_snapshot = scheduled_snapshot;
    worker->job = job;
    worker->result = -1;
    atomic_store_explicit(&worker->complete, 0, memory_order_relaxed);
    if(pthread_create(&worker->thread, (const void *)0,
                      api_action_worker_main, worker) != 0) {
        secure_zero(worker->value, sizeof(worker->value));
        return -1;
    }
    worker->active = 1;
    return 0;
}

static int api_action_worker_take(api_action_worker_t *worker,
                                  calf_action_t *action,
                                  char *value, size_t value_capacity,
                                  int *result, int *scheduled_snapshot,
                                  api_worker_job_t *job)
{
    if(!worker->active ||
       !atomic_load_explicit(&worker->complete, memory_order_acquire))
        return 0;
    (void)pthread_join(worker->thread, (void **)0);
    *action = worker->action;
    if(worker->action.value != (const char *)0) {
        string_copy(value, value_capacity, worker->value);
        action->value = value;
    }
    *result = worker->result;
    *scheduled_snapshot = worker->scheduled_snapshot;
    *job = worker->job;
    worker->active = 0;
    secure_zero(worker->value, sizeof(worker->value));
    return 1;
}

static int api_action_worker_busy(const api_action_worker_t *worker)
{
    return worker->active;
}

static void api_action_worker_join(api_action_worker_t *worker)
{
    if(!worker->active) return;
    (void)pthread_join(worker->thread, (void **)0);
    worker->active = 0;
    secure_zero(worker->value, sizeof(worker->value));
}

static int api_set_image_direct_mode(const char *type, const char *value,
                                     int fixed)
{
    char body[96];
    char response[HTTP_BUFFER_SIZE];
    char detail[192];
    size_t used = 0;
    body[0] = '\0';
    buffer_append(body, sizeof(body), &used, "{\"type\":\"");
    buffer_append(body, sizeof(body), &used, type);
    buffer_append(body, sizeof(body), &used, "\",\"value\":\"");
    buffer_append(body, sizeof(body), &used, value);
    buffer_append(body, sizeof(body), &used,
                  fixed ? "\",\"fixed\":true}" : "\"}");
    if(http_request_port_with_timeout(
           "POST", "/camera/v2/imgparams", body, response,
           sizeof(response), 3, 8989) != 0) {
        used = 0;
        detail[0] = '\0';
        buffer_append(detail, sizeof(detail), &used, "transport,type:");
        buffer_append(detail, sizeof(detail), &used, type);
        buffer_append(detail, sizeof(detail), &used, ",value:");
        buffer_append(detail, sizeof(detail), &used, value);
        supervisor_log_event("NIGHT_IMAGE_DIRECT", detail, -1);
        return -1;
    }
    if(!response_code_ok(response)) {
        used = 0;
        detail[0] = '\0';
        buffer_append(detail, sizeof(detail), &used, "backend,type:");
        buffer_append(detail, sizeof(detail), &used, type);
        buffer_append(detail, sizeof(detail), &used, ",value:");
        buffer_append(detail, sizeof(detail), &used, value);
        buffer_append(detail, sizeof(detail), &used, ",response:");
        buffer_append(detail, sizeof(detail), &used, response);
        supervisor_log_event("NIGHT_IMAGE_DIRECT", detail, -1);
        return -1;
    }
    return 0;
}

static double exposure_seconds_for_value(const char *value)
{
    if(string_equal(value, "0.0666667")) return 1.0 / 15.0;
    if(string_equal(value, "0.125")) return 0.125;
    if(string_equal(value, "0.25")) return 0.25;
    if(string_equal(value, "0.5")) return 0.5;
    if(string_equal(value, "1")) return 1.0;
    if(string_equal(value, "2")) return 2.0;
    if(string_equal(value, "4")) return 4.0;
    if(string_equal(value, "8")) return 8.0;
    if(string_equal(value, "12")) return 12.0;
    return 0.0;
}

static int iso_number_for_value(const char *value)
{
    static const int iso_numbers[] = {
        0, 100, 200, 400, 800, 1600, 3200, 6400, 12800,
    };
    int index = calf_iso_index_for_value(value);
    return index >= 0 &&
           index < (int)(sizeof(iso_numbers) / sizeof(iso_numbers[0]))
               ? iso_numbers[index] : 0;
}

static int nearest_preview_iso(double desired, int *clipped)
{
    static const int choices[] = {
        100, 200, 400, 800, 1600, 3200, 6400, 12800,
    };
    int nearest = choices[0];
    double distance = desired > nearest ? desired - nearest : nearest - desired;
    size_t index;
    *clipped = desired > 12800.0;
    for(index = 1; index < sizeof(choices) / sizeof(choices[0]); ++index) {
        double candidate_distance = desired > choices[index]
                                        ? desired - choices[index]
                                        : choices[index] - desired;
        if(candidate_distance < distance) {
            nearest = choices[index];
            distance = candidate_distance;
        }
    }
    return nearest;
}

static const char *iso_value_for_number(int iso)
{
    static const int numbers[] = {
        100, 200, 400, 800, 1600, 3200, 6400, 12800,
    };
    static const char *const values[] = {
        "iso100", "iso200", "iso400", "iso800", "iso1600",
        "iso3200", "iso6400", "iso12800",
    };
    size_t index;
    for(index = 0; index < sizeof(numbers) / sizeof(numbers[0]); ++index)
        if(numbers[index] == iso) return values[index];
    return "iso100";
}

static int api_set_night_preview_transaction(int fps,
                                             const char *exposure,
                                             const char *iso)
{
    char body[128];
    char response[HTTP_BUFFER_SIZE];
    char detail[192];
    size_t used = 0;
    body[0] = '\0';
    buffer_append(body, sizeof(body), &used, "{\"fps\":");
    buffer_append_uint(body, sizeof(body), &used, (unsigned)fps);
    buffer_append(body, sizeof(body), &used, ",\"exposure\":\"");
    buffer_append(body, sizeof(body), &used, exposure);
    buffer_append(body, sizeof(body), &used, "\",\"iso\":\"");
    buffer_append(body, sizeof(body), &used, iso);
    buffer_append(body, sizeof(body), &used, "\"}");
    if(http_request_port_with_timeout(
           "POST", "/camera/v2/nightpreview", body, response,
           sizeof(response), 8, 8989) != 0)
        return -1;
    if(response_code_ok(response)) return 0;
    used = 0;
    detail[0] = '\0';
    buffer_append(detail, sizeof(detail), &used, "backend,fps:");
    buffer_append_uint(detail, sizeof(detail), &used, (unsigned)fps);
    buffer_append(detail, sizeof(detail), &used, ",response:");
    buffer_append(detail, sizeof(detail), &used, response);
    supervisor_log_event("NIGHT_TRANSACTION", detail, -1);
    return -1;
}

static void api_restore_standard_preview_timing(void)
{
    (void)api_set_night_preview_transaction(
        30, "0.0333333", "iso100");
}

static int api_apply_night_preview(calf_ui_t *ui,
                                   const char *exposure_override,
                                   const char *iso_override)
{
    const char *exposure = exposure_override;
    const char *iso = iso_override;
    double seconds;
    double desired;
    int actual_iso;
    int preview_iso;
    int preview_fps;
    const char *preview_exposure;
    int clipped;
    if(exposure == (const char *)0 && ui->exposure_known)
        exposure = calf_exposure_value((size_t)ui->exposure_index);
    if(iso == (const char *)0 && ui->iso_known)
        iso = calf_iso_value((size_t)ui->iso_index);
    if(exposure == (const char *)0) exposure = "0.5";
    if(iso == (const char *)0) iso = "iso400";
    seconds = exposure_seconds_for_value(exposure);
    actual_iso = iso_number_for_value(iso);
    if(seconds <= 0.0 || actual_iso <= 0) return -1;
    preview_fps = night_preview_fps_for_exposure(exposure);
    preview_exposure = night_preview_exposure_for_fps(preview_fps);
    if(preview_fps == 0 || preview_exposure == (const char *)0) return -1;
    desired = (double)actual_iso * seconds * (double)preview_fps;
    preview_iso = nearest_preview_iso(desired, &clipped);

    /* The accepted profile remains at the real night values. These direct
     * calls affect only the live graph, so capture and EXIF continue to use
     * the selected exposure and ISO. */
    supervisor_log_night_preview(
        "begin", exposure, iso, preview_iso, preview_fps, 1, 0);
    if(api_set_night_preview_transaction(
           preview_fps, preview_exposure,
           iso_value_for_number(preview_iso)) != 0) {
        supervisor_log_night_preview(
            "failed", exposure, iso, preview_iso, preview_fps, 1, -1);
        return -1;
    }
    calf_ui_set_night_preview(ui, preview_iso, clipped);
    supervisor_log_night_preview(
        "applied", exposure, iso, preview_iso, preview_fps, 1, 0);
    return 0;
}

static int api_apply_actual_image_values(calf_ui_t *ui,
                                         const char *exposure,
                                         const char *iso)
{
    calf_action_t action;
    action.kind = CALF_ACTION_SET_EXPOSURE;
    action.value = exposure;
    action.selection = calf_ui_exposure_index_for_value(exposure);
    if(action.selection < 0 || api_perform_action(action) != 0) return -1;
    action.kind = CALF_ACTION_SET_ISO;
    action.value = iso;
    action.selection = calf_iso_index_for_value(iso);
    if(action.selection < 0 || api_perform_action(action) != 0) return -1;
    if(calf_ui_sync_image_value(ui, CALF_ACTION_SET_EXPOSURE, exposure) != 0 ||
       calf_ui_sync_image_value(ui, CALF_ACTION_SET_ISO, iso) != 0)
        return -1;
    return 0;
}

static int api_apply_audio_input_state(const audio_input_state_t *state)
{
    static const char *const sources[] = {
        "builtin_mic", "35mm_linein", "usb_mic",
    };
    calf_action_t action;
    char values[CALF_AUDIO_INPUT_VOLUME_COUNT][4];
    int index;
    if(!state->valid || !audio_input_state_valid(state)) return -1;
    action.kind = CALF_ACTION_SET_AUDIO_INPUT;
    action.value = state->automatic ? "auto" : sources[state->input_type];
    action.selection = -1;
    if(api_perform_action(action) != 0) return -1;
    for(index = 0; index < CALF_AUDIO_INPUT_VOLUME_COUNT; ++index) {
        size_t used = 0;
        values[index][0] = '\0';
        buffer_append_uint(values[index], sizeof(values[index]), &used,
                           (unsigned)state->volume[index]);
        action.kind = (calf_action_kind_t)(
            (int)CALF_ACTION_SET_BUILTIN_MIC_VOLUME + index);
        action.value = values[index];
        if(api_perform_action(action) != 0) return -1;
    }
    return 0;
}

static int api_start_initial_camera_graph(const char *profile)
{
    calf_action_t action;
    action.kind = CALF_ACTION_SET_CAMERA_MODE;
    action.value = profile;
    action.selection = 1;
    return api_perform_action(action);
}

int api_post_action(const char *path, const char *body)
{
    char response[HTTP_BUFFER_SIZE];
    if(http_request("POST", path, body, response, sizeof(response)) != 0)
        return -1;
    return response_code_ok(response) ? 0 : -1;
}

int api_stop_camera_graph(void)
{
    return api_post_action("/camera/v2/cameramode", "{\"action\":\"stop\"}");
}

static int api_power_off(void)
{
    char response[HTTP_BUFFER_SIZE];
    if(http_request("POST", "/camera/v2/poweroff", (const char *)0,
                    response, sizeof(response)) != 0)
        return -1;
    return response_code_ok(response) ? 0 : -1;
}

static int api_poll_status(calf_backend_status_t *status)
{
    char response[HTTP_BUFFER_SIZE];
    int value;
    const char *recording_section;
    const char *live_section;
    const char *usb_ethernet_section;
    if(http_request("POST", "/camera/v2/systemstatus", "{\"ssids\":4398}",
                    response, sizeof(response)) != 0 || !response_code_ok(response)) {
        status->online = 0;
        return -1;
    }
    status->online = 1;
    if(parse_integer_after(response, "\"batt_cap\"", &value) == 0)
        status->battery_percent = value;
    if(parse_integer_after(response, "\"is_usb_supply\"", &value) == 0)
        status->usb_power = value != 0;
    status->usb_ethernet_enabled = -1;
    status->usb_ethernet_configured = -1;
    status->usb_ethernet_port[0] = '\0';
    status->usb_ethernet_os[0] = '\0';
    status->usb_ethernet_ip_address[0] = '\0';
    usb_ethernet_section = find_text(response, "\"usbnet\"");
    if(usb_ethernet_section != (const char *)0) {
        if(parse_integer_after(usb_ethernet_section, "\"enabled\"", &value) == 0)
            status->usb_ethernet_enabled = value != 0;
        if(parse_integer_after(usb_ethernet_section, "\"configured\"", &value) == 0)
            status->usb_ethernet_configured = value != 0;
        (void)parse_scalar_after(usb_ethernet_section, "\"port\"",
                                 status->usb_ethernet_port,
                                 sizeof(status->usb_ethernet_port));
        (void)parse_scalar_after(usb_ethernet_section, "\"os\"",
                                 status->usb_ethernet_os,
                                 sizeof(status->usb_ethernet_os));
        (void)parse_scalar_after(
            usb_ethernet_section, "\"ipaddr\"",
            status->usb_ethernet_ip_address,
            sizeof(status->usb_ethernet_ip_address));
    }
    if(parse_integer_after(response, "\"free_mb\"", &value) == 0)
        status->storage_free_mb = value;
    if(parse_integer_after(response, "\"sys_temp\"", &value) == 0)
        status->system_temp = value;
    if(parse_integer_after(response, "\"core_temp\"", &value) == 0)
        status->core_temp = value;
    string_copy(status->ethernet_ip_address,
                sizeof(status->ethernet_ip_address), "0.0.0.0");
    {
        const char *ethernet_section = find_text(response, "\"eth0\"");
        if(ethernet_section != (const char *)0)
            (void)parse_scalar_after(
                ethernet_section, "\"ipaddr\"",
                status->ethernet_ip_address,
                sizeof(status->ethernet_ip_address));
    }
    recording_section = find_text(response, "\"rs\"");
    if(recording_section != (const char *)0 &&
       parse_integer_after(recording_section, "\"is_running\"", &value) == 0)
        status->recording = value != 0;
    if(recording_section != (const char *)0 &&
       parse_integer_after(recording_section, "\"duration\"", &value) == 0)
        status->recording_seconds = value >= 0 ? value : 0;
    else if(!status->recording)
        status->recording_seconds = 0;
    status->streaming = -1;
    live_section = find_text(response, "\"ls\"");
    if(live_section != (const char *)0 &&
       parse_integer_after(live_section, "\"duration\"", &value) == 0) {
        status->streaming = value != 0;
        if(!status->streaming &&
           parse_integer_after(live_section, "\"video_bps\"", &value) == 0)
            status->streaming = value != 0;
    }
    status->playback = -1;
    if(http_request("GET", "/camera/v2/playback", (const char *)0,
                    response, sizeof(response)) == 0 &&
       response_code_ok(response) &&
       parse_integer_after(response, "\"running\"", &value) == 0)
        status->playback = value != 0;
    return 0;
}

static void sync_recording_timer(calf_ui_t *ui,
                                 calf_backend_status_t *status,
                                 uint64_t now_ms,
                                 uint64_t *next_tick_ms)
{
    int was_recording = ui->status.recording;
    int displayed_seconds = ui->status.recording_seconds;
    if(status->recording) {
        if(was_recording && displayed_seconds > status->recording_seconds)
            status->recording_seconds = displayed_seconds;
        if(!was_recording || *next_tick_ms == 0)
            *next_tick_ms = now_ms + 1000u;
    }
    else {
        status->recording_seconds = 0;
        *next_tick_ms = 0;
    }
    calf_ui_set_status(ui, status);
}

static void advance_recording_timer(calf_ui_t *ui,
                                    calf_backend_status_t *status,
                                    uint64_t now_ms,
                                    uint64_t *next_tick_ms)
{
    uint64_t ticks;
    uint64_t available;
    if(!status->recording || *next_tick_ms == 0 || now_ms < *next_tick_ms)
        return;
    ticks = (now_ms - *next_tick_ms) / 1000u + 1u;
    available = (uint64_t)(2147483647 - status->recording_seconds);
    if(ticks > available) ticks = available;
    status->recording_seconds += (int)ticks;
    *next_tick_ms += ticks * 1000u;
    calf_ui_set_status(ui, status);
}

static int api_poll_motion(calf_ui_t *ui)
{
    char response[HTTP_BUFFER_SIZE];
    int gyro_x;
    int gyro_y;
    int gyro_z;
    int acceleration_y;
    int acceleration_z;
    if(http_request_with_timeout(
           "GET", "/camera/v2/imu_sample", (const char *)0,
           response, sizeof(response), 1) != 0 ||
       !response_code_ok(response) ||
       parse_integer_after(response, "\"gyro_x\"", &gyro_x) != 0 ||
       parse_integer_after(response, "\"gyro_y\"", &gyro_y) != 0 ||
       parse_integer_after(response, "\"gyro_z\"", &gyro_z) != 0 ||
       parse_integer_after(response, "\"acc_y\"", &acceleration_y) != 0 ||
       parse_integer_after(response, "\"acc_z\"", &acceleration_z) != 0) {
        calf_ui_set_motion(ui, 0, 0, 0, 0);
        calf_ui_set_level(ui, 0, 0, 0);
        return -1;
    }
    calf_ui_set_motion(ui, gyro_x, gyro_y, gyro_z, 1);
    calf_ui_set_level(ui, acceleration_y, acceleration_z, 1);
    return 0;
}

static int parse_histogram_response(
    const char *response, uint32_t bins[CALF_HISTOGRAM_BIN_COUNT])
{
    const char *cursor = find_text(response, "\"hist\"");
    size_t index;
    if(cursor == (const char *)0) return -1;
    cursor += string_length("\"hist\"");
    while(*cursor == ' ' || *cursor == '\t') ++cursor;
    if(*cursor++ != ':') return -1;
    while(*cursor == ' ' || *cursor == '\t') ++cursor;
    if(*cursor++ != '[') return -1;
    for(index = 0; index < CALF_HISTOGRAM_BIN_COUNT; ++index) {
        uint32_t value = 0;
        int digit = 0;
        while(*cursor == ' ' || *cursor == '\t') ++cursor;
        while(*cursor >= '0' && *cursor <= '9') {
            uint32_t next = (uint32_t)(*cursor - '0');
            if(value > (UINT32_MAX - next) / 10u) return -1;
            value = value * 10u + next;
            digit = 1;
            ++cursor;
        }
        if(!digit) return -1;
        bins[index] = value;
        while(*cursor == ' ' || *cursor == '\t') ++cursor;
        if(index + 1u < CALF_HISTOGRAM_BIN_COUNT) {
            if(*cursor++ != ',') return -1;
        }
        else if(*cursor != ']') return -1;
    }
    return 0;
}

static int api_poll_histogram(calf_ui_t *ui, int gallery)
{
    char response[HTTP_BUFFER_SIZE];
    uint32_t bins[CALF_HISTOGRAM_BIN_COUNT];
    if(http_request_with_timeout(
           "GET", "/camera/v2/exphist", (const char *)0,
           response, sizeof(response), 1) != 0 ||
       !response_code_ok(response) ||
       parse_histogram_response(response, bins) != 0) {
        if(gallery)
            calf_ui_set_gallery_histogram(ui, (const uint32_t *)0, 0);
        else
            calf_ui_set_live_histogram(ui, (const uint32_t *)0, -1);
        return -1;
    }
    if(gallery)
        calf_ui_set_gallery_histogram(ui, bins, 1);
    else
        calf_ui_set_live_histogram(ui, bins, 1);
    return 0;
}

static int api_get_integer(const char *path, const char *key, int *value)
{
    char response[HTTP_BUFFER_SIZE];
    if(http_request("GET", path, (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response))
        return -1;
    return parse_integer_after(response, key, value);
}

static int wifi_ssid_safe(const char *ssid)
{
    size_t length = string_length(ssid);
    size_t index;
    if(length == 0 || length >= CALF_WIFI_SSID_CAPACITY) return 0;
    for(index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)ssid[index];
        if(character < 0x20 || character > 0x7e || character == '/' ||
           character == '\\' || character == '"')
            return 0;
    }
    return 1;
}

static int wifi_password_safe(const char *password)
{
    size_t length = string_length(password);
    size_t index;
    if(length != 0 && (length < 8 || length >= CALF_WIFI_PASSWORD_CAPACITY))
        return 0;
    for(index = 0; index < length; ++index)
        if((unsigned char)password[index] < 0x20 ||
           (unsigned char)password[index] > 0x7e)
            return 0;
    return 1;
}

static int wifi_scan_collect(wifi_scan_result_t *scan)
{
    char response[HTTP_BUFFER_SIZE];
    char status_response[HTTP_BUFFER_SIZE];
    const char *cursor;
    int count = 0;
    int attempts = 0;
    int enable_attempted = 0;
    int index;
retry:
    count = 0;
    ++attempts;
    if(http_request("GET", "/camera/v2/scanwifi", (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response)) {
        char *enable[] = {"/app/bin/calf-wlan", "1", (char *)0};
        char *start[] = {"/app/bin/wifiservice", "start", (char *)0};
        if(enable_attempted || attempts >= 4) return -1;
        enable_attempted = 1;
        if(run_program(enable[0], enable) != 0 &&
           run_program(start[0], start) != 0)
            return -1;
        usleep(1000000u);
        goto retry;
    }
    scan->current_ssid[0] = '\0';
    scan->ip_address[0] = '\0';
    if(http_request("GET", "/camera/v2/wifi", (const char *)0,
                    status_response, sizeof(status_response)) == 0 &&
       response_code_ok(status_response)) {
        (void)parse_scalar_after(status_response, "\"essid\"",
                                 scan->current_ssid,
                                 sizeof(scan->current_ssid));
        (void)parse_scalar_after(status_response, "\"ipaddr\"",
                                 scan->ip_address,
                                 sizeof(scan->ip_address));
    }
    cursor = find_text(response, "\"list\"");
    if(cursor == (const char *)0) return -1;
    while(count < CALF_WIFI_MAX_NETWORKS &&
          (cursor = find_text(cursor, "\"essid\"")) != (const char *)0) {
        const char *object_end = find_text(cursor, "}");
        const char *quality = find_text(cursor, "\"qual\"");
        const char *level = find_text(cursor, "\"level\"");
        char ssid[CALF_WIFI_SSID_CAPACITY];
        int duplicate = 0;
        if(object_end == (const char *)0 || quality == (const char *)0 ||
           level == (const char *)0 || quality > object_end ||
           level > object_end)
            break;
        if(parse_scalar_after(cursor, "\"essid\"", ssid, sizeof(ssid)) == 0 &&
           wifi_ssid_safe(ssid) &&
           parse_integer_after(quality, "\"qual\"",
                               &scan->networks[count].quality) == 0 &&
           parse_integer_after(level, "\"level\"",
                               &scan->networks[count].level) == 0) {
            for(index = 0; index < count; ++index)
                if(string_equal(scan->networks[index].ssid, ssid))
                    duplicate = 1;
            if(!duplicate) {
                string_copy(scan->networks[count].ssid,
                            sizeof(scan->networks[count].ssid), ssid);
                ++count;
            }
        }
        cursor = object_end + 1;
    }
    if(count == 0 && attempts < 4) {
        usleep(750000u);
        goto retry;
    }
    for(index = 1; index < count; ++index) {
        calf_wifi_network_t item = scan->networks[index];
        int position = index;
        while(position > 0 &&
              scan->networks[position - 1].level < item.level) {
            scan->networks[position] = scan->networks[position - 1];
            --position;
        }
        scan->networks[position] = item;
    }
    scan->count = count;
    return 0;
}

static int wifi_connect_saved(const char *ssid)
{
    char profile_path[96];
    size_t used = 0;
    int descriptor;
    char *arguments[] = {
        "/app/bin/wifiservice", "connect", (char *)ssid, (char *)0,
    };
    if(!wifi_ssid_safe(ssid)) return -1;
    profile_path[0] = '\0';
    buffer_append(profile_path, sizeof(profile_path), &used,
                  "/local/etc/wpa_supplicant_");
    buffer_append(profile_path, sizeof(profile_path), &used, ssid);
    buffer_append(profile_path, sizeof(profile_path), &used, ".conf");
    if(used + 1 >= sizeof(profile_path)) return -1;
    descriptor = open(profile_path, O_RDONLY | O_NOFOLLOW);
    if(descriptor < 0) return WIFI_CONNECT_NO_PROFILE;
    close(descriptor);
    return run_program(arguments[0], arguments);
}

static int wifi_connect_password(const char *ssid, const char *password)
{
    char *arguments[] = {
        "/app/bin/wifiservice", "set", (char *)ssid,
        (char *)password, (char *)0,
    };
    if(!wifi_ssid_safe(ssid) || !wifi_password_safe(password)) return -1;
    if(password[0] == '\0') arguments[3] = (char *)0;
    return run_program(arguments[0], arguments);
}

static int wifi_set_enabled(int enabled)
{
    char *enable_arguments[] = {
        "/app/bin/calf-wlan", "1", (char *)0,
    };
    char *disable_arguments[] = {
        "/app/bin/calf-wlan", "0", (char *)0,
    };
    char **arguments = enabled ? enable_arguments : disable_arguments;
    return run_program(arguments[0], arguments) == 0 ? 0 : -1;
}

static int wifi_start_service(void)
{
    char *arguments[] = {
        "/app/bin/wifiservice", "start", (char *)0,
    };
    return run_program(arguments[0], arguments) == 0 ? 0 : -1;
}

static void wifi_refresh_connection(calf_ui_t *ui,
                                    const char *fallback_ssid)
{
    char response[HTTP_BUFFER_SIZE];
    char current_ssid[CALF_WIFI_SSID_CAPACITY];
    char ip_address[16];
    string_copy(current_ssid, sizeof(current_ssid),
                wifi_ssid_safe(fallback_ssid) ? fallback_ssid : "");
    ip_address[0] = '\0';
    if(http_request("GET", "/camera/v2/wifi", (const char *)0,
                    response, sizeof(response)) == 0 &&
       response_code_ok(response)) {
        char reported_ssid[CALF_WIFI_SSID_CAPACITY];
        reported_ssid[0] = '\0';
        if(parse_scalar_after(response, "\"essid\"", reported_ssid,
                              sizeof(reported_ssid)) == 0 &&
           wifi_ssid_safe(reported_ssid))
            string_copy(current_ssid, sizeof(current_ssid), reported_ssid);
        (void)parse_scalar_after(response, "\"ipaddr\"", ip_address,
                                 sizeof(ip_address));
    }
    calf_ui_set_wifi_connection(ui, current_ssid, ip_address);
}

static int api_stream_has_no_traffic(const char *path, int check_duration)
{
    char response[HTTP_BUFFER_SIZE];
    int duration = 0;
    int video_bps = 0;
    if(http_request("GET", path, (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response))
        return -1;
    if(check_duration) {
        if(parse_integer_after(response, "\"duration\"", &duration) != 0)
            return -1;
        if(duration != 0) return 0;
    }
    if(parse_integer_after(response, "\"video_bps\"", &video_bps) != 0)
        return -1;
    return video_bps <= 0 ? 1 : 0;
}

static int api_deep_idle_is_safe(calf_backend_status_t *status)
{
    static const char *const stream_paths[] = {
        "/camera/v2/live",
        "/camera/v2/rtmp",
        "/camera/v2/rtsp",
    };
    char response[HTTP_BUFFER_SIZE];
    char uvc_status[24];
    int value;
    size_t index;
    if(api_poll_status(status) != 0 || !status->online) {
        supervisor_log_event("DEEP_IDLE_CHECK", "status", -1);
        return 0;
    }
    if(status->recording != 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "recording-status", -1);
        return 0;
    }
    if(status->streaming != 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "streaming-status", -1);
        return 0;
    }
    if(status->playback != 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "playback-status", -1);
        return 0;
    }
    if(api_get_integer("/camera/v2/recording", "\"running\"", &value) != 0 ||
       value != 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "recording", -1);
        return 0;
    }
    if(api_get_integer("/camera/v2/playback", "\"running\"", &value) != 0 ||
       value != 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "playback", -1);
        return 0;
    }
    for(index = 0; index < sizeof(stream_paths) / sizeof(stream_paths[0]);
        ++index) {
        if(api_stream_has_no_traffic(stream_paths[index], index == 0) != 1) {
            supervisor_log_event("DEEP_IDLE_CHECK", stream_paths[index], -1);
            return 0;
        }
    }
    if(api_stream_has_no_traffic("/camera/v2/srt", 0) != 1) {
        supervisor_log_event("DEEP_IDLE_CHECK", "/camera/v2/srt", -1);
        return 0;
    }
    if(api_get_integer("/camera/v2/openstream", "\"duration\"", &value) != 0 ||
       value >= 0) {
        supervisor_log_event("DEEP_IDLE_CHECK", "openstream", -1);
        return 0;
    }
    if(http_request("GET", "/camera/v2/uvc", (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response) ||
       parse_scalar_after(response, "\"status\"", uvc_status,
                          sizeof(uvc_status)) != 0 ||
       !string_equal(uvc_status, "disabled")) {
        supervisor_log_event("DEEP_IDLE_CHECK", "uvc", -1);
        return 0;
    }
    supervisor_log_event("DEEP_IDLE_CHECK", "safe", 0);
    return 1;
}

static const char *firmware_preflight(calf_ui_t *ui,
                                      calf_backend_status_t *status,
                                      int *size_mb, char digest[65])
{
    status->battery_percent = -1;
    status->usb_power = -1;
    if(api_poll_status(status) != 0 || !status->online)
        return "CAMERA STATUS UNAVAILABLE";
    calf_ui_set_status(ui, status);
    if(status->usb_power != 1) return "CONNECT USB POWER";
    if(status->battery_percent < 50) return "BATTERY MUST BE 50%";
    if(!api_deep_idle_is_safe(status)) {
        calf_ui_set_status(ui, status);
        return "STOP CAMERA ACTIVITY";
    }
    calf_ui_set_status(ui, status);
    if(firmware_update_validate(size_mb, digest) != 0)
        return "VALID UPDATE + HASH NOT FOUND";
    return (const char *)0;
}

static int api_set_lcd_power(calf_ui_t *ui, int turn_on)
{
    calf_action_t action;
    int result;
    action.kind = CALF_ACTION_SET_LCD_POWER;
    action.value = turn_on ? "turn_on" : "turn_off";
    action.selection = turn_on ? 1 : 0;
    result = api_perform_action(action);
    calf_ui_complete_action(ui, action, result == 0,
                            result == 0 ? "LCD UPDATED" : "LCD ERROR");
    return result;
}

static int api_sync_image_state(calf_ui_t *ui, int apply_to_backend);

static int api_restore_primary_graph(calf_ui_t *ui, const char *profile,
                                     const audio_input_state_t *audio_state)
{
    supervisor_log_event("GRAPH_RESTORE", "stage:begin", 0);
    if(api_start_initial_camera_graph(profile) != 0) {
        supervisor_log_event("GRAPH_RESTORE", "stage:camera", -1);
        return -1;
    }
    if(api_apply_encoder_profile(profile) != 0) {
        supervisor_log_event("GRAPH_RESTORE", "stage:encoder", -1);
        return -1;
    }
    if(api_sync_image_state(ui, 1) < 0) {
        supervisor_log_event("GRAPH_RESTORE", "stage:image-state", -1);
        return -1;
    }
    if(audio_state->valid && api_apply_audio_input_state(audio_state) != 0) {
        supervisor_log_event("GRAPH_RESTORE", "stage:audio", -1);
        return -1;
    }
    if(ui->capture_mode == CALF_CAPTURE_NIGHT &&
       api_apply_night_preview(ui, (const char *)0,
                              (const char *)0) != 0) {
        supervisor_log_event("GRAPH_RESTORE", "stage:night-preview", -1);
        return -1;
    }
    (void)api_sync_encoder_state(ui, profile);
    ui->lens_index = 1;
    ui->lens_known = 1;
    supervisor_log_event("GRAPH_RESTORE", "stage:complete", 0);
    return 0;
}

static int api_sync_audio_state(calf_ui_t *ui,
                                audio_input_state_t *saved_state)
{
    static const calf_action_kind_t volume_actions[] = {
        CALF_ACTION_SET_BUILTIN_MIC_VOLUME,
        CALF_ACTION_SET_LINEIN_VOLUME,
        CALF_ACTION_SET_USB_MIC_VOLUME,
    };
    static const char *const volume_keys[] = {
        "\"inputvol0\"", "\"inputvol1\"", "\"inputvol2\"",
    };
    char response[HTTP_BUFFER_SIZE];
    audio_input_state_t current;
    int value;
    size_t index;
    if(http_request("GET", "/camera/v2/audioinfo", (const char *)0,
                    response, sizeof(response)) != 0 ||
       !response_code_ok(response) ||
       parse_integer_after(response, "\"autoinput\"", &current.automatic) != 0 ||
       parse_integer_after(response, "\"inputtype\"", &current.input_type) != 0)
        return -1;
    current.valid = 1;
    for(index = 0; index < sizeof(volume_actions) / sizeof(volume_actions[0]);
        ++index) {
        if(parse_integer_after(response, volume_keys[index], &value) != 0)
            return -1;
        current.volume[index] = value;
    }
    if(!audio_input_state_valid(&current)) return -1;
    (void)calf_ui_sync_audio_input(
        ui, current.automatic != 0, current.input_type);
    for(index = 0; index < sizeof(volume_actions) / sizeof(volume_actions[0]);
        ++index)
        (void)calf_ui_sync_audio_volume(
            ui, volume_actions[index], current.volume[index]);
    if(!audio_input_state_equal(&current, saved_state)) {
        if(save_audio_input_state(&current) != 0) return -1;
        *saved_state = current;
    }
    return 0;
}

static int api_sync_image_state(calf_ui_t *ui, int apply_to_backend)
{
    static const struct {
        const char *key;
        const char *profile_key;
        const char *default_value;
        calf_action_kind_t kind;
    } fields[] = {
        {"\"exp\"", "exp", "-1", CALF_ACTION_SET_EXPOSURE},
        {"\"iso\"", "iso", "auto", CALF_ACTION_SET_ISO},
        {"\"wb\"", "wb", "auto", CALF_ACTION_SET_WHITE_BALANCE},
        {"\"ev\"", "ev", "0", CALF_ACTION_SET_EV},
        {"\"brightness\"", "brightness", "10", CALF_ACTION_SET_BRIGHTNESS},
        {"\"contrast\"", "contrast", "10", CALF_ACTION_SET_CONTRAST},
        {"\"saturation\"", "saturation", "10", CALF_ACTION_SET_SATURATION},
        {"\"sharpness\"", "sharpness", "10", CALF_ACTION_SET_SHARPNESS},
        {"\"3dnr\"", "_3dnr", "5", CALF_ACTION_SET_DNR},
        {"\"antiflicker\"", "antiflicker", "auto", CALF_ACTION_SET_ANTIFLICKER},
        {"\"imgeffect\"", "effect", "none", CALF_ACTION_SET_EFFECT},
    };
    char response[HTTP_BUFFER_SIZE];
    static char profile[PROFILE_BUFFER_SIZE];
    const char *profile_section = (const char *)0;
    const char *profile_end = (const char *)0;
    char value[32];
    size_t index;
    int found = 0;
    int apply_failed = 0;
    int coordinator_available =
        http_request("GET", "/camera/v2/imgparams", (const char *)0,
                     response, sizeof(response)) == 0 &&
        response_code_ok(response);

    if(!coordinator_available) {
        if(read_text_file(STOCK_PROFILE, profile, sizeof(profile)) != 0)
            return 1;
        if(starts_with(profile, "image_params:\n")) profile_section = profile;
        else {
            profile_section = find_text(profile, "\nimage_params:\n");
            if(profile_section != (const char *)0) ++profile_section;
        }
        if(profile_section == (const char *)0) return 1;
        profile_end = top_level_section_end(profile_section);
    }
    for(index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        int parsed = -1;
        int using_default = 0;
        int night_transient = night_image_action_is_transient(
            ui->capture_mode, fields[index].kind);
        if(night_transient) {
            char night_exposure[16];
            char night_iso[16];
            /* The backend's accepted image state describes ordinary graph
             * values, while Night deliberately keeps a transient preview
             * pair.  The persisted Night pair is authoritative for its UI,
             * capture coordinator, and later preview restoration. */
            load_mode_image_values(CALF_CAPTURE_NIGHT,
                                   night_exposure, night_iso);
            string_copy(value, sizeof(value),
                        fields[index].kind == CALF_ACTION_SET_EXPOSURE
                            ? night_exposure : night_iso);
            parsed = 0;
        }
        else if(coordinator_available)
            parsed = parse_scalar_after(response, fields[index].key,
                                        value, sizeof(value));
        else {
            char pattern[32];
            size_t used = 0;
            const char *line;
            pattern[0] = '\0';
            buffer_append(pattern, sizeof(pattern), &used, "\n  ");
            buffer_append(pattern, sizeof(pattern), &used,
                          fields[index].profile_key);
            buffer_append(pattern, sizeof(pattern), &used, ":");
            line = find_text(profile_section, pattern);
            if(line != (const char *)0 && line < profile_end)
                parsed = parse_scalar_after(line + 1,
                                            fields[index].profile_key,
                                            value, sizeof(value));
        }
        if(parsed != 0 && apply_to_backend) {
            string_copy(value, sizeof(value), fields[index].default_value);
            parsed = 0;
            using_default = 1;
        }
        if(parsed == 0) {
            calf_action_t action;
            if(apply_to_backend && !night_transient) {
                action.kind = fields[index].kind;
                action.value = value;
                action.selection = -1;
                if(api_perform_action(action) != 0) {
                    if(!using_default) apply_failed = 1;
                    continue;
                }
                if(using_default)
                    (void)save_stock_image_parameter(
                        fields[index].profile_key, value);
            }
            found = 1;
            (void)calf_ui_sync_image_value(ui, fields[index].kind, value);
        }
    }
    if(apply_failed) return -1;
    return found ? 0 : 1;
}

static const char *success_message(calf_action_kind_t kind)
{
    if(kind == CALF_ACTION_SNAPSHOT) return "";
    if(kind == CALF_ACTION_CAPTURE_SEQUENCE_START) return "";
    if(kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL) return "CAPTURE STOPPED";
    if(kind == CALF_ACTION_RECORD_TOGGLE) return "";
    if(kind == CALF_ACTION_SET_CAMERA_MODE) return "MODE UPDATED";
    if(kind == CALF_ACTION_SET_CAPTURE_MODE) return "MODE UPDATED";
    if(kind == CALF_ACTION_SET_RESOLUTION) return "RESOLUTION UPDATED";
    if(kind == CALF_ACTION_SET_PHOTO_FORMAT) return "PHOTO FORMAT UPDATED";
    if(kind == CALF_ACTION_SET_DRIVE_MODE) return "DRIVE MODE UPDATED";
    if(kind >= CALF_ACTION_SET_ENCODING_CODEC &&
       kind <= CALF_ACTION_SET_RECORDING_COLOR_RANGE)
        return "ENCODER UPDATED";
    if(kind == CALF_ACTION_GALLERY_DELETE) return "DELETED";
    if(kind >= CALF_ACTION_GALLERY_ENTER &&
       kind <= CALF_ACTION_GALLERY_PLAY_TOGGLE) return "";
    if(kind == CALF_ACTION_SET_LCD_POWER) return "LCD UPDATED";
    if(kind == CALF_ACTION_SET_DISPLAY_OFF) return "DISPLAY TIMER UPDATED";
    if(kind == CALF_ACTION_SET_LANGUAGE) return "LANGUAGE UPDATED";
    if(kind == CALF_ACTION_SET_INDICATOR_LED) return "INDICATOR UPDATED";
    if(kind == CALF_ACTION_SET_SPEAKER_VOLUME) return "VOLUME UPDATED";
    if(kind == CALF_ACTION_SET_TIMEZONE) return "TIME ZONE UPDATED";
    if(kind == CALF_ACTION_SET_AUTO_TIME) return "AUTO TIME UPDATED";
    if(kind == CALF_ACTION_SET_DATETIME) return "CLOCK UPDATED";
    if(kind == CALF_ACTION_WIFI_SCAN) return "NETWORKS UPDATED";
    if(kind == CALF_ACTION_WIFI_CONNECT_SAVED ||
       kind == CALF_ACTION_WIFI_CONNECT_PASSWORD) return "WI-FI CONNECTED";
    if(kind == CALF_ACTION_SET_WIFI_ENABLED)
        return "WI-FI POWER UPDATED";
    if(kind == CALF_ACTION_SET_USB_ETHERNET)
        return "USB SETUP STARTED";
    if(kind == CALF_ACTION_FIRMWARE_CHECK) return "";
    if(kind == CALF_ACTION_FIRMWARE_INSTALL) return "REBOOTING TO UPDATE";
    if(kind == CALF_ACTION_LOAD_STOCK_UI) return "LOADING STOCK UI";
    return "APPLIED";
}

static void stop_handler(int signal_number)
{
    if(g_running) {
        if(signal_number == SIGTERM)
            supervisor_log_append("CALF_UI_SIGNAL term\n", 20u);
        else
            supervisor_log_append("CALF_UI_SIGNAL int\n", 19u);
    }
    g_supervisor_stop = 1;
    g_running = 0;
}

static void fatal_handler(int signal_number)
{
    static const char marker[] = "crash\n";
    int descriptor;
    if(signal_number == SIGILL)
        supervisor_log_append("CALF_UI_FATAL ill\n", 18u);
    else if(signal_number == SIGABRT)
        supervisor_log_append("CALF_UI_FATAL abrt\n", 19u);
    else if(signal_number == SIGFPE)
        supervisor_log_append("CALF_UI_FATAL fpe\n", 18u);
    else if(signal_number == SIGSEGV)
        supervisor_log_append("CALF_UI_FATAL segv\n", 19u);
    else if(signal_number == SIGBUS)
        supervisor_log_append("CALF_UI_FATAL bus\n", 18u);
    else
        supervisor_log_append("CALF_UI_FATAL unknown\n", 22u);
    /* Prevent the stock fallback selected below from inheriting a Night-only
     * AIQ frame-rate policy after a replacement-UI crash. */
    (void)unlink(NIGHT_PREVIEW_FPS_STATE);
    descriptor = open(STOCK_UI_SESSION_MARKER,
                      O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if(descriptor >= 0) {
        (void)write(descriptor, marker, sizeof(marker) - 1u);
        (void)close(descriptor);
    }
    _exit(128 + signal_number);
}

static int camera_main(void)
{
    display_t display;
    touch_t touch;
    keys_t keys;
    calf_ui_t ui;
    calf_backend_status_t status;
    audio_input_state_t audio_input_state;
    time_settings_t time_settings;
    camera_profiles_t camera_profiles;
    gallery_state_t gallery;
    capture_sequence_t capture_sequence;
    api_action_worker_t action_worker = {0};
    uint32_t rendered_revision = 0;
    unsigned poll_ticks = 0;
    unsigned notice_ticks = 0;
    uint64_t last_activity_ms;
    uint64_t next_time_floor_ms;
    uint64_t next_live_histogram_ms = 0;
    uint64_t next_gallery_histogram_ms = 0;
    uint64_t next_motion_ms = 0;
    uint64_t next_recording_timer_ms = 0;
    uint64_t gallery_navigation_ready_ms = 0;
    uint64_t gallery_volume_hide_ms = 0;
    uint64_t next_power_sample_ms;
    uint64_t next_wifi_status_ms;
    uint64_t next_indicator_led_enforce_ms = 0;
    uint64_t next_graph_recovery_ms = 0;
    calf_power_sample_t power_samples[POWER_SAMPLES_PER_HISTORY_POINT];
    size_t power_sample_count = 0;
    unsigned wifi_start_attempts = 0;
    int gallery_navigation_offset = 0;
    int touch_available;
    int keys_available;
    int camera_graph_available;
    int deep_idle = 0;
    char primary_profile[32];
    char checked_update_sha256[65];
    calf_capture_mode_t capture_mode = CALF_CAPTURE_PHOTO;
    int initial_speaker_volume;
    int indicator_led_selection;

    checked_update_sha256[0] = '\0';

    (void)signal(SIGINT, stop_handler);
    (void)signal(SIGTERM, stop_handler);
    (void)signal(SIGILL, fatal_handler);
    (void)signal(SIGABRT, fatal_handler);
    (void)signal(SIGFPE, fatal_handler);
    (void)signal(SIGSEGV, fatal_handler);
    (void)signal(SIGBUS, fatal_handler);
    supervisor_log_ui_start();
    calf_ui_init(&ui);
    calf_ui_set_wifi_enabled(&ui, load_stock_wifi_enabled());
    load_time_settings(&time_settings);
    if(calf_ui_sync_timezone(&ui, time_settings.timezone) != 0) {
        string_copy(time_settings.timezone, sizeof(time_settings.timezone), "UTC");
        (void)calf_ui_sync_timezone(&ui, time_settings.timezone);
    }
    calf_ui_sync_auto_time(&ui, time_settings.automatic);
    if(setenv("TZ", time_settings.timezone, 1) == 0) tzset();
    (void)restore_time_floor();
    if(time_settings.automatic)
        (void)system("/app/bin/ntpservice start");
    sync_local_datetime(&ui);
    (void)persist_time_floor();
    audio_input_state.valid = 0;
    gallery_init(&gallery);
    capture_sequence_init(&capture_sequence);
    if(load_audio_input_state(&audio_input_state) != 0)
        (void)load_stock_audio_input_state(&audio_input_state);
    initial_speaker_volume = load_speaker_volume();
    (void)load_camera_profiles(&camera_profiles);
    camera_profiles.mode = load_capture_mode(camera_profiles.mode);
    capture_mode = camera_profiles.mode;
    /* A previous crash or interrupted capture can leave a temporary timing
     * marker behind. Start every new UI process from the graph's baseline;
     * Night mode publishes its selected preview rate after graph startup. */
    string_copy(primary_profile, sizeof(primary_profile),
                capture_mode == CALF_CAPTURE_VIDEO
                    ? camera_profiles.video : camera_profiles.photo);
    calf_ui_set_capture_mode(&ui, capture_mode);
    (void)calf_ui_sync_resolution(&ui, capture_mode, primary_profile);
    calf_ui_sync_photo_format(&ui, load_raw_capture_enabled());
    calf_ui_set_drive_mode(&ui, load_drive_mode_selection());
    calf_ui_set_display_off(&ui, load_display_off_selection());
    calf_ui_set_language(&ui, load_language_selection());
    indicator_led_selection = load_indicator_led_selection();
    calf_ui_set_indicator_led(&ui, indicator_led_selection);
    if(indicator_led_selection == 1) indicator_led_force_off();
    last_activity_ms = monotonic_milliseconds();
    next_time_floor_ms = last_activity_ms +
                         (time_settings.automatic ? 60000u : 900000u);
    next_power_sample_ms = last_activity_ms;
    next_wifi_status_ms = last_activity_ms;
    status = ui.status;
    if(display_open(&display) != 0) return 2;
    display_present(&display, &ui);
    rendered_revision = ui.revision;
    camera_graph_available = api_start_initial_camera_graph(primary_profile) == 0;
    if(camera_graph_available) {
        char initial_exposure[16];
        char initial_iso[16];
        ui.lens_index = 1;
        ui.lens_known = 1;
        if(api_apply_encoder_profile(primary_profile) != 0)
            camera_graph_available = 0;
        if(capture_mode == CALF_CAPTURE_NIGHT) {
            load_mode_image_values(CALF_CAPTURE_NIGHT,
                                   initial_exposure, initial_iso);
            /* Restore WB, EV, sharpening and noise reduction too.  The
             * backend starts from profile defaults after a process restart;
             * applying only the Night exposure pair silently discarded the
             * persisted image-processing choices. */
            if(api_sync_image_state(&ui, 1) < 0 ||
               api_apply_actual_image_values(
                   &ui, initial_exposure, initial_iso) != 0 ||
               api_apply_night_preview(
                   &ui, initial_exposure, initial_iso) != 0)
                camera_graph_available = 0;
        }
        else
            (void)api_sync_image_state(&ui, 1);
        if(audio_input_state.valid)
            (void)api_apply_audio_input_state(&audio_input_state);
        (void)api_sync_encoder_state(&ui, primary_profile);
    }
    if(ui.wifi_enabled) {
        (void)wifi_start_service();
        wifi_start_attempts = 1;
    }
    {
        calf_action_t speaker_action;
        speaker_action.kind = CALF_ACTION_SET_SPEAKER_VOLUME;
        speaker_action.value = (const char *)0;
        speaker_action.selection = initial_speaker_volume / 10;
        if(api_perform_action(speaker_action) != 0)
            (void)calf_ui_sync_speaker_volume(&ui, -1);
        else
            (void)calf_ui_sync_speaker_volume(&ui,
                                              initial_speaker_volume);
    }
    touch_available = touch_open(&touch) == 0;
    keys_available = keys_open(&keys) > 0;
    (void)api_poll_status(&status);
    sync_recording_timer(&ui, &status, monotonic_milliseconds(),
                         &next_recording_timer_ms);
    if(ui.wifi_enabled) wifi_refresh_connection(&ui, "");
    next_wifi_status_ms = monotonic_milliseconds() +
        (ui.wifi_enabled && ui.wifi_ip_address[0] == '\0' &&
         wifi_start_attempts < WIFI_START_MAX_ATTEMPTS
             ? WIFI_START_RETRY_INTERVAL_MS : WIFI_STATUS_INTERVAL_MS);
    (void)api_sync_image_state(&ui, 0);
    (void)api_sync_audio_state(&ui, &audio_input_state);
    (void)api_sync_encoder_state(&ui, primary_profile);
    if(!camera_graph_available)
        calf_ui_notice(&ui, "CAMERA START FAILED", 1);
    else if(!touch_available) calf_ui_notice(&ui, "TOUCH NOT FOUND", 1);
    else if(!keys_available) calf_ui_notice(&ui, "KEYS NOT FOUND", 1);
    else calf_ui_notice(&ui, "", 0);

    while(g_running) {
        calf_action_t action;
        char resumed_action_value[API_ACTION_WORKER_VALUE_CAPACITY];
        int worker_result = -1;
        int worker_result_ready = 0;
        api_worker_job_t worker_job = API_WORKER_BACKEND_ACTION;
        int has_action = 0;
        int input_activity = 0;
        int physical_power_short = 0;
        int physical_power_long = 0;
        int automatic_display_off = 0;
        int scheduled_snapshot = 0;
        action.kind = CALF_ACTION_NONE;
        action.value = (const char *)0;
        action.selection = -1;
        resumed_action_value[0] = '\0';
        if(api_action_worker_take(
               &action_worker, &action, resumed_action_value,
               sizeof(resumed_action_value), &worker_result,
               &scheduled_snapshot, &worker_job)) {
            has_action = 1;
            worker_result_ready = 1;
        }
        /* A pending action is normally owned by the API worker. If neither a
         * live worker nor its just-completed continuation owns it, recover an
         * orphan created by two events in the same input batch. */
        if(ui.pending_action != CALF_ACTION_NONE &&
           !api_action_worker_busy(&action_worker) &&
           !worker_result_ready) {
            ui.pending_action = CALF_ACTION_NONE;
            ui.pending_selection = -1;
            calf_ui_notice(&ui, "READY - TRY AGAIN", 1);
            notice_ticks = 60;
        }
        if(rendered_revision != ui.revision) {
            display_present(&display, &ui);
            rendered_revision = ui.revision;
        }
        if(!has_action && (touch_available || keys_available)) {
            struct pollfd descriptors[MAX_EVENT_INPUTS + 1];
            int key_offset = touch_available ? 1 : 0;
            int descriptor_count = key_offset + keys.count;
            int index;
            if(touch_available) {
                descriptors[0].fd = touch.fd;
                descriptors[0].events = POLLIN;
                descriptors[0].revents = 0;
            }
            for(index = 0; index < keys.count; ++index) {
                descriptors[key_offset + index].fd = keys.descriptors[index];
                descriptors[key_offset + index].events = POLLIN;
                descriptors[key_offset + index].revents = 0;
            }
            if(poll(descriptors, (unsigned long)descriptor_count,
                    deep_idle ? 250 : 25) > 0) {
                if(touch_available && (descriptors[0].revents & POLLIN))
                    has_action = touch_read_action(
                        &touch, &ui, &action, !ui.lcd_on,
                        !deep_idle || capture_sequence.active,
                        &input_activity);
                for(index = 0; index < keys.count; ++index) {
                    if(descriptors[key_offset + index].revents & POLLIN) {
                        calf_action_t key_action;
                        int key_event = keys_read_action(
                            &keys, keys.descriptors[index], &ui, &key_action,
                            !ui.lcd_on,
                            !deep_idle || capture_sequence.active,
                            &physical_power_short, &physical_power_long,
                            &input_activity);
                        if(key_event) {
                            has_action = 1;
                            if(key_action.kind != CALF_ACTION_NONE)
                                action = key_action;
                        }
                    }
                }
            }
        }
        else if(!has_action) usleep(deep_idle ? 250000u : 25000u);

        if(input_activity)
            last_activity_ms = monotonic_milliseconds();

        /* Input readers consume actions while deep idle is active.  Promote
         * the first non-Power input to the same guarded wake path only after
         * it has been consumed, so a blind touch cannot activate a control.
         * A physical Power press waits for release and retains short/long
         * press handling. */
        if(deep_idle && input_activity && !has_action &&
           !physical_power_short &&
           !physical_power_long && !keys.power_pressed) {
            physical_power_short = 1;
            supervisor_log_event("DEEP_IDLE_WAKE", "source:input", 0);
        }

        if(!has_action && ui.lcd_on &&
           ui.display_off_seconds >= 0 &&
           ui.pending_action == CALF_ACTION_NONE) {
            uint64_t now_ms = monotonic_milliseconds();
            uint64_t delay_ms =
                (uint64_t)(unsigned)ui.display_off_seconds * 1000u;
            if(now_ms >= last_activity_ms &&
               now_ms - last_activity_ms >= delay_ms) {
                physical_power_short = 1;
                automatic_display_off = 1;
                last_activity_ms = now_ms;
            }
        }

        if(automatic_display_off)
            supervisor_log_event("DISPLAY_TIMER", "timeout", 0);
        else if(physical_power_short) {
            const char *detail = deep_idle ? "short:deep-idle"
                                 : !ui.lcd_on ? "short:lcd-off"
                                 : "short:lcd-on";
            supervisor_log_event("POWER_KEY", detail,
                                 (int)ui.pending_action);
        }

        if(physical_power_long &&
           api_action_worker_busy(&action_worker)) {
            physical_power_long = 0;
            calf_ui_notice(&ui, "ACTION IN PROGRESS", 1);
            notice_ticks = 60;
        }

        if(physical_power_long) {
            int wait_step;
            action.kind = CALF_ACTION_NONE;
            has_action = 0;
            calf_ui_notice(&ui, "POWERING OFF", 0);
            display_present(&display, &ui);
            rendered_revision = ui.revision;
            sync();
            if(api_power_off() == 0) {
                for(wait_step = 0; wait_step < 60 && g_running; ++wait_step)
                    usleep(250000u);
                if(g_running) {
                    calf_ui_notice(&ui, "POWER OFF TIMEOUT", 1);
                    notice_ticks = 60;
                }
            }
            else {
                calf_ui_notice(&ui, "POWER OFF FAILED", 1);
                notice_ticks = 60;
            }
        }

        if(physical_power_short &&
            ui.pending_action == CALF_ACTION_NONE) {
            if(capture_sequence.active) {
                if(automatic_display_off)
                    supervisor_log_event(
                        "DEEP_IDLE_CHECK", "capture-sequence", -1);
                if(api_set_lcd_power(&ui, !ui.lcd_on) != 0)
                    calf_ui_notice(&ui, "LCD POWER FAILED", 1);
                notice_ticks = 60;
            }
            else if(deep_idle) {
                int lcd_result;
                supervisor_log_event("DEEP_IDLE_WAKE", "stage:begin", 0);
                calf_ui_notice(&ui, "WAKING CAMERA", 0);
                display_present(&display, &ui);
                rendered_revision = ui.revision;
                /* Make wake visible before the slower stereo graph rebuild.
                 * A failed first request is retried after graph restoration. */
                lcd_result = api_set_lcd_power(&ui, 1);
                if(api_restore_primary_graph(
                       &ui, primary_profile, &audio_input_state) == 0) {
                    deep_idle = 0;
                    camera_graph_available = 1;
                    capture_sequence_set_sleeping(&capture_sequence, 0);
                    if(lcd_result != 0)
                        lcd_result = api_set_lcd_power(&ui, 1);
                    if(lcd_result == 0)
                        calf_ui_notice(&ui, "", 0);
                    else
                        calf_ui_notice(&ui, "LCD WAKE FAILED", 1);
                    supervisor_log_event(
                        "DEEP_IDLE_WAKE", "stage:complete", lcd_result);
                }
                else {
                    camera_graph_available = 0;
                    if(lcd_result != 0)
                        (void)api_set_lcd_power(&ui, 1);
                    calf_ui_notice(&ui, "CAMERA WAKE FAILED", 1);
                    supervisor_log_event(
                        "DEEP_IDLE_WAKE", "stage:graph-restore", -1);
                }
                poll_ticks = 40;
                notice_ticks = 60;
                display_present(&display, &ui);
                rendered_revision = ui.revision;
            }
            else if(!ui.lcd_on) {
                if(api_set_lcd_power(&ui, 1) != 0)
                    calf_ui_notice(&ui, "LCD WAKE FAILED", 1);
                notice_ticks = 60;
            }
            else {
                int safe = !gallery.active && camera_graph_available &&
                           api_deep_idle_is_safe(&status);
                if(gallery.active)
                    supervisor_log_event(
                        "DEEP_IDLE_CHECK", "gallery", -1);
                else if(!camera_graph_available)
                    supervisor_log_event(
                        "DEEP_IDLE_CHECK", "camera-graph", -1);
                calf_ui_set_status(&ui, &status);
                if(!safe) {
                    if(api_set_lcd_power(&ui, 0) != 0)
                        calf_ui_notice(&ui, "LCD SLEEP FAILED", 1);
                }
                else {
                    supervisor_log_event(
                        "DEEP_IDLE_SLEEP", "stage:begin", 0);
                    calf_ui_notice(&ui, "ENTERING DEEP IDLE", 0);
                    display_present(&display, &ui);
                    rendered_revision = ui.revision;
                    if(api_set_lcd_power(&ui, 0) == 0) {
                        if(api_stop_camera_graph() == 0) {
                            deep_idle = 1;
                            camera_graph_available = 0;
                            poll_ticks = 0;
                            supervisor_log_event(
                                "DEEP_IDLE_SLEEP", "stage:complete", 0);
                        }
                        else {
                            camera_graph_available =
                                api_restore_primary_graph(
                                    &ui, primary_profile,
                                    &audio_input_state) == 0;
                            deep_idle = !camera_graph_available;
                            (void)api_set_lcd_power(&ui, 1);
                            calf_ui_notice(
                                &ui, camera_graph_available
                                         ? "DEEP IDLE FAILED"
                                         : "CAMERA RESTORE FAILED",
                                1);
                            supervisor_log_event(
                                "DEEP_IDLE_SLEEP", "stage:graph-stop", -1);
                        }
                    }
                    else
                        calf_ui_notice(&ui, "LCD SLEEP FAILED", 1);
                }
                notice_ticks = 60;
            }
        }

        if(capture_sequence.active && capture_sequence.sleeping &&
           deep_idle && ui.pending_action == CALF_ACTION_NONE &&
           capture_sequence_should_wake(
               &capture_sequence, monotonic_milliseconds())) {
            if(api_restore_primary_graph(
                   &ui, primary_profile, &audio_input_state) == 0) {
                deep_idle = 0;
                camera_graph_available = 1;
                capture_sequence_set_sleeping(&capture_sequence, 0);
                poll_ticks = 40;
            }
            else {
                camera_graph_available = 0;
                capture_sequence_cancel(&capture_sequence);
                calf_ui_set_capture_sequence(&ui, 0, 0, 0, 0, 0);
                calf_ui_notice(&ui, "INTERVAL WAKE FAILED", 1);
                notice_ticks = 60;
            }
        }

        if(!has_action && ui.pending_action == CALF_ACTION_NONE &&
           capture_sequence_capture_due(
               &capture_sequence, monotonic_milliseconds())) {
            action.kind = CALF_ACTION_SNAPSHOT;
            action.value = (const char *)0;
            action.selection = -1;
            scheduled_snapshot = 1;
            has_action = 1;
        }

        if(capture_sequence.active)
            calf_ui_set_capture_sequence(
                &ui, 1, capture_sequence.interval,
                capture_sequence.sleeping,
                capture_sequence_remaining_seconds(
                    &capture_sequence, monotonic_milliseconds()),
                capture_sequence.shot_count);

        if(has_action && gallery.active && gallery.count > 0 &&
           (action.kind == CALF_ACTION_GALLERY_PREV ||
            action.kind == CALF_ACTION_GALLERY_NEXT)) {
            int direction = action.kind == CALF_ACTION_GALLERY_PREV ? -1 : 1;
            calf_ui_set_gallery_index(
                &ui,
                gallery_offset_index(
                    &gallery, gallery_navigation_offset + direction));
        }

        if(has_action &&
           action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE) {
            uint64_t now_ms = monotonic_milliseconds();
            if(now_ms < gallery.play_ready_ms) {
                calf_ui_complete_action(&ui, action, 1, "");
                action.kind = CALF_ACTION_NONE;
                has_action = 0;
            }
            else
                gallery.play_ready_ms =
                    now_ms + GALLERY_PLAY_DEBOUNCE_MS;
        }

        if(has_action &&
           (action.kind == CALF_ACTION_GALLERY_PREV ||
            action.kind == CALF_ACTION_GALLERY_NEXT) &&
           (monotonic_milliseconds() < gallery_navigation_ready_ms ||
            gallery_navigation_offset != 0)) {
            gallery_navigation_offset +=
                action.kind == CALF_ACTION_GALLERY_PREV ? -1 : 1;
            if(gallery.count > 0)
                gallery_navigation_offset %= gallery.count;
            calf_ui_complete_action(&ui, action, 1, "");
            action.kind = CALF_ACTION_NONE;
            has_action = 0;
        }

        if(!has_action && gallery.active &&
           gallery_navigation_offset != 0 &&
           monotonic_milliseconds() >= gallery_navigation_ready_ms &&
           ui.pending_action == CALF_ACTION_NONE) {
            int offset = gallery_navigation_offset;
            int result;
            gallery_navigation_offset = 0;
            result = gallery_move(&gallery, offset);
            gallery_clear_preview(&ui, &gallery);
            gallery_sync_ui(&ui, &gallery);
            if(result == 0) {
                gallery_navigation_ready_ms =
                    monotonic_milliseconds() +
                    GALLERY_NAVIGATION_DEBOUNCE_MS;
                api_poll_gallery_info(&ui, &gallery);
            }
            else {
                calf_ui_notice(&ui, "GALLERY ERROR", 1);
                notice_ticks = 60;
            }
        }

        if(has_action &&
           (action.kind == CALF_ACTION_GALLERY_ENTER ||
            action.kind == CALF_ACTION_GALLERY_EXIT ||
            action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE ||
            action.kind == CALF_ACTION_GALLERY_DELETE))
            gallery_navigation_offset = 0;

        if(has_action && action.kind != CALF_ACTION_NONE) {
            int result;
            int profile_saved = 1;
            int primary_restore_failed = 0;
            int completion_handled = 0;
            int gallery_volume_action =
                action.kind == CALF_ACTION_SET_SPEAKER_VOLUME &&
                ui.screen == CALF_SCREEN_GALLERY;
            const char *failure_message = "BACKEND ERROR";
            const char *gallery_failure_message = "GALLERY ERROR";
            int gallery_was_suspended = gallery.graph_suspended;
            display_present(&display, &ui);
            rendered_revision = ui.revision;
            if(action.kind == CALF_ACTION_SET_CAMERA_MODE &&
               starts_with(action.value, "PRIMARY"))
                action.value = primary_profile;
            if(scheduled_snapshot &&
               (!camera_graph_available ||
                capture_mode == CALF_CAPTURE_VIDEO)) {
                primary_restore_failed = 1;
                failure_message = "CAPTURE SEQUENCE STOPPED";
            }
            else if(calf_ui_action_requires_primary(&ui, action)) {
                calf_action_t primary_action;
                primary_action.kind = CALF_ACTION_SET_CAMERA_MODE;
                primary_action.value = primary_profile;
                primary_action.selection = 1;
                calf_ui_notice(&ui, "RESTORING STEREO", 0);
                display_present(&display, &ui);
                rendered_revision = ui.revision;
                if(api_perform_action(primary_action) != 0)
                    primary_restore_failed = 1;
                else {
                    ui.lens_index = 1;
                    ui.lens_known = 1;
                    if(api_sync_image_state(&ui, 1) < 0 ||
                       (audio_input_state.valid &&
                        api_apply_audio_input_state(
                            &audio_input_state) != 0))
                        primary_restore_failed = 1;
                }
                if(!primary_restore_failed) {
                    calf_ui_notice(&ui, "CAPTURING", 0);
                    display_present(&display, &ui);
                    rendered_revision = ui.revision;
                    usleep(1000000);
                }
            }
            if(primary_restore_failed)
                result = -1;
            else if(action.kind == CALF_ACTION_SNAPSHOT) {
                if(!worker_result_ready) {
                    supervisor_log_event("SNAPSHOT", "stage:begin", 0);
                    if(!scheduled_snapshot) {
                        calf_ui_notice(&ui, "CAPTURING - PLEASE WAIT", 0);
                        display_present(&display, &ui);
                        rendered_revision = ui.revision;
                    }
                    else if(ui.pending_action == CALF_ACTION_NONE) {
                        ui.pending_action = CALF_ACTION_SNAPSHOT;
                        ui.pending_selection = -1;
                        ++ui.revision;
                    }
                    if(api_action_worker_submit(
                           &action_worker, action, scheduled_snapshot,
                           API_WORKER_BACKEND_ACTION) == 0)
                        goto action_deferred;
                    result = -1;
                }
                else result = worker_result;
                supervisor_log_event("SNAPSHOT", "stage:http-complete",
                                     result);
                if(capture_mode == CALF_CAPTURE_NIGHT)
                    if(api_apply_night_preview(
                           &ui, (const char *)0, (const char *)0) != 0)
                        supervisor_log_event(
                            "SNAPSHOT", "stage:preview-restore", -1);
                if(scheduled_snapshot) {
                    capture_sequence_complete_capture(
                        &capture_sequence, result == 0,
                        monotonic_milliseconds());
                    calf_ui_set_capture_sequence(
                        &ui, capture_sequence.active,
                        capture_sequence.interval,
                        capture_sequence.sleeping,
                        capture_sequence_remaining_seconds(
                            &capture_sequence, monotonic_milliseconds()),
                        capture_sequence.shot_count);
                }
            }
            else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_START) {
                if(capture_mode == CALF_CAPTURE_VIDEO ||
                   !camera_graph_available || !status.online ||
                   status.recording || status.streaming != 0 ||
                   status.playback != 0 || !ui.drive_mode_known ||
                   ui.drive_mode_index == 0) {
                    result = -1;
                    failure_message = "CAPTURE START BLOCKED";
                }
                else if(calf_drive_mode_is_burst(
                            (size_t)ui.drive_mode_index) &&
                        (!ui.photo_format_known ||
                         ui.photo_format_index != 0)) {
                    result = -1;
                    failure_message = "BURST REQUIRES JPEG";
                }
                else {
                    result = capture_sequence_start(
                        &capture_sequence, ui.drive_mode_index,
                        monotonic_milliseconds());
                    if(result == 0)
                        calf_ui_set_capture_sequence(
                            &ui, 1, capture_sequence.interval, 0,
                            capture_sequence_remaining_seconds(
                                &capture_sequence,
                                monotonic_milliseconds()),
                            0);
                }
            }
            else if(action.kind == CALF_ACTION_CAPTURE_SEQUENCE_CANCEL) {
                capture_sequence_cancel(&capture_sequence);
                calf_ui_set_capture_sequence(&ui, 0, 0, 0, 0, 0);
                if(!deep_idle)
                    result = 0;
                else if(api_restore_primary_graph(
                            &ui, primary_profile,
                            &audio_input_state) == 0) {
                    deep_idle = 0;
                    camera_graph_available = 1;
                    poll_ticks = 40;
                    result = 0;
                }
                else {
                    camera_graph_available = 0;
                    failure_message = "CAMERA RESTORE FAILED";
                    result = -1;
                }
            }
            else if(action.kind == CALF_ACTION_GALLERY_ENTER) {
                supervisor_log_event("GALLERY_ENTER", "stage:begin", 0);
                if(status.recording) {
                    gallery_failure_message = "STOP RECORDING FIRST";
                    result = -1;
                }
                else if(!status.online) {
                    gallery_failure_message = "STATUS UNKNOWN";
                    result = -1;
                }
                else if(status.streaming != 0) {
                    gallery_failure_message = status.streaming > 0
                                                  ? "STOP LIVE FIRST"
                                                  : "LIVE STATUS UNKNOWN";
                    result = -1;
                }
                else if(status.playback != 0) {
                    gallery_failure_message = status.playback > 0
                                                  ? "STOP PLAYBACK FIRST"
                                                  : "PLAY STATUS UNKNOWN";
                    result = -1;
                }
                else {
                    supervisor_log_event(
                        "GALLERY_ENTER", "stage:playback-start", 0);
                    result = gallery_enter_backend(&gallery);
                    supervisor_log_event(
                        "GALLERY_ENTER", "stage:playback-complete", result);
                }
                if(result != 0 && gallery.graph_suspended) {
                    (void)gallery_close_backend(&gallery);
                    gallery.graph_suspended = 0;
                }
                gallery_clear_preview(&ui, &gallery);
                gallery_sync_ui(&ui, &gallery);
                if(result == 0) {
                    gallery_navigation_ready_ms =
                        monotonic_milliseconds() +
                        GALLERY_NAVIGATION_DEBOUNCE_MS;
                    api_poll_gallery_info(&ui, &gallery);
                    supervisor_log_event(
                        "GALLERY_ENTER", "stage:complete", 0);
                }
                else
                    supervisor_log_event(
                        "GALLERY_ENTER", "stage:failed", result);
            }
            else if(action.kind == CALF_ACTION_GALLERY_EXIT) {
                supervisor_log_event("GALLERY_EXIT", "stage:begin", 0);
                gallery_clear_preview(&ui, &gallery);
                result = gallery_close_backend(&gallery);
                if(gallery_was_suspended) {
                    if(result == 0) {
                        gallery.graph_suspended = 0;
                        ui.lens_index = 1;
                        ui.lens_known = 1;
                        supervisor_log_event(
                            "GALLERY_EXIT", "stage:graph-restored", 0);
                        /* Playback stop rebuilds the selected camera graph
                         * and correctly reapplies the persisted capture
                         * exposure/ISO. Restore Night mode's lower-rate
                         * transient preview after the graph is ready. */
                        if(capture_mode == CALF_CAPTURE_NIGHT &&
                           api_apply_night_preview(
                               &ui, (const char *)0,
                               (const char *)0) != 0) {
                            result = -1;
                            failure_message = "NIGHT PREVIEW FAILED";
                            supervisor_log_event(
                                "GALLERY_EXIT", "stage:night-preview", -1);
                        }
                        else
                            supervisor_log_event(
                                "GALLERY_EXIT", "stage:complete", 0);
                    }
                    else
                        supervisor_log_event(
                            "GALLERY_EXIT", "stage:graph-restore", -1);
                }
                else
                    supervisor_log_event(
                        "GALLERY_EXIT", "stage:complete-empty", result);
            }
            else if(action.kind == CALF_ACTION_GALLERY_PREV ||
                    action.kind == CALF_ACTION_GALLERY_NEXT) {
                result = gallery_move(
                    &gallery, action.kind == CALF_ACTION_GALLERY_PREV ? -1 : 1);
                gallery_clear_preview(&ui, &gallery);
                gallery_sync_ui(&ui, &gallery);
                if(result == 0) {
                    gallery_navigation_ready_ms =
                        monotonic_milliseconds() +
                        GALLERY_NAVIGATION_DEBOUNCE_MS;
                    api_poll_gallery_info(&ui, &gallery);
                }
            }
            else if(action.kind == CALF_ACTION_GALLERY_PLAY_TOGGLE) {
                result = gallery_toggle_playback(&gallery);
            }
            else if(action.kind == CALF_ACTION_GALLERY_DELETE) {
                result = gallery_delete_current(&gallery);
                gallery_clear_preview(&ui, &gallery);
                gallery_sync_ui(&ui, &gallery);
            }
            else if(action.kind == CALF_ACTION_SET_CAPTURE_MODE) {
                calf_capture_mode_t selected_mode =
                    (calf_capture_mode_t)action.selection;
                const char *selected_profile =
                                               selected_mode == CALF_CAPTURE_VIDEO
                                                   ? camera_profiles.video
                                                   : camera_profiles.photo;
                char departing_exposure[16];
                char departing_iso[16];
                char selected_exposure[16];
                char selected_iso[16];
                int changes_night_state =
                    capture_mode == CALF_CAPTURE_NIGHT ||
                    selected_mode == CALF_CAPTURE_NIGHT;
                int entering_night =
                    capture_mode != CALF_CAPTURE_NIGHT &&
                    selected_mode == CALF_CAPTURE_NIGHT;
                int graph_changed = 0;
                departing_exposure[0] = '\0';
                departing_iso[0] = '\0';
                if(ui.exposure_known)
                    string_copy(departing_exposure,
                                sizeof(departing_exposure),
                                calf_exposure_value(
                                    (size_t)ui.exposure_index));
                if(ui.iso_known)
                    string_copy(departing_iso, sizeof(departing_iso),
                                calf_iso_value((size_t)ui.iso_index));
                if(changes_night_state && departing_exposure[0] != '\0' &&
                   departing_iso[0] != '\0')
                    (void)save_mode_image_values(
                        capture_mode, departing_exposure, departing_iso);
                if(changes_night_state)
                    load_mode_image_values(selected_mode,
                                           selected_exposure, selected_iso);
                if(!status.online || status.recording ||
                   status.streaming != 0 || status.playback != 0) {
                    result = -1;
                    failure_message = "MODE STATUS BLOCKED";
                }
                else {
                    if(capture_mode == CALF_CAPTURE_NIGHT &&
                       selected_mode != CALF_CAPTURE_NIGHT) {
                        /* Night and Photo share VR180_PIC, so graph start is
                         * intentionally a no-op between them. Restore the
                         * actual sensor/AIQ baseline before that no-op rather
                         * than only deleting the policy marker and leaving
                         * the live pair at 4/8/15 fps. */
                        supervisor_log_event(
                            "CAPTURE_MODE", "stage:night-timing-restore", 0);
                        api_restore_standard_preview_timing();
                    }
                    /* Photo and Night share VR180_PIC, but the no-op graph
                     * start can retain an AIQ context that accepts the 4 fps
                     * range write without publishing it on a sensor frame.
                     * Entering Night therefore gets one explicit graph
                     * restart before image state and the atomic preview
                     * transaction are reapplied. The existing failure path
                     * restores the departing graph and settings. */
                    if(entering_night) {
                        result = api_stop_camera_graph();
                        if(result == 0) {
                            graph_changed = 1;
                            result = api_start_initial_camera_graph(
                                         selected_profile);
                        }
                        else supervisor_log_event(
                            "CAPTURE_MODE", "stage:graph-stop", -1);
                    }
                    else {
                        result = api_start_initial_camera_graph(
                                     selected_profile);
                        graph_changed = result == 0;
                    }
                    if(result != 0) {
                        failure_message = "MODE GRAPH FAILED";
                        supervisor_log_event(
                            "CAPTURE_MODE", "stage:graph", -1);
                    }
                }
                if(result == 0) {
                    result = api_apply_encoder_profile(selected_profile);
                    if(result != 0) {
                        failure_message = "MODE ENCODER FAILED";
                        supervisor_log_event(
                            "CAPTURE_MODE", "stage:encoder", -1);
                    }
                }
                if(result == 0) {
                    result = changes_night_state
                                 ? api_apply_actual_image_values(
                                       &ui, selected_exposure, selected_iso)
                                 : api_sync_image_state(&ui, 1) < 0 ? -1 : 0;
                    if(result != 0) {
                        failure_message = "MODE IMAGE FAILED";
                        supervisor_log_event(
                            "CAPTURE_MODE", "stage:image", -1);
                    }
                }
                if(result == 0 && selected_mode == CALF_CAPTURE_NIGHT &&
                   api_apply_night_preview(
                       &ui, selected_exposure, selected_iso) != 0) {
                    result = -1;
                    failure_message = "NIGHT PREVIEW FAILED";
                    supervisor_log_event(
                        "CAPTURE_MODE", "stage:night-preview", -1);
                }
                if(result == 0 && audio_input_state.valid &&
                   api_apply_audio_input_state(&audio_input_state) != 0) {
                    result = -1;
                    failure_message = "MODE AUDIO FAILED";
                    supervisor_log_event(
                        "CAPTURE_MODE", "stage:audio", -1);
                }
                if(result != 0 && graph_changed) {
                    (void)api_start_initial_camera_graph(primary_profile);
                    (void)api_apply_encoder_profile(primary_profile);
                    if(changes_night_state &&
                       departing_exposure[0] != '\0' &&
                       departing_iso[0] != '\0')
                        (void)api_apply_actual_image_values(
                            &ui, departing_exposure, departing_iso);
                    else
                        (void)api_sync_image_state(&ui, 1);
                    if(capture_mode == CALF_CAPTURE_NIGHT)
                        (void)api_apply_night_preview(
                            &ui, departing_exposure, departing_iso);
                    if(audio_input_state.valid)
                        (void)api_apply_audio_input_state(&audio_input_state);
                }
                else if(result != 0 &&
                        capture_mode == CALF_CAPTURE_NIGHT)
                    (void)api_apply_night_preview(
                        &ui, departing_exposure, departing_iso);
                if(result == 0) {
                    string_copy(primary_profile, sizeof(primary_profile),
                                selected_profile);
                    camera_profiles.mode = selected_mode;
                    capture_mode = camera_profiles.mode;
                    (void)api_sync_encoder_state(&ui, selected_profile);
                    ui.lens_index = 1;
                    ui.lens_known = 1;
                }
            }
            else if(action.kind == CALF_ACTION_SET_RESOLUTION) {
                int graph_changed = 0;
                if(!status.online || status.recording ||
                   status.streaming != 0 || status.playback != 0)
                    result = -1;
                else {
                    result = api_start_initial_camera_graph(action.value);
                    graph_changed = result == 0;
                }
                if(result == 0 &&
                   api_apply_encoder_profile(action.value) != 0)
                    result = -1;
                if(result == 0 && api_sync_image_state(&ui, 1) < 0)
                    result = -1;
                if(result == 0 && capture_mode == CALF_CAPTURE_NIGHT &&
                   api_apply_night_preview(
                       &ui, (const char *)0, (const char *)0) != 0)
                    result = -1;
                if(result == 0 && audio_input_state.valid &&
                   api_apply_audio_input_state(&audio_input_state) != 0)
                    result = -1;
                if(result != 0 && graph_changed) {
                    (void)api_start_initial_camera_graph(primary_profile);
                    (void)api_apply_encoder_profile(primary_profile);
                    (void)api_sync_image_state(&ui, 1);
                    if(audio_input_state.valid)
                        (void)api_apply_audio_input_state(&audio_input_state);
                }
                if(result == 0) {
                    string_copy(primary_profile, sizeof(primary_profile),
                                action.value);
                    if(capture_mode == CALF_CAPTURE_VIDEO)
                        string_copy(camera_profiles.video,
                                    sizeof(camera_profiles.video),
                                    action.value);
                    else
                        string_copy(camera_profiles.photo,
                                    sizeof(camera_profiles.photo),
                                    action.value);
                    profile_saved = save_stock_resolution(
                                        capture_mode, action.value) == 0;
                    (void)api_sync_encoder_state(&ui, action.value);
                    ui.lens_index = 1;
                    ui.lens_known = 1;
                }
            }
            else if(action.kind >= CALF_ACTION_SET_ENCODING_CODEC &&
                    action.kind <= CALF_ACTION_SET_RECORDING_COLOR_RANGE)
                result = perform_encoder_action(
                    action, primary_profile, &profile_saved);
            else if(action.kind == CALF_ACTION_SET_DISPLAY_OFF)
                result = save_display_off_selection(action.selection);
            else if(action.kind == CALF_ACTION_SET_LANGUAGE)
                result = save_language_selection(action.selection);
            else if(action.kind == CALF_ACTION_SET_INDICATOR_LED) {
                result = save_indicator_led_selection(action.selection);
                if(result == 0) {
                    indicator_led_selection = action.selection;
                    next_indicator_led_enforce_ms = 0;
                    if(indicator_led_selection == 1)
                        indicator_led_force_off();
                    else if(status.recording)
                        indicator_led_restore_recording_blink();
                }
            }
            else if(action.kind == CALF_ACTION_SET_TIMEZONE ||
                    action.kind == CALF_ACTION_SET_AUTO_TIME ||
                    action.kind == CALF_ACTION_SET_DATETIME)
                result = perform_time_action(action, &time_settings);
            else if(action.kind == CALF_ACTION_WIFI_SCAN) {
                if(!worker_result_ready) {
                    if(api_action_worker_submit(
                           &action_worker, action, scheduled_snapshot,
                           API_WORKER_WIFI_SCAN) == 0)
                        goto action_deferred;
                    result = -1;
                }
                else {
                    result = worker_result;
                    if(result == 0 && worker_job == API_WORKER_WIFI_SCAN)
                        calf_ui_set_wifi_networks(
                            &ui, action_worker.wifi_scan.networks,
                            action_worker.wifi_scan.count,
                            wifi_ssid_safe(
                                action_worker.wifi_scan.current_ssid)
                                ? action_worker.wifi_scan.current_ssid : "",
                            action_worker.wifi_scan.ip_address);
                }
            }
            else if(action.kind == CALF_ACTION_WIFI_CONNECT_SAVED) {
                result = string_equal(action.value, ui.wifi_current_ssid)
                             ? 0 : wifi_connect_saved(action.value);
                if(result != 0) {
                    int restore_result = -1;
                    if(result != WIFI_CONNECT_NO_PROFILE &&
                       ui.wifi_current_ssid[0] != '\0' &&
                       !string_equal(action.value, ui.wifi_current_ssid))
                        restore_result = wifi_connect_saved(
                            ui.wifi_current_ssid);
                    calf_ui_wifi_require_password(&ui, action.selection);
                    if(restore_result == 0)
                        calf_ui_notice(&ui,
                            "SAVED LOGIN FAILED - WI-FI RESTORED", 1);
                    completion_handled = 1;
                }
                else {
                    usleep(500000u);
                    wifi_refresh_connection(&ui, action.value);
                }
            }
            else if(action.kind == CALF_ACTION_WIFI_CONNECT_PASSWORD) {
                result = wifi_connect_password(action.value,
                                               ui.wifi_password);
                if(result == 0) {
                    secure_zero(ui.wifi_password,
                                sizeof(ui.wifi_password));
                    usleep(500000u);
                    wifi_refresh_connection(&ui, action.value);
                }
                else {
                    int restore_result = -1;
                    if(ui.wifi_current_ssid[0] != '\0' &&
                       !string_equal(action.value, ui.wifi_current_ssid))
                        restore_result = wifi_connect_saved(
                            ui.wifi_current_ssid);
                    if(restore_result == 0) {
                        usleep(500000u);
                        wifi_refresh_connection(&ui,
                                                ui.wifi_current_ssid);
                    }
                    failure_message = result == 2
                        ? (restore_result == 0
                               ? "AUTH FAILED - WI-FI RESTORED"
                               : "AUTH FAILED - CHECK PASSWORD")
                        : (restore_result == 0
                               ? "CONNECT FAILED - WI-FI RESTORED"
                               : "WI-FI CONNECTION FAILED");
                }
            }
            else if(action.kind == CALF_ACTION_SET_WIFI_ENABLED) {
                result = wifi_set_enabled(action.selection != 0);
                if(result == 0) {
                    wifi_start_attempts = action.selection != 0 ? 1u : 0u;
                    next_wifi_status_ms = monotonic_milliseconds() +
                        (action.selection != 0
                             ? WIFI_START_RETRY_INTERVAL_MS
                             : WIFI_STATUS_INTERVAL_MS);
                }
            }
            else if(action.kind == CALF_ACTION_FIRMWARE_CHECK) {
                int update_size_mb = 0;
                char update_sha256[65];
                checked_update_sha256[0] = '\0';
                failure_message = firmware_preflight(
                    &ui, &status, &update_size_mb, update_sha256);
                result = failure_message == (const char *)0 ? 0 : -1;
                if(result == 0) {
                    string_copy(checked_update_sha256,
                                sizeof(checked_update_sha256),
                                update_sha256);
                    calf_ui_set_update_ready(&ui, update_size_mb);
                }
            }
            else if(action.kind == CALF_ACTION_FIRMWARE_INSTALL) {
                int update_size_mb = 0;
                char update_sha256[65];
                failure_message = firmware_preflight(
                    &ui, &status, &update_size_mb, update_sha256);
                if(failure_message == (const char *)0 &&
                   (!ui.update_ready || update_size_mb != ui.update_size_mb ||
                    !string_equal(update_sha256,
                                  checked_update_sha256)))
                    failure_message = "UPDATE FILE CHANGED; CHECK AGAIN";
                if(failure_message == (const char *)0) {
                    sync();
                    result = api_perform_action(action);
                }
                else result = -1;
            }
            else if(action.kind == CALF_ACTION_LOAD_STOCK_UI) {
                result = request_stock_ui_session();
                if(result == 0) {
                    calf_ui_complete_action(&ui, action, 1,
                                            "LOADING STOCK UI");
                    display_present(&display, &ui);
                    rendered_revision = ui.revision;
                    usleep(250000u);
                    g_running = 0;
                    completion_handled = 1;
                }
            }
            else if(action.kind == CALF_ACTION_SET_PHOTO_FORMAT)
                result = save_raw_capture_enabled(action.selection != 0);
            else if(action.kind == CALF_ACTION_SET_DRIVE_MODE)
                result = save_drive_mode_selection(action.selection);
            else if(night_image_action_is_transient(
                        capture_mode, action.kind)) {
                /* Applying the real exposure first is not valid across a
                 * direct 4/8/15-fps transition (for example 1/8 while the
                 * sensors are still at 4 fps).  The transient Night preview
                 * transaction below owns the live update; profile persistence
                 * records the real capture pair after it succeeds. */
                result = 0;
            }
            else {
                if(action.kind == CALF_ACTION_SET_LCD_POWER)
                    supervisor_log_event(
                        "LCD_POWER", action.value, 0);
                if(!worker_result_ready) {
                    if(api_action_worker_submit(
                           &action_worker, action, scheduled_snapshot,
                           API_WORKER_BACKEND_ACTION) == 0)
                        goto action_deferred;
                    result = -1;
                }
                else result = worker_result;
                if(action.kind == CALF_ACTION_SET_LCD_POWER)
                    supervisor_log_event(
                        "LCD_POWER", action.value, result);
            }
            if(result == 0 && action.kind == CALF_ACTION_SET_LCD_POWER &&
               action.selection != 0 &&
               !gallery.active && camera_graph_available &&
               capture_mode == CALF_CAPTURE_NIGHT &&
               api_apply_night_preview(
                   &ui, (const char *)0, (const char *)0) != 0) {
                result = -1;
                failure_message = "NIGHT PREVIEW FAILED";
            }
            if(result == 0 && capture_mode == CALF_CAPTURE_NIGHT &&
               (action.kind == CALF_ACTION_SET_EXPOSURE ||
                action.kind == CALF_ACTION_SET_ISO)) {
                const char *preview_exposure =
                    action.kind == CALF_ACTION_SET_EXPOSURE
                        ? action.value
                        : ui.exposure_known
                              ? calf_exposure_value(
                                    (size_t)ui.exposure_index)
                              : "0.5";
                const char *preview_iso =
                    action.kind == CALF_ACTION_SET_ISO
                        ? action.value
                        : ui.iso_known
                              ? calf_iso_value((size_t)ui.iso_index)
                              : "iso400";
                if(api_apply_night_preview(
                       &ui, preview_exposure, preview_iso) != 0) {
                    const char *restore_exposure =
                        ui.exposure_known
                            ? calf_exposure_value(
                                  (size_t)ui.exposure_index)
                            : "0.5";
                    const char *restore_iso =
                        ui.iso_known
                            ? calf_iso_value((size_t)ui.iso_index)
                            : "iso400";
                    (void)api_apply_night_preview(
                        &ui, restore_exposure, restore_iso);
                    result = -1;
                    failure_message = "NIGHT PREVIEW FAILED";
                }
            }
            if(scheduled_snapshot && primary_restore_failed) {
                capture_sequence_cancel(&capture_sequence);
                calf_ui_set_capture_sequence(&ui, 0, 0, 0, 0, 0);
            }
            if(result == 0 && action.kind == CALF_ACTION_SET_CAMERA_MODE &&
               api_sync_image_state(&ui, 1) < 0)
                result = -1;
            if(result == 0 && action.kind == CALF_ACTION_SET_CAMERA_MODE &&
               capture_mode == CALF_CAPTURE_NIGHT &&
               api_apply_night_preview(
                   &ui, (const char *)0, (const char *)0) != 0) {
                result = -1;
                failure_message = "NIGHT PREVIEW FAILED";
            }
            if(result == 0 && action.kind == CALF_ACTION_SET_CAMERA_MODE &&
               audio_input_state.valid &&
               api_apply_audio_input_state(&audio_input_state) != 0)
                result = -1;
            if(result == 0 && image_profile_key(action.kind) != (const char *)0) {
                profile_saved = save_stock_image_parameter(
                                    image_profile_key(action.kind),
                                    action.value) == 0;
                if(profile_saved && capture_mode == CALF_CAPTURE_NIGHT &&
                   (action.kind == CALF_ACTION_SET_EXPOSURE ||
                    action.kind == CALF_ACTION_SET_ISO)) {
                    const char *stored_exposure =
                        action.kind == CALF_ACTION_SET_EXPOSURE
                            ? action.value
                            : calf_exposure_value(
                                  (size_t)ui.exposure_index);
                    const char *stored_iso =
                        action.kind == CALF_ACTION_SET_ISO
                            ? action.value
                            : calf_iso_value((size_t)ui.iso_index);
                    profile_saved = save_mode_image_values(
                                        CALF_CAPTURE_NIGHT,
                                        stored_exposure, stored_iso) == 0;
                }
            }
            else if(result == 0 && action.kind == CALF_ACTION_SET_CAPTURE_MODE)
                profile_saved = save_stock_top_level_value(
                                    "cam_mode", action.selection == CALF_CAPTURE_VIDEO
                                                    ? "recording" : "photo") == 0 &&
                                save_capture_mode(
                                    (calf_capture_mode_t)action.selection) == 0;
            else if(result == 0 &&
                    action.kind == CALF_ACTION_SET_SPEAKER_VOLUME)
                profile_saved = save_speaker_volume(
                                    action.selection * 10) == 0;
            if(completion_handled) {
                /* Saved-profile failure deliberately opens password entry. */
            }
            else if(result == 0)
                calf_ui_complete_action(&ui, action, 1,
                                        (gallery_volume_action ||
                                         action.kind ==
                                             CALF_ACTION_GALLERY_PLAY_TOGGLE)
                                            ? "" : success_message(action.kind));
            else if(primary_restore_failed)
                calf_ui_complete_action(&ui, action, 0,
                                        "STEREO RESTORE FAILED");
            else if(action.kind == CALF_ACTION_SET_TIMEZONE ||
                    action.kind == CALF_ACTION_SET_AUTO_TIME ||
                    action.kind == CALF_ACTION_SET_DATETIME)
                calf_ui_complete_action(&ui, action, 0,
                                        "TIME UPDATE FAILED");
            else if(action.kind >= CALF_ACTION_GALLERY_ENTER &&
                    action.kind <= CALF_ACTION_GALLERY_DELETE)
                calf_ui_complete_action(&ui, action, 0,
                                        gallery_failure_message);
            else if(action.kind == CALF_ACTION_SET_WIFI_ENABLED)
                calf_ui_complete_action(&ui, action, 0,
                                        "WI-FI POWER FAILED");
            else if(failure_message != (const char *)0)
                calf_ui_complete_action(&ui, action, 0, failure_message);
            else
                calf_ui_complete_action(&ui, action, 0, "BACKEND ERROR");
            if(result == 0 && action.kind == CALF_ACTION_SET_CAPTURE_MODE)
                (void)calf_ui_sync_resolution(
                    &ui, capture_mode, primary_profile);
            if(result == 0 && !profile_saved)
                calf_ui_notice(&ui, "PROFILE SAVE FAILED", 1);
            if(result == 0 && gallery_volume_action) {
                calf_ui_set_gallery_volume_visible(&ui, 1);
                gallery_volume_hide_ms = monotonic_milliseconds() +
                                         GALLERY_VOLUME_OVERLAY_MS;
            }
            notice_ticks = 60;
            if(action.kind != CALF_ACTION_SNAPSHOT)
                poll_ticks = 40;
            if(result == 0 &&
               (action.kind == CALF_ACTION_SET_TIMEZONE ||
                action.kind == CALF_ACTION_SET_DATETIME))
                sync_local_datetime(&ui);
            if(result == 0 && action.kind == CALF_ACTION_SET_AUTO_TIME &&
               time_settings.automatic)
                next_time_floor_ms = monotonic_milliseconds() + 60000u;
            if(scheduled_snapshot && result != 0) {
                calf_ui_notice(&ui, "CAPTURE FAILED - SEQUENCE STOPPED", 1);
                notice_ticks = 60;
            }
            else if(scheduled_snapshot && result == 0 &&
                    capture_sequence_should_sleep(
                        &capture_sequence, monotonic_milliseconds()) &&
                    camera_graph_available &&
                    api_deep_idle_is_safe(&status) &&
                    capture_sequence_should_sleep(
                        &capture_sequence, monotonic_milliseconds())) {
                calf_ui_set_status(&ui, &status);
                if(api_stop_camera_graph() == 0) {
                    deep_idle = 1;
                    camera_graph_available = 0;
                    capture_sequence_set_sleeping(&capture_sequence, 1);
                    calf_ui_set_capture_sequence(
                        &ui, 1, 1, 1,
                        capture_sequence_remaining_seconds(
                            &capture_sequence,
                            monotonic_milliseconds()),
                        capture_sequence.shot_count);
                    poll_ticks = 0;
                }
                else {
                    camera_graph_available =
                        api_restore_primary_graph(
                            &ui, primary_profile,
                            &audio_input_state) == 0;
                    deep_idle = !camera_graph_available;
                    capture_sequence_set_sleeping(
                        &capture_sequence, deep_idle);
                    if(!camera_graph_available) {
                        capture_sequence_cancel(&capture_sequence);
                        calf_ui_set_capture_sequence(
                            &ui, 0, 0, 0, 0, 0);
                        calf_ui_notice(
                            &ui, "INTERVAL SLEEP RESTORE FAILED", 1);
                    }
                    else
                        calf_ui_notice(
                            &ui, "INTERVAL SLEEP FAILED", 1);
                    notice_ticks = 60;
                }
            }
        }
action_deferred:
        if(gallery.active) {
            /* Media pixels are owned by ngcd's VDEC/VPSS/VO path.  Keep the
             * UI plane to transparent controls and never decode a second
             * private JPEG copy in this process. */
            if(gallery.preview_pixels != (uint32_t *)0)
                gallery_clear_preview(&ui, &gallery);
            if(!api_action_worker_busy(&action_worker) && ui.lcd_on &&
               ui.gallery_histogram_visible) {
                uint64_t now_ms = monotonic_milliseconds();
                if(next_gallery_histogram_ms == 0 ||
                   now_ms >= next_gallery_histogram_ms) {
                    int histogram_result = api_poll_histogram(&ui, 1);
                    next_gallery_histogram_ms = monotonic_milliseconds() +
                        (histogram_result == 0 ? 200u : 1000u);
                }
            }
            else
                next_gallery_histogram_ms = 0;
        }
        else
            next_gallery_histogram_ms = 0;
        if(ui.gallery_volume_visible &&
           (ui.screen != CALF_SCREEN_GALLERY || !ui.gallery_playing ||
            (gallery_volume_hide_ms != 0 &&
             monotonic_milliseconds() >= gallery_volume_hide_ms))) {
            calf_ui_set_gallery_volume_visible(&ui, 0);
            gallery_volume_hide_ms = 0;
        }
        /* ngmonitor starts ngcd and ngui as siblings. After a matched hot
         * replacement, ngui can make its one initial graph request before the
         * new backend is listening and retain camera_graph_available=false
         * even after ngcd has recovered. Reconcile only the unintended awake,
         * idle state: deliberate deep idle, interval sleep, gallery playback,
         * and active UI operations remain authoritative and untouched. */
        if(!deep_idle && !camera_graph_available && !gallery.active &&
           !capture_sequence.sleeping && !has_action &&
           ui.pending_action == CALF_ACTION_NONE) {
            uint64_t now_ms = monotonic_milliseconds();
            if(next_graph_recovery_ms == 0 ||
               now_ms >= next_graph_recovery_ms) {
                supervisor_log_event(
                    "GRAPH_RECONCILE", "stage:begin", 0);
                if(api_restore_primary_graph(
                       &ui, primary_profile, &audio_input_state) == 0) {
                    camera_graph_available = 1;
                    next_graph_recovery_ms = 0;
                    calf_ui_notice(&ui, "", 0);
                    supervisor_log_event(
                        "GRAPH_RECONCILE", "stage:complete", 0);
                }
                else {
                    next_graph_recovery_ms = now_ms +
                                             GRAPH_RECOVERY_RETRY_MS;
                    supervisor_log_event(
                        "GRAPH_RECONCILE", "stage:retry", -1);
                }
            }
        }
        else if(camera_graph_available)
            next_graph_recovery_ms = 0;
        if(!deep_idle)
            advance_recording_timer(
                &ui, &status, monotonic_milliseconds(),
                &next_recording_timer_ms);
        if(!api_action_worker_busy(&action_worker) && !deep_idle &&
           ui.lcd_on && ui.screen == CALF_SCREEN_MAIN &&
           ui.live_histogram_visible) {
            uint64_t now_ms = monotonic_milliseconds();
            if(next_live_histogram_ms == 0)
                next_live_histogram_ms = now_ms;
            else if(now_ms >= next_live_histogram_ms) {
                int histogram_result = api_poll_histogram(&ui, 0);
                next_live_histogram_ms = monotonic_milliseconds() +
                    (histogram_result == 0 ? 100u : 1000u);
            }
        }
        else {
            next_live_histogram_ms = 0;
        }
        if(!api_action_worker_busy(&action_worker) && !deep_idle &&
           ui.lcd_on && ui.status.online &&
           ui.screen == CALF_SCREEN_MAIN && !ui.live_histogram_visible &&
           !ui.capture_sequence_active) {
            uint64_t now_ms = monotonic_milliseconds();
            if(now_ms >= next_motion_ms) {
                (void)api_poll_motion(&ui);
                next_motion_ms = monotonic_milliseconds() + 67u;
            }
        }
        else {
            next_motion_ms = 0;
        }
        if(notice_ticks != 0 && --notice_ticks == 0)
            calf_ui_notice(&ui, "", 0);
        if(monotonic_milliseconds() >= next_power_sample_ms) {
            calf_power_sample_t power_sample;
            (void)target_read_power_sample(&power_sample, status.recording);
            power_samples[power_sample_count++] = power_sample;
            if(power_sample_count == POWER_SAMPLES_PER_HISTORY_POINT) {
                calf_power_sample_t average;
                (void)calf_power_average_samples(
                    power_samples, power_sample_count, &average);
                calf_ui_add_power_sample(&ui, &average);
                power_sample_count = 0;
            }
            next_power_sample_ms = monotonic_milliseconds() +
                                   POWER_ADC_POLL_INTERVAL_MS;
        }
        if(!api_action_worker_busy(&action_worker) && !deep_idle &&
           monotonic_milliseconds() >= next_wifi_status_ms) {
            if(ui.wifi_enabled) {
                wifi_refresh_connection(&ui, "");
                if(ui.wifi_ip_address[0] == '\0' &&
                   wifi_start_attempts < WIFI_START_MAX_ATTEMPTS) {
                    (void)wifi_start_service();
                    ++wifi_start_attempts;
                }
            }
            else wifi_start_attempts = 0;
            next_wifi_status_ms = monotonic_milliseconds() +
                (ui.wifi_enabled && ui.wifi_ip_address[0] == '\0' &&
                 wifi_start_attempts < WIFI_START_MAX_ATTEMPTS
                     ? WIFI_START_RETRY_INTERVAL_MS
                     : WIFI_STATUS_INTERVAL_MS);
        }
        if(!api_action_worker_busy(&action_worker) && !deep_idle &&
           ++poll_ticks >= 40) {
            (void)api_poll_status(&status);
            sync_recording_timer(&ui, &status, monotonic_milliseconds(),
                                 &next_recording_timer_ms);
            if(gallery.active)
                api_poll_gallery_info(&ui, &gallery);
            else {
                (void)api_sync_image_state(&ui, 0);
                (void)api_sync_audio_state(&ui, &audio_input_state);
                (void)api_sync_encoder_state(&ui, primary_profile);
                (void)calf_ui_sync_resolution(
                    &ui, capture_mode, primary_profile);
            }
            sync_local_datetime(&ui);
            poll_ticks = 0;
        }
        if(monotonic_milliseconds() >= next_time_floor_ms) {
            (void)persist_time_floor();
            next_time_floor_ms = monotonic_milliseconds() + 900000u;
        }
        if(indicator_led_selection == 1) {
            uint64_t now_ms = monotonic_milliseconds();
            if(next_indicator_led_enforce_ms == 0 ||
               now_ms >= next_indicator_led_enforce_ms) {
                indicator_led_force_off();
                next_indicator_led_enforce_ms = now_ms +
                    INDICATOR_LED_ENFORCE_INTERVAL_MS;
            }
        }
    }
    api_action_worker_join(&action_worker);
    (void)persist_time_floor();
    if(gallery.active) {
        gallery_clear_preview(&ui, &gallery);
        (void)gallery_close_backend(&gallery);
    }
    else if(deep_idle)
        (void)api_start_initial_camera_graph(primary_profile);
    if(capture_mode == CALF_CAPTURE_NIGHT) {
        const char *exposure = ui.exposure_known
                                   ? calf_exposure_value(
                                         (size_t)ui.exposure_index)
                                   : "0.5";
        const char *iso = ui.iso_known
                              ? calf_iso_value((size_t)ui.iso_index)
                              : "iso400";
        api_restore_standard_preview_timing();
        (void)api_set_image_direct_mode("exp", exposure, 0);
        (void)api_set_image_direct_mode("iso", iso, 0);
    }
    display_clear(&display);
    if(touch_available) close(touch.fd);
    if(keys_available) {
        int index;
        for(index = 0; index < keys.count; ++index)
            close(keys.descriptors[index]);
    }
    display_close(&display);
    gallery_destroy(&gallery);
    return 0;
}

void _start(void)
{
    int result = camera_main();
    supervisor_log_ui_exit(result);
    if(!g_supervisor_stop)
        (void)request_stock_ui_session();
    _exit(result);
}
