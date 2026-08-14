#include "ngcd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define BATTERY_CAPACITY_PATH \
    "/sys/class/power_supply/cw2015-battery/capacity"
#define USB_ONLINE_PATH \
    "/sys/class/power_supply/bq25700-charger/online"
#define SYSTEM_TEMPERATURE_PATH "/sys/class/thermal/thermal_zone7/temp"
#define CORE_TEMPERATURE_PATH "/sys/class/thermal/thermal_zone0/temp"

int ngcd_power_parse_value(const char *text, int minimum, int maximum,
                           int *value)
{
    char *end;
    long parsed;
    if (text == NULL || value == NULL || minimum > maximum)
        return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || parsed < minimum || parsed > maximum)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end != '\0')
        return -1;
    *value = (int)parsed;
    return 0;
}

static int read_value(const char *path, int minimum, int maximum, int *value)
{
    char buffer[64];
    FILE *file = fopen(path, "r");
    size_t length;
    int failed;
    if (file == NULL)
        return -1;
    length = fread(buffer, 1, sizeof(buffer) - 1U, file);
    failed = ferror(file) ||
             (length == sizeof(buffer) - 1U && fgetc(file) != EOF);
    if (fclose(file) != 0)
        failed = 1;
    if (failed)
        return -1;
    buffer[length] = '\0';
    return ngcd_power_parse_value(buffer, minimum, maximum, value);
}

int ngcd_power_read_status(struct ngcd_power_info *info)
{
    int system_millidegrees;
    int core_millidegrees;
    if (info == NULL)
        return -1;
    info->battery_percent = -1;
    info->usb_supply = 0;
    info->system_temperature = 0;
    info->core_temperature = 0;
    if (read_value(BATTERY_CAPACITY_PATH, 0, 100,
                   &info->battery_percent) != 0 ||
        read_value(USB_ONLINE_PATH, 0, 1, &info->usb_supply) != 0)
        return -1;
    if (read_value(SYSTEM_TEMPERATURE_PATH, -100000, 300000,
                   &system_millidegrees) == 0)
        info->system_temperature = system_millidegrees / 1000;
    if (read_value(CORE_TEMPERATURE_PATH, -100000, 300000,
                   &core_millidegrees) == 0)
        info->core_temperature = core_millidegrees / 1000;
    return 0;
}
