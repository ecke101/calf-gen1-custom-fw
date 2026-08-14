#include "ngcd.h"
#include "ngcd_playback.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MOUNTS_PATH "/proc/mounts"
#define MOUNTS_MAX 32768U
#define IO_TEST_BLOCK_KB_MAX 4096
#define IO_TEST_BYTES_MAX (UINT64_C(64) * 1024U * 1024U)
#define IO_TEST_FREE_RESERVE (UINT64_C(16) * 1024U * 1024U)
#define MEDIA_SEQUENCE_FIRST 1000000U
#define MEDIA_SEQUENCE_LAST 9999999U

static const char *const STORAGE_LOCATIONS[] = {
    "/mnt/mmcblk1p1",
    "/mnt/mmcblk1p2",
    "/mnt/sda2",
};

static int copy_location(char *output, size_t output_size,
                         const char *start, size_t length)
{
    if (output == NULL || output_size == 0 || length >= output_size)
        return -1;
    memcpy(output, start, length);
    output[length] = '\0';
    return 0;
}

static int next_field(const char *text, size_t line_end, size_t *cursor,
                      size_t *start, size_t *end)
{
    while (*cursor < line_end &&
           (text[*cursor] == ' ' || text[*cursor] == '\t'))
        ++*cursor;
    if (*cursor == line_end)
        return -1;
    *start = *cursor;
    while (*cursor < line_end && text[*cursor] != ' ' &&
           text[*cursor] != '\t')
        ++*cursor;
    *end = *cursor;
    return 0;
}

static bool mount_options_read_only(const char *start, const char *end)
{
    while (start < end) {
        const char *option_end = start;
        while (option_end < end && *option_end != ',')
            ++option_end;
        if (option_end - start == 2 && start[0] == 'r' && start[1] == 'o')
            return true;
        start = option_end < end ? option_end + 1 : end;
    }
    return false;
}

int ngcd_storage_parse_mounts(const char *text, size_t length,
                              char *location, size_t location_size,
                              bool *read_only)
{
    size_t offset = 0;
    if (text == NULL || location == NULL || location_size == 0 ||
        read_only == NULL)
        return -1;
    location[0] = '\0';
    *read_only = false;
    while (offset < length) {
        size_t line_end = offset;
        size_t cursor = offset;
        size_t device_start;
        size_t device_end;
        size_t mount_start;
        size_t mount_end;
        size_t filesystem_start;
        size_t filesystem_end;
        size_t options_start;
        size_t options_end;
        size_t index;
        while (line_end < length && text[line_end] != '\n')
            ++line_end;
        if (next_field(text, line_end, &cursor,
                       &device_start, &device_end) != 0 ||
            next_field(text, line_end, &cursor,
                       &mount_start, &mount_end) != 0 ||
            next_field(text, line_end, &cursor,
                       &filesystem_start, &filesystem_end) != 0 ||
            next_field(text, line_end, &cursor,
                       &options_start, &options_end) != 0) {
            offset = line_end < length ? line_end + 1U : line_end;
            continue;
        }
        (void)device_start;
        (void)device_end;
        (void)filesystem_start;
        (void)filesystem_end;
        for (index = 0;
             index < sizeof(STORAGE_LOCATIONS) / sizeof(STORAGE_LOCATIONS[0]);
             ++index) {
            size_t expected = strlen(STORAGE_LOCATIONS[index]);
            if (mount_end - mount_start == expected &&
                memcmp(text + mount_start, STORAGE_LOCATIONS[index],
                       expected) == 0) {
                if (copy_location(location, location_size,
                                  text + mount_start, expected) == 0) {
                    *read_only = mount_options_read_only(
                        text + options_start, text + options_end);
                    return 0;
                } else {
                    return -1;
                }
            }
        }
        offset = line_end < length ? line_end + 1U : line_end;
    }
    return -1;
}

static int read_mounts(char *buffer, size_t size, size_t *length)
{
    FILE *file;
    size_t count;
    int failed;
    if (buffer == NULL || size < 2 || length == NULL)
        return -1;
    file = fopen(MOUNTS_PATH, "r");
    if (file == NULL)
        return -1;
    count = fread(buffer, 1, size - 1U, file);
    failed = ferror(file) || (count == size - 1U && fgetc(file) != EOF);
    if (fclose(file) != 0)
        failed = 1;
    if (failed)
        return -1;
    buffer[count] = '\0';
    *length = count;
    return 0;
}

static int multiply_blocks(uint64_t blocks, uint64_t block_size,
                           uint64_t *bytes)
{
    if (block_size != 0 && blocks > UINT64_MAX / block_size)
        return -1;
    *bytes = blocks * block_size;
    return 0;
}

int ngcd_storage_read_status(struct ngcd_storage_info *info)
{
    char mounts[MOUNTS_MAX];
    struct statvfs status;
    size_t length;
    uint64_t block_size;
    if (info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    if (read_mounts(mounts, sizeof(mounts), &length) != 0 ||
        ngcd_storage_parse_mounts(mounts, length, info->location,
                                  sizeof(info->location),
                                  &info->read_only) != 0 ||
        statvfs(info->location, &status) != 0)
        return -1;
    block_size = status.f_frsize != 0 ? status.f_frsize : status.f_bsize;
    if (block_size == 0 ||
        multiply_blocks((uint64_t)status.f_blocks, block_size,
                        &info->total_bytes) != 0 ||
        multiply_blocks((uint64_t)status.f_bavail, block_size,
                        &info->free_bytes) != 0 ||
        info->free_bytes > info->total_bytes) {
        memset(info, 0, sizeof(*info));
        return -1;
    }
    return 0;
}

static int ensure_real_directory(const char *path)
{
    DIR *directory;
    char link_target;
    if (mkdir(path, 0775) != 0 && errno != EEXIST)
        return -1;
    if (readlink(path, &link_target, 1U) >= 0 || errno != EINVAL)
        return -1;
    directory = opendir(path);
    if (directory == NULL)
        return -1;
    return closedir(directory);
}

static int media_name_sequence(const char *name, unsigned int *sequence)
{
    unsigned int value = 0U;
    size_t index;
    if (name == NULL || sequence == NULL || strlen(name) != 12U ||
        name[0] != 'V' || name[8] != '.' ||
        (strcasecmp(name + 9, "jpg") != 0 &&
         strcasecmp(name + 9, "mp4") != 0))
        return -1;
    for (index = 1U; index <= 7U; ++index) {
        if (name[index] < '0' || name[index] > '9')
            return -1;
        value = value * 10U + (unsigned int)(name[index] - '0');
    }
    if (value < MEDIA_SEQUENCE_FIRST || value > MEDIA_SEQUENCE_LAST)
        return -1;
    *sequence = value;
    return 0;
}

static int reserved_media_sequence(const char *name, unsigned int *sequence)
{
    const char *stem = name;
    const char *suffix;
    unsigned int value = 0U;
    size_t index;
    if (name == NULL || sequence == NULL)
        return -1;
    /* In-progress files are named .Vddddddd.ext.ngcd-PID.tmp.  Count them as
     * reservations so a segment rotation cannot reuse its predecessor's
     * number before that predecessor is atomically published. */
    if (stem[0] == '.')
        ++stem;
    if ((stem[0] != 'V' && stem[0] != 'P') || strlen(stem) < 12U)
        return -1;
    for (index = 1U; index <= 7U; ++index) {
        if (stem[index] < '0' || stem[index] > '9')
            return -1;
        value = value * 10U + (unsigned int)(stem[index] - '0');
    }
    suffix = stem + 8U;
    if (strcasecmp(suffix, ".jpg") != 0 &&
        strcasecmp(suffix, ".mp4") != 0 &&
        strcasecmp(suffix, "-L.dng") != 0 &&
        strcasecmp(suffix, "-R.dng") != 0 &&
        strncmp(suffix, ".jpg.ngcd-", sizeof(".jpg.ngcd-") - 1U) != 0 &&
        strncmp(suffix, ".mp4.ngcd-", sizeof(".mp4.ngcd-") - 1U) != 0)
        return -1;
    if (value < MEDIA_SEQUENCE_FIRST || value > MEDIA_SEQUENCE_LAST)
        return -1;
    *sequence = value;
    return 0;
}

static int media_directory_name_valid(const char *name)
{
    size_t index;
    if (name == NULL || strlen(name) < 4U || strlen(name) >= 64U ||
        name[0] < '1' || name[0] > '9' ||
        name[1] < '0' || name[1] > '9' ||
        name[2] < '0' || name[2] > '9')
        return 0;
    for (index = 3U; name[index] != '\0'; ++index)
        if (!((name[index] >= 'A' && name[index] <= 'Z') ||
              (name[index] >= '0' && name[index] <= '9') ||
              name[index] == '_'))
            return 0;
    return 1;
}

static int media_entry_compare(const void *left, const void *right)
{
    const struct ngcd_media_entry *a = left;
    const struct ngcd_media_entry *b = right;
    return strcmp(b->path, a->path);
}

static int reserve_media_entries(struct ngcd_media_entry **entries,
                                 size_t *capacity, size_t needed)
{
    struct ngcd_media_entry *resized;
    size_t next = *capacity == 0U ? 128U : *capacity;
    if (needed > 10000U)
        return -1;
    while (next < needed) {
        if (next > 5000U)
            next = 10000U;
        else
            next *= 2U;
    }
    if (next == *capacity)
        return 0;
    resized = realloc(*entries, next * sizeof(**entries));
    if (resized == NULL)
        return -1;
    *entries = resized;
    *capacity = next;
    return 0;
}

static int scan_media_directory(const char *directory_path,
                                struct ngcd_media_entry **entries,
                                size_t *count, size_t *capacity)
{
    DIR *directory = opendir(directory_path);
    struct dirent *entry;
    int saved_errno = 0;
    if (directory == NULL)
        return errno == ENOENT ? 0 : -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct ngcd_media_entry item;
        off_t file_size;
        int descriptor;
        unsigned int sequence;
        int length;
        if (media_name_sequence(entry->d_name, &sequence) != 0)
            continue;
        (void)sequence;
        memset(&item, 0, sizeof(item));
        length = snprintf(item.path, sizeof(item.path), "%s/%s",
                          directory_path, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(item.path))
            continue;
        descriptor = open(item.path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        file_size = lseek(descriptor, 0, SEEK_END);
        if (close(descriptor) != 0 || file_size <= 0)
            continue;
        memcpy(item.name, entry->d_name, strlen(entry->d_name) + 1U);
        item.size = (uint64_t)file_size;
        item.create_time = ngcd_file_create_time(item.path);
        item.video = strcasecmp(entry->d_name + 9U, "mp4") == 0;
        if (item.video) {
            struct ngcd_mp4_reader *reader = NULL;
            if (ngcd_mp4_reader_open(&reader, item.path) != 0)
                continue;
            ngcd_mp4_reader_close(reader);
        } else {
            unsigned int width;
            unsigned int height;
            uint64_t probed_size;
            if (ngcd_jpeg_probe(item.path, &width, &height,
                                &probed_size) != 0 ||
                probed_size != item.size)
                continue;
        }
        if (reserve_media_entries(entries, capacity, *count + 1U) != 0) {
            saved_errno = ENOMEM;
            break;
        }
        (*entries)[(*count)++] = item;
        errno = 0;
    }
    if (saved_errno == 0)
        saved_errno = errno;
    if (closedir(directory) != 0 && saved_errno == 0)
        saved_errno = errno != 0 ? errno : EIO;
    return saved_errno == 0 ? 0 : -1;
}

int ngcd_storage_media_list(const char *root, size_t offset,
                            struct ngcd_media_entry *output,
                            size_t output_capacity, size_t *output_count,
                            size_t *total)
{
    char dcim[NGCD_PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    struct ngcd_media_entry *entries = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t copy_count;
    int result = -1;
    int length;
    if (root == NULL || root[0] != '/' || output_count == NULL ||
        total == NULL || (output_capacity > 0U && output == NULL))
        return -1;
    *output_count = 0U;
    *total = 0U;
    length = snprintf(dcim, sizeof(dcim), "%s/DCIM", root);
    if (length <= 0 || (size_t)length >= sizeof(dcim))
        return -1;
    directory = opendir(dcim);
    if (directory == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char media_directory[NGCD_PATH_MAX];
        char link_target;
        if (!media_directory_name_valid(entry->d_name))
            continue;
        length = snprintf(media_directory, sizeof(media_directory), "%s/%s",
                          dcim, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(media_directory) ||
            readlink(media_directory, &link_target, 1U) >= 0 ||
            errno != EINVAL ||
            scan_media_directory(media_directory, &entries,
                                 &count, &capacity) != 0)
            goto done;
        errno = 0;
    }
    {
        int scan_errno = errno;
        int close_result = closedir(directory);
        directory = NULL;
        if (scan_errno != 0 || close_result != 0)
            goto done;
    }
    qsort(entries, count, sizeof(*entries), media_entry_compare);
    *total = count;
    if (offset > count)
        offset = count;
    copy_count = count - offset;
    if (copy_count > output_capacity)
        copy_count = output_capacity;
    if (copy_count > 0U)
        memcpy(output, entries + offset, copy_count * sizeof(*output));
    *output_count = copy_count;
    result = 0;

done:
    if (directory != NULL)
        (void)closedir(directory);
    free(entries);
    return result;
}

static int path_absent(const char *path)
{
    char link_target;
    if (readlink(path, &link_target, 1U) >= 0)
        return 0;
    if (errno != EINVAL && errno != ENOENT)
        return -1;
    if (access(path, F_OK) == 0)
        return 0;
    return errno == ENOENT ? 1 : -1;
}

int ngcd_storage_media_paths(const char *root, const char *extension,
                             unsigned int *sequence,
                             char *basename, size_t basename_size,
                             char *temporary, size_t temporary_size,
                             char *final_path, size_t final_size)
{
    char dcim[NGCD_PATH_MAX];
    char media[NGCD_PATH_MAX];
    char other_jpg[NGCD_PATH_MAX];
    char other_mp4[NGCD_PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    unsigned int highest = 0U;
    unsigned int candidate;
    int available = 0;
    int scan_error;
    int count;
    if (root == NULL || root[0] != '/' || sequence == NULL ||
        basename == NULL || basename_size == 0U || temporary == NULL ||
        temporary_size == 0U || final_path == NULL || final_size == 0U ||
        extension == NULL ||
        (strcmp(extension, "jpg") != 0 && strcmp(extension, "mp4") != 0))
        return -1;
    count = snprintf(dcim, sizeof(dcim), "%s/DCIM", root);
    if (count <= 0 || (size_t)count >= sizeof(dcim) ||
        ensure_real_directory(root) != 0 || ensure_real_directory(dcim) != 0)
        return -1;
    count = snprintf(media, sizeof(media), "%s/100_CALF", dcim);
    if (count <= 0 || (size_t)count >= sizeof(media) ||
        ensure_real_directory(media) != 0)
        return -1;
    directory = opendir(media);
    if (directory == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        unsigned int found;
        if (reserved_media_sequence(entry->d_name, &found) == 0 &&
            found > highest)
            highest = found;
        errno = 0;
    }
    scan_error = errno;
    if (closedir(directory) != 0 || scan_error != 0)
        return -1;
    candidate = highest;
    if (candidate < MEDIA_SEQUENCE_FIRST - 1U)
        candidate = MEDIA_SEQUENCE_FIRST - 1U;
    while (candidate < MEDIA_SEQUENCE_LAST) {
        int jpg_absent;
        int mp4_absent;
        ++candidate;
        count = snprintf(other_jpg, sizeof(other_jpg),
                         "%s/V%07u.jpg", media, candidate);
        if (count <= 0 || (size_t)count >= sizeof(other_jpg))
            return -1;
        count = snprintf(other_mp4, sizeof(other_mp4),
                         "%s/V%07u.mp4", media, candidate);
        if (count <= 0 || (size_t)count >= sizeof(other_mp4))
            return -1;
        jpg_absent = path_absent(other_jpg);
        mp4_absent = path_absent(other_mp4);
        if (jpg_absent < 0 || mp4_absent < 0)
            return -1;
        if (jpg_absent != 0 && mp4_absent != 0) {
            available = 1;
            break;
        }
    }
    if (!available)
        return -1;
    count = snprintf(basename, basename_size, "V%07u.%s",
                     candidate, extension);
    if (count <= 0 || (size_t)count >= basename_size)
        return -1;
    count = snprintf(final_path, final_size, "%s/%s", media, basename);
    if (count <= 0 || (size_t)count >= final_size)
        return -1;
    count = snprintf(temporary, temporary_size, "%s/.%s.ngcd-%ld.tmp",
                     media, basename, (long)getpid());
    if (count <= 0 || (size_t)count >= temporary_size ||
        path_absent(temporary) != 1)
        return -1;
    *sequence = candidate;
    return 0;
}

static int elapsed_nanoseconds(const struct timespec *start,
                               const struct timespec *end,
                               uint64_t *elapsed)
{
    int64_t seconds;
    int64_t nanoseconds;
    if (start == NULL || end == NULL || elapsed == NULL)
        return -1;
    seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000LL;
    }
    if (seconds < 0 || (uint64_t)seconds > UINT64_MAX / UINT64_C(1000000000))
        return -1;
    *elapsed = (uint64_t)seconds * UINT64_C(1000000000) +
               (uint64_t)nanoseconds;
    return *elapsed == 0 ? -1 : 0;
}

int ngcd_storage_io_test_file(const char *path, int block_kb, int count,
                              uint64_t available_bytes,
                              int *kilobytes_per_second)
{
    FILE *stream = NULL;
    unsigned char *buffer = NULL;
    struct timespec start;
    struct timespec end;
    uint64_t block_bytes;
    uint64_t total_bytes;
    uint64_t elapsed;
    uint64_t rate;
    int created = 0;
    int descriptor;
    int success = 0;
    int index;
    if (path == NULL || path[0] == '\0' || kilobytes_per_second == NULL ||
        block_kb <= 0 || block_kb > IO_TEST_BLOCK_KB_MAX || count <= 0)
        return -1;
    *kilobytes_per_second = 0;
    block_bytes = (uint64_t)(unsigned)block_kb * 1024U;
    if ((uint64_t)(unsigned)count > UINT64_MAX / block_bytes)
        return -1;
    total_bytes = block_bytes * (uint64_t)(unsigned)count;
    if (total_bytes > IO_TEST_BYTES_MAX ||
        available_bytes < IO_TEST_FREE_RESERVE ||
        total_bytes > available_bytes - IO_TEST_FREE_RESERVE)
        return -1;
    buffer = malloc((size_t)block_bytes);
    if (buffer == NULL)
        return -1;
    memset(buffer, 0xa5, (size_t)block_bytes);
    stream = fopen(path, "wbx");
    if (stream == NULL)
        goto cleanup;
    created = 1;
    if (unlink(path) != 0)
        goto cleanup;
    created = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        goto cleanup;
    for (index = 0; index < count; ++index)
        if (fwrite(buffer, (size_t)block_bytes, 1, stream) != 1)
            goto cleanup;
    descriptor = fileno(stream);
    if (fflush(stream) != 0 || descriptor < 0 ||
        fdatasync(descriptor) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0 ||
        elapsed_nanoseconds(&start, &end, &elapsed) != 0)
        goto cleanup;
    rate = (total_bytes / 1024U) * UINT64_C(1000000000) / elapsed;
    if (rate == 0)
        rate = 1;
    *kilobytes_per_second = rate > (uint64_t)INT_MAX ? INT_MAX : (int)rate;
    success = 1;

cleanup:
    if (stream != NULL && fclose(stream) != 0)
        success = 0;
    if (created && unlink(path) != 0)
        success = 0;
    free(buffer);
    if (!success)
        *kilobytes_per_second = 0;
    return success ? 0 : -1;
}
