#ifndef NGCD_TARGET_TIME_H
#define NGCD_TARGET_TIME_H

#include <stddef.h>
#include <stdint.h>

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

extern int clock_gettime(int clock_id, struct timespec *time);
extern int nanosleep(const struct timespec *duration,
                     struct timespec *remaining);
extern struct tm *localtime_r(const long *time, struct tm *result);
extern size_t strftime(char *output, size_t size, const char *format,
                       const struct tm *time);

#endif
