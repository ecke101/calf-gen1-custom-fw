#ifndef NGCD_TARGET_SYS_UN_H
#define NGCD_TARGET_SYS_UN_H

#include <sys/types.h>

struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};

#endif
