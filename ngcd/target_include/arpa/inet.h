#ifndef NGCD_TARGET_ARPA_INET_H
#define NGCD_TARGET_ARPA_INET_H

#include <sys/socket.h>

extern int inet_pton(int family, const char *source, void *destination);
extern const char *inet_ntop(int family, const void *source,
                             char *destination, socklen_t size);

#endif
