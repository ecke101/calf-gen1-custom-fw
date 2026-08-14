#include "target_internal.h"

static int sync_parent_directory(const char *path)
{
    char parent[256];
    size_t length = 0;
    size_t slash = length;
    int descriptor;
    int result;
    while(path[length] != '\0') ++length;
    slash = length;
    while(slash > 0 && path[slash - 1u] != '/') --slash;
    if(slash == 0) {
        parent[0] = '.';
        parent[1] = '\0';
    }
    else if(slash == 1) {
        parent[0] = '/';
        parent[1] = '\0';
    }
    else {
        size_t index;
        if(slash > sizeof(parent)) return -1;
        for(index = 0; index + 1u < slash; ++index)
            parent[index] = path[index];
        parent[slash - 1u] = '\0';
    }
    descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if(descriptor < 0) return -1;
    result = fsync(descriptor);
    if(close(descriptor) != 0) result = -1;
    return result;
}

int target_write_atomic_file(const char *destination, const char *temporary,
                             const char *text, size_t length,
                             unsigned int mode, int owner, int group)
{
    int descriptor;
    size_t written = 0;
    descriptor = open(temporary,
                      O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
    if(descriptor < 0) return -1;
    if(fchmod(descriptor, mode) != 0) goto failed;
    /* The stock profile writer also treats ownership adjustment as best effort:
     * ngui may run as the target user without CAP_CHOWN. */
    if(owner >= 0 && group >= 0)
        (void)fchown(descriptor, (unsigned)owner, (unsigned)group);
    while(written < length) {
        ssize_t count = write(descriptor, text + written, length - written);
        if(count <= 0) goto failed;
        written += (size_t)count;
    }
    if(fsync(descriptor) != 0 || close(descriptor) != 0) {
        descriptor = -1;
        goto failed;
    }
    descriptor = -1;
    if(rename(temporary, destination) != 0) goto failed;
    /* Some camera filesystems publish the rename successfully but reject
     * fsync() on the directory. The file itself is already fully synced. */
    (void)sync_parent_directory(destination);
    return 0;

failed:
    if(descriptor >= 0) (void)close(descriptor);
    (void)unlink(temporary);
    return -1;
}
