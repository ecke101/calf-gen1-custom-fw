#ifndef NGCD_TARGET_SYS_STATVFS_H
#define NGCD_TARGET_SYS_STATVFS_H

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
    int reserved[6];
};

extern int statvfs(const char *path, struct statvfs *status);

#endif
