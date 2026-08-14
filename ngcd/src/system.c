#include "ngcd.h"

#include <fcntl.h>
#include <sys/reboot.h>
#include <unistd.h>

/* sync() is present in the target libc but hidden by strict POSIX feature
 * selection on some host libcs. */
extern void sync(void);

int ngcd_write_stock_session_marker(const char *path)
{
    static const char marker[] = "ngcd-fallback\n";
    int descriptor;
    ssize_t written;
    if (path == NULL || path[0] != '/')
        return -1;
    descriptor = open(path,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    if (descriptor < 0)
        return -1;
    written = write(descriptor, marker, sizeof(marker) - 1U);
    if (close(descriptor) != 0 || written != (ssize_t)(sizeof(marker) - 1U))
        return -1;
    return 0;
}

int ngcd_select_stock_session(void)
{
    return ngcd_write_stock_session_marker(NGCD_STOCK_SESSION_MARKER);
}

int ngcd_system_poweroff(void)
{
    sync();
    return reboot(RB_POWER_OFF);
}
