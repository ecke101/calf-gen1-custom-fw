#ifndef NGCD_TARGET_FCNTL_H
#define NGCD_TARGET_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NOFOLLOW 0x20000
#define O_CLOEXEC 0x80000

extern int open(const char *path, int flags, ...);

#endif
