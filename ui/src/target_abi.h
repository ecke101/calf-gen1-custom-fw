#ifndef CALF_TARGET_ABI_H
#define CALF_TARGET_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Minimal Linux/glibc ABI declarations keep the camera build independent of
 * host multiarch headers. */
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int socklen_t;
typedef unsigned long pthread_t;
typedef void DIR;

struct sockaddr {
    unsigned short family;
    char data[14];
};

struct sockaddr_in {
    unsigned short family;
    unsigned short port;
    uint32_t address;
    unsigned char zero[8];
};

struct timeval {
    long seconds;
    long microseconds;
};

struct timespec {
    long seconds;
    long nanoseconds;
};

struct tm {
    int second;
    int minute;
    int hour;
    int month_day;
    int month;
    int year;
    int week_day;
    int year_day;
    int is_dst;
    long utc_offset;
    const char *zone;
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct dirent {
    unsigned long inode;
    long offset;
    unsigned short record_length;
    unsigned char type;
    char name[256];
};

extern int open(const char *path, int flags, ...);
extern int close(int descriptor);
extern ssize_t read(int descriptor, void *buffer, size_t count);
extern ssize_t write(int descriptor, const void *buffer, size_t count);
extern off_t lseek(int descriptor, off_t offset, int origin);
extern int ioctl(int descriptor, unsigned long request, ...);
extern int poll(struct pollfd *descriptors, unsigned long count,
                int timeout_ms);
extern void *mmap(void *address, size_t length, int protection, int flags,
                  int descriptor, off_t offset);
extern int munmap(void *address, size_t length);
extern int fsync(int descriptor);
extern int fchmod(int descriptor, unsigned int mode);
extern int fchown(int descriptor, unsigned int owner, unsigned int group);
extern int rename(const char *old_path, const char *new_path);
extern int unlink(const char *path);
extern DIR *opendir(const char *path);
extern struct dirent *readdir(DIR *directory);
extern int closedir(DIR *directory);
extern void sync(void);
extern int clock_gettime(int clock_id, struct timespec *time);
extern long time(long *result);
extern struct tm *localtime_r(const long *clock, struct tm *result);
extern long mktime(struct tm *local);
extern int settimeofday(const struct timeval *time, const void *timezone);
extern int setenv(const char *name, const char *value, int overwrite);
extern void tzset(void);
extern int system(const char *command);
extern pid_t fork(void);
extern pid_t getpid(void);
extern int execv(const char *path, char *const arguments[]);
extern pid_t waitpid(pid_t process, int *status, int options);
extern int pthread_create(pthread_t *thread, const void *attributes,
                          void *(*start_routine)(void *), void *argument);
extern int pthread_join(pthread_t thread, void **result);
extern int socket(int domain, int type, int protocol);
extern int connect(int descriptor, const struct sockaddr *address,
                   socklen_t length);
extern int setsockopt(int descriptor, int level, int option,
                      const void *value, socklen_t length);
extern ssize_t send(int descriptor, const void *buffer, size_t length,
                    int flags);
extern ssize_t recv(int descriptor, void *buffer, size_t length, int flags);
extern int usleep(unsigned int microseconds);
extern void *memcpy(void *destination, const void *source, size_t count);
extern void *malloc(size_t size);
extern void free(void *pointer);
extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
extern int dlclose(void *handle);
extern void _exit(int status);
extern void (*signal(int signal_number, void (*handler)(int)))(int);

extern int RK_MPI_MB_UniqueId2Fd(int unique_id);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_NOFOLLOW 0x20000
#define O_DIRECTORY 0x10000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)(intptr_t)-1)
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define POLLIN 0x0001
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
#define CLOCK_MONOTONIC 1

#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define SYN_REPORT 0
#define BTN_TOUCH 0x14a
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define EVIOCGABS_MT_SLOT 0x8018456ful

#define KEY_F1 59
#define KEY_F2 60
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_POWER 116
#define KEY_MENU 139
#define KEY_FILE 144
#define KEY_BACK 158
#define KEY_RECORD 167

#endif
