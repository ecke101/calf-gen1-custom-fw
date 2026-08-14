#ifndef NGCD_TARGET_ERRNO_H
#define NGCD_TARGET_ERRNO_H

extern int *__errno_location(void);
#define errno (*__errno_location())

#define ENOENT 2
#define EINTR 4
#define EAGAIN 11
#define ENOMEM 12
#define EEXIST 17
#define EINVAL 22
#define EIO 5
#define EWOULDBLOCK EAGAIN

#endif
