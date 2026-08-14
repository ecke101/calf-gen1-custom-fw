#include "ngcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WPA_CONTROL_PATH "/var/run/wpa_supplicant/wlan0"
#define WPA_RESPONSE_MAX 65536U

static int copy_span(char *destination, size_t size, const char *start,
                     size_t length)
{
    if (size == 0 || length >= size)
        return -1;
    memcpy(destination, start, length);
    destination[length] = '\0';
    return 0;
}

static int status_field(const char *text, size_t length, const char *key,
                        char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    size_t offset = 0;
    while (offset < length) {
        size_t end = offset;
        while (end < length && text[end] != '\n' && text[end] != '\r')
            ++end;
        if (end - offset > key_length &&
            memcmp(text + offset, key, key_length) == 0 &&
            text[offset + key_length] == '=')
            return copy_span(output, output_size,
                             text + offset + key_length + 1,
                             end - offset - key_length - 1) == 0 ? 1 : -1;
        while (end < length && (text[end] == '\n' || text[end] == '\r'))
            ++end;
        offset = end;
    }
    return 0;
}

int ngcd_wifi_parse_status(const char *text, size_t length,
                           struct ngcd_wifi_info *info)
{
    char state[32];
    int result;
    if (text == NULL || info == NULL ||
        (length >= 4 && memcmp(text, "FAIL", 4) == 0))
        return -1;
    memset(info, 0, sizeof(*info));
    result = status_field(text, length, "wpa_state", state, sizeof(state));
    if (result <= 0)
        return -1;
    if (strcmp(state, "COMPLETED") != 0)
        return 0;
    if (status_field(text, length, "ssid", info->ssid,
                     sizeof(info->ssid)) < 0 ||
        status_field(text, length, "ip_address", info->ip_address,
                     sizeof(info->ip_address)) < 0 ||
        status_field(text, length, "address", info->mac_address,
                     sizeof(info->mac_address)) < 0)
        return -1;
    return 0;
}

static int scan_quality(int level)
{
    int quality = (level + 100) / 10;
    if (quality < 0)
        return 0;
    if (quality > 5)
        return 5;
    return quality;
}

static int valid_ssid(const char *text, size_t length)
{
    size_t index;
    if (length == 0 || length >= NGCD_WIFI_SSID_MAX)
        return 0;
    for (index = 0; index < length; ++index)
        if ((unsigned char)text[index] < 0x20U || text[index] == 0x7f)
            return 0;
    return 1;
}

static int parse_level(const char *start, size_t length, int *level)
{
    char value[32];
    char *end;
    long parsed;
    if (copy_span(value, sizeof(value), start, length) != 0)
        return -1;
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < -200 || parsed > 100)
        return -1;
    *level = (int)parsed;
    return 0;
}

static void sort_networks(struct ngcd_wifi_network *networks, size_t count)
{
    size_t index;
    for (index = 1; index < count; ++index) {
        struct ngcd_wifi_network item = networks[index];
        size_t position = index;
        while (position > 0 && networks[position - 1].level < item.level) {
            networks[position] = networks[position - 1];
            --position;
        }
        networks[position] = item;
    }
}

int ngcd_wifi_parse_scan_results(const char *text, size_t length,
                                 struct ngcd_wifi_network *networks,
                                 size_t capacity, size_t *count)
{
    size_t offset = 0;
    size_t found = 0;
    bool heading = true;
    if (text == NULL || networks == NULL || count == NULL || capacity == 0 ||
        (length >= 4 && memcmp(text, "FAIL", 4) == 0))
        return -1;
    while (offset < length) {
        const char *tabs[4];
        size_t end = offset;
        size_t tab_count = 0;
        size_t cursor;
        int level;
        while (end < length && text[end] != '\n' && text[end] != '\r')
            ++end;
        if (heading) {
            heading = false;
        } else {
            for (cursor = offset; cursor < end && tab_count < 4; ++cursor)
                if (text[cursor] == '\t')
                    tabs[tab_count++] = text + cursor;
            if (tab_count == 4 &&
                parse_level(tabs[1] + 1,
                            (size_t)(tabs[2] - tabs[1] - 1), &level) == 0 &&
                valid_ssid(tabs[3] + 1,
                           (size_t)(text + end - tabs[3] - 1))) {
                const char *ssid = tabs[3] + 1;
                size_t ssid_length = (size_t)(text + end - ssid);
                size_t index;
                for (index = 0; index < found; ++index)
                    if (strlen(networks[index].ssid) == ssid_length &&
                        memcmp(networks[index].ssid, ssid, ssid_length) == 0)
                        break;
                if (index < found) {
                    if (level > networks[index].level) {
                        networks[index].level = level;
                        networks[index].quality = scan_quality(level);
                    }
                } else if (found < capacity) {
                    if (copy_span(networks[found].ssid,
                                  sizeof(networks[found].ssid), ssid,
                                  ssid_length) != 0)
                        return -1;
                    networks[found].level = level;
                    networks[found].quality = scan_quality(level);
                    ++found;
                }
            }
        }
        while (end < length && (text[end] == '\n' || text[end] == '\r'))
            ++end;
        offset = end;
    }
    sort_networks(networks, found);
    *count = found;
    return 0;
}

static int wpa_request(const char *command, char *response,
                       size_t response_size)
{
    static unsigned int sequence;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    int descriptor;
    int count;
    ssize_t received;
    if (command == NULL || response == NULL || response_size < 2)
        return -1;
    descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0)
        return -1;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    count = snprintf(local.sun_path, sizeof(local.sun_path),
                     "/tmp/ngcd-wpa-%ld-%u", (long)getpid(), ++sequence);
    if (count < 0 || (size_t)count >= sizeof(local.sun_path)) {
        close(descriptor);
        return -1;
    }
    (void)unlink(local.sun_path);
    if (bind(descriptor, (const struct sockaddr *)&local, sizeof(local)) != 0 ||
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        close(descriptor);
        (void)unlink(local.sun_path);
        return -1;
    }
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    if (copy_span(remote.sun_path, sizeof(remote.sun_path), WPA_CONTROL_PATH,
                  sizeof(WPA_CONTROL_PATH) - 1) != 0 ||
        connect(descriptor, (const struct sockaddr *)&remote,
                sizeof(remote)) != 0 ||
        send(descriptor, command, strlen(command), 0) !=
            (ssize_t)strlen(command)) {
        close(descriptor);
        (void)unlink(local.sun_path);
        return -1;
    }
    received = recv(descriptor, response, response_size - 1, 0);
    close(descriptor);
    (void)unlink(local.sun_path);
    if (received <= 0)
        return -1;
    response[received] = '\0';
    return (int)received;
}

static void read_wireless_metrics(struct ngcd_wifi_info *info)
{
    char buffer[1024];
    char *cursor;
    char *end;
    FILE *file = fopen("/proc/net/wireless", "r");
    size_t length;
    long quality;
    long level;
    if (file == NULL)
        return;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    cursor = strstr(buffer, "wlan0:");
    if (cursor == NULL)
        return;
    cursor += sizeof("wlan0:") - 1;
    (void)strtol(cursor, &end, 16);
    if (end == cursor)
        return;
    quality = strtol(end, &end, 10);
    if (*end == '.')
        ++end;
    level = strtol(end, &end, 10);
    if (quality >= 0 && quality <= 100 && level >= -200 && level <= 100) {
        info->quality = (int)quality;
        info->level = (int)level;
    }
}

int ngcd_wifi_read_status(struct ngcd_wifi_info *info)
{
    char response[2048];
    int length = wpa_request("STATUS", response, sizeof(response));
    if (length < 0 || ngcd_wifi_parse_status(response, (size_t)length, info) != 0)
        return -1;
    read_wireless_metrics(info);
    return 0;
}

int ngcd_wifi_scan_begin(void)
{
    char command_response[32];
    int length;
    length = wpa_request("SCAN", command_response, sizeof(command_response));
    return length >= 2 && memcmp(command_response, "OK", 2) == 0 ? 0 : -1;
}

int ngcd_wifi_scan_results(struct ngcd_wifi_network *networks, size_t capacity,
                           size_t *count)
{
    char *response;
    int length;
    if (networks == NULL || count == NULL || capacity == 0)
        return -1;
    response = malloc(WPA_RESPONSE_MAX);
    if (response == NULL)
        return -1;
    length = wpa_request("SCAN_RESULTS", response, WPA_RESPONSE_MAX);
    if (length < 0 ||
        ngcd_wifi_parse_scan_results(response, (size_t)length, networks,
                                     capacity, count) != 0) {
        free(response);
        return -1;
    }
    free(response);
    return 0;
}

int ngcd_wifi_scan(struct ngcd_wifi_network *networks, size_t capacity,
                   size_t *count)
{
    struct timespec delay = {.tv_sec = 3, .tv_nsec = 0};
    if (ngcd_wifi_scan_begin() != 0)
        return -1;
    (void)nanosleep(&delay, NULL);
    return ngcd_wifi_scan_results(networks, capacity, count);
}
