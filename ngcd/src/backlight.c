#include "ngcd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define BACKLIGHT_BRIGHTNESS_PATH \
    "/sys/class/backlight/backlight/brightness"

int ngcd_backlight_read(int *brightness)
{
    char buffer[32];
    char *end;
    long value;
    FILE *file;
    size_t length;
    int failed;
    if (brightness == NULL)
        return -1;
    file = fopen(BACKLIGHT_BRIGHTNESS_PATH, "r");
    if (file == NULL)
        return -1;
    length = fread(buffer, 1, sizeof(buffer) - 1U, file);
    failed = ferror(file) ||
             (length == sizeof(buffer) - 1U && fgetc(file) != EOF);
    if (fclose(file) != 0)
        failed = 1;
    if (failed || length == 0)
        return -1;
    buffer[length] = '\0';
    errno = 0;
    value = strtol(buffer, &end, 10);
    if (errno != 0 || end == buffer || value < 0 || value > 255)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    if (*end != '\0')
        return -1;
    *brightness = (int)value;
    return 0;
}

int ngcd_backlight_write(int brightness)
{
    FILE *file;
    int failed;
    if (brightness < 0 || brightness > 255)
        return -1;
    file = fopen(BACKLIGHT_BRIGHTNESS_PATH, "w");
    if (file == NULL)
        return -1;
    failed = fprintf(file, "%d\n", brightness) < 0;
    if (fclose(file) != 0)
        failed = 1;
    return failed ? -1 : 0;
}
