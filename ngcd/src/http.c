#include "ngcd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HTTP_HEADER_MAX 16384U
#define HTTP_BUFFER_MAX (HTTP_HEADER_MAX + NGCD_HTTP_BODY_MAX)
#define HTTP_CONNECTION_TIMEOUT_MS 5000U

static volatile sig_atomic_t shutdown_requested;

void ngcd_request_shutdown(void)
{
    shutdown_requested = 1;
}

static int equal_case(const char *left, size_t left_length, const char *right)
{
    size_t index;
    if (strlen(right) != left_length)
        return 0;
    for (index = 0; index < left_length; ++index)
        if (tolower((unsigned char)left[index]) !=
            tolower((unsigned char)right[index]))
            return 0;
    return 1;
}

static char *find_bytes(char *data, size_t length, const char *needle,
                        size_t needle_length)
{
    size_t index;
    if (needle_length > length)
        return NULL;
    for (index = 0; index + needle_length <= length; ++index)
        if (memcmp(data + index, needle, needle_length) == 0)
            return data + index;
    return NULL;
}

static int parse_size(const char *text, size_t length, size_t *value)
{
    size_t result = 0;
    size_t index;
    if (length == 0)
        return -1;
    for (index = 0; index < length; ++index) {
        unsigned int digit;
        if (text[index] < '0' || text[index] > '9')
            return -1;
        digit = (unsigned int)(text[index] - '0');
        if (result > (NGCD_HTTP_BODY_MAX - digit) / 10U)
            return -1;
        result = result * 10U + digit;
    }
    *value = result;
    return 0;
}

static enum ngcd_method parse_method(const char *text, size_t length)
{
    if (length == 3 && memcmp(text, "GET", 3) == 0)
        return NGCD_METHOD_GET;
    if (length == 4 && memcmp(text, "POST", 4) == 0)
        return NGCD_METHOD_POST;
    if (length == 7 && memcmp(text, "OPTIONS", 7) == 0)
        return NGCD_METHOD_OPTIONS;
    return NGCD_METHOD_UNKNOWN;
}

static int parse_request(char *buffer, size_t header_length,
                         size_t available_length, struct ngcd_request *request,
                         size_t *required_length)
{
    char *line_end = find_bytes(buffer, header_length, "\r\n", 2);
    char *method_end;
    char *target_end;
    char *cursor;
    size_t content_length = 0;
    bool content_length_seen = false;

    memset(request, 0, sizeof(*request));
    if (line_end == NULL)
        return -1;
    method_end = memchr(buffer, ' ', (size_t)(line_end - buffer));
    if (method_end == NULL)
        return -1;
    request->method = parse_method(buffer, (size_t)(method_end - buffer));
    if (request->method == NGCD_METHOD_UNKNOWN)
        return -1;
    target_end = memchr(method_end + 1, ' ',
                        (size_t)(line_end - method_end - 1));
    if (target_end == NULL ||
        (size_t)(target_end - method_end - 1) >= sizeof(request->path) +
                                                    sizeof(request->query))
        return -1;
    if ((size_t)(line_end - target_end) != sizeof(" HTTP/1.1") - 1 &&
        (size_t)(line_end - target_end) != sizeof(" HTTP/1.0") - 1)
        return -1;
    if (memcmp(target_end, " HTTP/1.1", sizeof(" HTTP/1.1") - 1) != 0 &&
        memcmp(target_end, " HTTP/1.0", sizeof(" HTTP/1.0") - 1) != 0)
        return -1;
    {
        char *query = memchr(method_end + 1, '?',
                             (size_t)(target_end - method_end - 1));
        char *path_end = query != NULL ? query : target_end;
        size_t path_length = (size_t)(path_end - method_end - 1);
        size_t query_length = query != NULL ? (size_t)(target_end - query - 1) : 0;
        if (path_length == 0 || path_length >= sizeof(request->path) ||
            query_length >= sizeof(request->query))
            return -1;
        memcpy(request->path, method_end + 1, path_length);
        request->path[path_length] = '\0';
        if (query != NULL) {
            memcpy(request->query, query + 1, query_length);
            request->query[query_length] = '\0';
        }
    }

    cursor = line_end + 2;
    while (cursor < buffer + header_length - 2) {
        char *next = find_bytes(cursor,
                                (size_t)(buffer + header_length - cursor),
                                "\r\n", 2);
        char *colon;
        char *value;
        char *value_end;
        if (next == NULL || next == cursor)
            break;
        colon = memchr(cursor, ':', (size_t)(next - cursor));
        if (colon == NULL)
            return -1;
        value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t'))
            ++value;
        value_end = next;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t'))
            --value_end;
        if (equal_case(cursor, (size_t)(colon - cursor), "Content-Length")) {
            size_t parsed;
            if (parse_size(value, (size_t)(value_end - value), &parsed) != 0 ||
                (content_length_seen && parsed != content_length))
                return -1;
            content_length = parsed;
            content_length_seen = true;
        } else if (equal_case(cursor, (size_t)(colon - cursor),
                              "Transfer-Encoding") &&
                   !equal_case(value, (size_t)(value_end - value), "identity")) {
            return -1;
        }
        cursor = next + 2;
    }

    *required_length = header_length + content_length;
    if (*required_length > HTTP_BUFFER_MAX)
        return -1;
    if (available_length < *required_length)
        return 1;
    request->body = content_length > 0 ? buffer + header_length : NULL;
    request->body_length = content_length;
    return 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int connection_expired(struct ngcd_app *app, uint64_t deadline)
{
    (void)app->backend.ops->tick(&app->backend);
    return shutdown_requested || monotonic_milliseconds() >= deadline;
}

static int send_all(struct ngcd_app *app, int descriptor, const void *data,
                    size_t length, uint64_t deadline)
{
    const char *cursor = data;
    while (length > 0) {
        ssize_t count = send(descriptor, cursor, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) {
            if (connection_expired(app, deadline))
                return -1;
            continue;
        }
        if (count <= 0)
            return -1;
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
}

static const char *status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Error";
    }
}

static int write_response(struct ngcd_app *app, int descriptor,
                          const struct ngcd_response *response,
                          uint64_t deadline)
{
    char header[1024];
    int count = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n",
        response->status, status_reason(response->status),
        response->content_type[0] != '\0' ? response->content_type
                                           : "application/json",
        response->body_length);
    if (count < 0 || (size_t)count >= sizeof(header) ||
        send_all(app, descriptor, header, (size_t)count, deadline) != 0)
        return -1;
    return response->body_length == 0 ||
                   send_all(app, descriptor,
                            response->body_data != NULL
                                ? response->body_data : response->body,
                            response->body_length, deadline) == 0
               ? 0 : -1;
}

static void bad_request(struct ngcd_app *app, int descriptor, int status,
                        const char *message, uint64_t deadline)
{
    struct ngcd_response response;
    int count;
    memset(&response, 0, sizeof(response));
    response.status = status;
    memcpy(response.content_type, "application/json", sizeof("application/json"));
    count = snprintf(response.body, sizeof(response.body),
                     "{\"code\":-1,\"message\":\"%s\"}", message);
    if (count > 0 && (size_t)count < sizeof(response.body))
        response.body_length = (size_t)count;
    (void)write_response(app, descriptor, &response, deadline);
}

static void serve_connection(struct ngcd_app *app, int descriptor)
{
    char *buffer = malloc(HTTP_BUFFER_MAX);
    size_t used = 0;
    size_t header_length = 0;
    size_t required_length = 0;
    struct ngcd_request request;
    struct ngcd_response response;
    int parsed;
    uint64_t deadline = monotonic_milliseconds() +
                        HTTP_CONNECTION_TIMEOUT_MS;

    if (buffer == NULL) {
        bad_request(app, descriptor, 500, "out of memory", deadline);
        return;
    }
    for (;;) {
        ssize_t count;
        char *header_end;
        if (used == HTTP_BUFFER_MAX) {
            bad_request(app, descriptor, 413, "request too large", deadline);
            free(buffer);
            return;
        }
        count = recv(descriptor, buffer + used, HTTP_BUFFER_MAX - used, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) {
            if (connection_expired(app, deadline)) {
                free(buffer);
                return;
            }
            continue;
        }
        if (count <= 0) {
            free(buffer);
            return;
        }
        used += (size_t)count;
        if (header_length == 0) {
            header_end = find_bytes(buffer, used, "\r\n\r\n", 4);
            if (header_end == NULL) {
                if (used >= HTTP_HEADER_MAX) {
                    bad_request(app, descriptor, 413, "headers too large",
                                deadline);
                    free(buffer);
                    return;
                }
                continue;
            }
            header_length = (size_t)(header_end - buffer) + 4;
        }
        parsed = parse_request(buffer, header_length, used, &request,
                               &required_length);
        if (parsed < 0) {
            bad_request(app, descriptor, 400, "malformed request", deadline);
            free(buffer);
            return;
        }
        if (parsed == 0)
            break;
    }
    (void)ngcd_dispatch(app, &request, &response);
    (void)write_response(app, descriptor, &response, deadline);
    free(buffer);
}

int ngcd_serve(struct ngcd_app *app, const char *address, uint16_t port)
{
    int descriptor;
    int enabled = 1;
    struct sockaddr_in socket_address;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 5000};
    /* Media packets are drained by backend ticks in this service loop. Keep
     * the idle accept wait comfortably below a 60 fps frame interval so the
     * encoder queue cannot be starved while no HTTP clients are connected. */
    struct timeval accept_timeout = {.tv_sec = 0, .tv_usec = 5000};

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0)
        return -1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0 ||
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout,
                   sizeof(accept_timeout)) != 0) {
        close(descriptor);
        return -1;
    }
    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &socket_address.sin_addr) != 1 ||
        bind(descriptor, (const struct sockaddr *)&socket_address,
             sizeof(socket_address)) != 0 ||
        listen(descriptor, 32) != 0) {
        close(descriptor);
        return -1;
    }
    while (!shutdown_requested) {
        (void)app->backend.ops->tick(&app->backend);
        int connection = accept(descriptor, NULL, NULL);
        if (connection < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            close(descriptor);
            return -1;
        }
        (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
        (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        serve_connection(app, connection);
        close(connection);
    }
    close(descriptor);
    return 0;
}
