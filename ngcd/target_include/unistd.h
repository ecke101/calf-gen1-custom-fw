#ifndef NGCD_TARGET_UNISTD_H
#define NGCD_TARGET_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define F_OK 0

extern int access(const char *path, int mode);
extern int close(int descriptor);
extern int fdatasync(int descriptor);
extern int execv(const char *path, char *const arguments[]);
extern pid_t fork(void);
extern int fsync(int descriptor);
extern int ftruncate(int descriptor, off_t length);
extern int getpid(void);
extern off_t lseek(int descriptor, off_t offset, int origin);
extern ssize_t read(int descriptor, void *buffer, size_t size);
extern ssize_t readlink(const char *path, char *buffer, size_t size);
extern int unlink(const char *path);
extern ssize_t write(int descriptor, const void *buffer, size_t size);
extern void sync(void);
extern void _exit(int status) __attribute__((noreturn));

#endif
