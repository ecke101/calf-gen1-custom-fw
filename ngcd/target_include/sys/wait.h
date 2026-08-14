#ifndef NGCD_TARGET_SYS_WAIT_H
#define NGCD_TARGET_SYS_WAIT_H

#include <sys/types.h>

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

extern pid_t waitpid(pid_t child, int *status, int options);

#endif
