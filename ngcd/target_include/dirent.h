#ifndef NGCD_TARGET_DIRENT_H
#define NGCD_TARGET_DIRENT_H

#include <stdint.h>

typedef struct __dirstream DIR;

struct dirent {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

extern int closedir(DIR *directory);
extern DIR *opendir(const char *path);
extern struct dirent *readdir(DIR *directory);

#endif
