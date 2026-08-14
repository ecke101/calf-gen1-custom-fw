#ifndef NGCD_TARGET_SYS_SOCKET_H
#define NGCD_TARGET_SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

#define AF_INET 2
#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

extern int accept(int descriptor, struct sockaddr *address,
                  socklen_t *length);
extern int bind(int descriptor, const struct sockaddr *address,
                socklen_t length);
extern int connect(int descriptor, const struct sockaddr *address,
                   socklen_t length);
extern int listen(int descriptor, int backlog);
extern ssize_t recv(int descriptor, void *buffer, size_t length, int flags);
extern ssize_t send(int descriptor, const void *buffer, size_t length,
                    int flags);
extern int setsockopt(int descriptor, int level, int option,
                      const void *value, socklen_t length);
extern int socket(int domain, int type, int protocol);

#endif
