#ifndef NGCD_TARGET_NETINET_IN_H
#define NGCD_TARGET_NETINET_IN_H

#include <stdint.h>

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

extern unsigned short htons(unsigned short value);

#endif
