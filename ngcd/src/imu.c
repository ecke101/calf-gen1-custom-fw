#include "ngcd_imu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IMU_DEFAULT_ROOT "/sys/bus/iio/devices"
#define IMU_DEFAULT_CALIBRATION "/param/imu_calib.yaml"
#define IMU_CAMM_QUEUE_CAPACITY 4096U
#define IMU_GYRO_RATE "208.000\n"
#define IMU_ACCEL_RATE "104.000\n"
#define IMU_MIN_CALIBRATION_SAMPLES 5
#define IMU_MAX_CALIBRATION_SAMPLES 10000
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define LSM6DSM_WHO_AM_I 0x0fU
#define LSM6DSM_WHO_AM_I_VALUE 0x6aU
#define LSM6DSM_CTRL1_XL 0x10U
#define LSM6DSM_CTRL2_G 0x11U
#define LSM6DSM_OUTX_L_G 0x22U
#define LSM6DSM_ACCEL_ODR_104HZ 0x40U
#define LSM6DSM_GYRO_ODR_208HZ 0x50U
#define NGCD_I2C_M_RD 0x0001U
#define NGCD_I2C_SLAVE_FORCE 0x0706UL
#define NGCD_I2C_RDWR 0x0707UL

struct ngcd_i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    unsigned char *buf;
};

struct ngcd_i2c_rdwr_ioctl_data {
    struct ngcd_i2c_msg *msgs;
    uint32_t nmsgs;
};

struct ngcd_imu {
    pthread_mutex_t mutex;
    pthread_t gyro_thread;
    pthread_t accel_thread;
    bool gyro_thread_started;
    bool accel_thread_started;
    bool stopping;
    int i2c_fd;
    unsigned short i2c_address;
    bool direct_i2c;
    unsigned char saved_ctrl1_xl;
    unsigned char saved_ctrl2_g;
    bool restore_control_registers;
    int gyro_fd[3];
    int accel_fd[3];
    double gyro_radians_per_count;
    double accel_meters_per_second2_per_count;
    double gyro_units_per_count;
    double accel_units_per_count;
    uint64_t gyro_interval_ns;
    uint64_t accel_interval_ns;
    struct ngcd_imu_sample latest;
    bool gyro_valid;
    bool accel_valid;
    int calibration_state;
    int calibration_target;
    int calibration_used;
    int64_t calibration_sum[3];
    bool calibration_save;
    int bias[6];
    struct ngcd_camm_gyro_sample queue[IMU_CAMM_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_count;
    char calibration_path[NGCD_PATH_MAX];
    char gyro_frequency_path[NGCD_PATH_MAX];
    char saved_gyro_frequency[32];
    bool restore_gyro_frequency;
    char accel_frequency_path[NGCD_PATH_MAX];
    char saved_accel_frequency[32];
    bool restore_accel_frequency;
};

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0U;
    return (uint64_t)now.tv_sec * NANOSECONDS_PER_SECOND +
           (uint64_t)now.tv_nsec;
}

static int path_join(char *output, size_t size, const char *first,
                     const char *second)
{
    int count = snprintf(output, size, "%s/%s", first, second);
    return count > 0 && (size_t)count < size ? 0 : -1;
}

static int read_text(const char *path, char *output, size_t size)
{
    int descriptor;
    ssize_t count;
    if (output == NULL || size < 2U)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    count = read(descriptor, output, size - 1U);
    if (close(descriptor) != 0 || count <= 0 || (size_t)count >= size)
        return -1;
    output[count] = '\0';
    return 0;
}

static int write_text(const char *path, const char *text)
{
    int descriptor;
    size_t length = strlen(text);
    size_t used = 0U;
    descriptor = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    while (used < length) {
        ssize_t count = write(descriptor, text + used, length - used);
        if (count <= 0) {
            (void)close(descriptor);
            return -1;
        }
        used += (size_t)count;
    }
    return close(descriptor) == 0 ? 0 : -1;
}

static int parse_double_file(const char *path, double *value)
{
    char text[64];
    char *end = NULL;
    double parsed;
    if (read_text(path, text, sizeof(text)) != 0)
        return -1;
    errno = 0;
    parsed = strtod(text, &end);
    while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' ||
                           *end == '\n'))
        ++end;
    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        !(parsed > 0.0))
        return -1;
    *value = parsed;
    return 0;
}

static int open_axis_files(const char *directory, const char *kind,
                           int descriptors[3])
{
    static const char *const axes[] = {"x", "y", "z"};
    size_t index;
    for (index = 0U; index < 3U; ++index) {
        char name[48];
        char path[NGCD_PATH_MAX];
        int count = snprintf(name, sizeof(name), "in_%s_%s_raw", kind,
                             axes[index]);
        if (count <= 0 || (size_t)count >= sizeof(name) ||
            path_join(path, sizeof(path), directory, name) != 0)
            goto fail;
        descriptors[index] = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptors[index] < 0)
            goto fail;
    }
    return 0;

fail:
    for (index = 0U; index < 3U; ++index) {
        if (descriptors[index] >= 0)
            (void)close(descriptors[index]);
        descriptors[index] = -1;
    }
    return -1;
}

static int discover_devices(const char *root, char *gyro, size_t gyro_size,
                            char *accel, size_t accel_size)
{
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (directory == NULL)
        return -1;
    gyro[0] = '\0';
    accel[0] = '\0';
    while ((entry = readdir(directory)) != NULL) {
        char device[NGCD_PATH_MAX];
        char name_path[NGCD_PATH_MAX];
        char name[64];
        size_t length;
        if (strncmp(entry->d_name, "iio:device", 10U) != 0 ||
            path_join(device, sizeof(device), root, entry->d_name) != 0 ||
            path_join(name_path, sizeof(name_path), device, "name") != 0 ||
            read_text(name_path, name, sizeof(name)) != 0)
            continue;
        length = strcspn(name, "\r\n");
        name[length] = '\0';
        if (strcmp(name, "lsm6dsm_gyro") == 0 && gyro[0] == '\0') {
            if (strlen(device) >= gyro_size)
                break;
            memcpy(gyro, device, strlen(device) + 1U);
        } else if (strcmp(name, "lsm6dsm_accel") == 0 &&
                   accel[0] == '\0') {
            if (strlen(device) >= accel_size)
                break;
            memcpy(accel, device, strlen(device) + 1U);
        }
    }
    (void)closedir(directory);
    return gyro[0] != '\0' && accel[0] != '\0' ? 0 : -1;
}

static int read_axis(int descriptor, int *value)
{
    char text[32];
    char *end = NULL;
    long parsed;
    ssize_t count;
    if (lseek(descriptor, 0, SEEK_SET) < 0)
        return -1;
    count = read(descriptor, text, sizeof(text) - 1U);
    if (count <= 0 || (size_t)count >= sizeof(text))
        return -1;
    text[count] = '\0';
    errno = 0;
    parsed = strtol(text, &end, 10);
    while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' ||
                           *end == '\n'))
        ++end;
    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int read_axes(int descriptors[3], int values[3], uint64_t *timestamp)
{
    uint64_t before = monotonic_nanoseconds();
    uint64_t after;
    if (before == 0U || read_axis(descriptors[0], &values[0]) != 0 ||
        read_axis(descriptors[1], &values[1]) != 0 ||
        read_axis(descriptors[2], &values[2]) != 0)
        return -1;
    after = monotonic_nanoseconds();
    if (after == 0U || after < before)
        return -1;
    *timestamp = before + (after - before) / 2U;
    return 0;
}

static int i2c_read_registers(int descriptor, unsigned short address,
                              unsigned char first, unsigned char *data,
                              size_t size)
{
    struct ngcd_i2c_msg messages[2];
    struct ngcd_i2c_rdwr_ioctl_data transfer;
    if (descriptor < 0 || data == NULL || size == 0U || size > UINT16_MAX)
        return -1;
    memset(messages, 0, sizeof(messages));
    messages[0].addr = address;
    messages[0].len = 1U;
    messages[0].buf = &first;
    messages[1].addr = address;
    messages[1].flags = NGCD_I2C_M_RD;
    messages[1].len = (unsigned short)size;
    messages[1].buf = data;
    transfer.msgs = messages;
    transfer.nmsgs = 2U;
    return ioctl(descriptor, NGCD_I2C_RDWR, &transfer) == 2 ? 0 : -1;
}

static int i2c_write_register(int descriptor, unsigned short address,
                              unsigned char reg, unsigned char value)
{
    unsigned char data[2] = {reg, value};
    struct ngcd_i2c_msg message;
    struct ngcd_i2c_rdwr_ioctl_data transfer;
    if (descriptor < 0)
        return -1;
    memset(&message, 0, sizeof(message));
    message.addr = address;
    message.len = sizeof(data);
    message.buf = data;
    transfer.msgs = &message;
    transfer.nmsgs = 1U;
    return ioctl(descriptor, NGCD_I2C_RDWR, &transfer) == 1 ? 0 : -1;
}

static int discover_i2c_endpoint(const char *device, char *path,
                                 size_t path_size,
                                 unsigned short *address)
{
    char resolved[NGCD_PATH_MAX];
    char *i2c;
    char *client;
    char *separator;
    unsigned int bus = 0U;
    unsigned int client_bus = 0U;
    unsigned int parsed_address = 0U;
    const char *cursor;
    int count;
    ssize_t resolved_size = readlink(device, resolved, sizeof(resolved) - 1U);
    if (resolved_size <= 0 || (size_t)resolved_size >= sizeof(resolved))
        return -1;
    resolved[resolved_size] = '\0';
    i2c = strstr(resolved, "/i2c-");
    if (i2c == NULL)
        return -1;
    cursor = i2c + 5;
    if (*cursor < '0' || *cursor > '9')
        return -1;
    while (*cursor >= '0' && *cursor <= '9') {
        if (bus > 999U)
            return -1;
        bus = bus * 10U + (unsigned int)(*cursor - '0');
        ++cursor;
    }
    if (*cursor != '/')
        return -1;
    client = (char *)cursor + 1;
    cursor = client;
    if (*cursor < '0' || *cursor > '9')
        return -1;
    while (*cursor >= '0' && *cursor <= '9') {
        if (client_bus > 999U)
            return -1;
        client_bus = client_bus * 10U + (unsigned int)(*cursor - '0');
        ++cursor;
    }
    separator = (char *)cursor;
    if (separator == client || *separator != '-' || client_bus != bus)
        return -1;
    cursor = separator + 1;
    if (!((*cursor >= '0' && *cursor <= '9') ||
          (*cursor >= 'a' && *cursor <= 'f') ||
          (*cursor >= 'A' && *cursor <= 'F')))
        return -1;
    while ((*cursor >= '0' && *cursor <= '9') ||
           (*cursor >= 'a' && *cursor <= 'f') ||
           (*cursor >= 'A' && *cursor <= 'F')) {
        unsigned int digit =
            *cursor <= '9' ? (unsigned int)(*cursor - '0')
                           : (unsigned int)((*cursor | 0x20) - 'a' + 10);
        if (parsed_address > 0x7fU / 16U)
            return -1;
        parsed_address = parsed_address * 16U + digit;
        ++cursor;
    }
    if (*cursor != '/' || parsed_address > 0x7fU)
        return -1;
    count = snprintf(path, path_size, "/dev/i2c-%u", bus);
    if (count <= 0 || (size_t)count >= path_size)
        return -1;
    *address = (unsigned short)parsed_address;
    return 0;
}

static int setup_direct_i2c(struct ngcd_imu *imu, const char *gyro)
{
    char path[64];
    unsigned char identity;
    unsigned char ctrl1;
    unsigned char ctrl2;
    if (discover_i2c_endpoint(gyro, path, sizeof(path),
                              &imu->i2c_address) != 0)
        return -1;
    imu->i2c_fd = open(path, O_RDWR | O_CLOEXEC);
    if (imu->i2c_fd < 0 ||
        ioctl(imu->i2c_fd, NGCD_I2C_SLAVE_FORCE, imu->i2c_address) != 0 ||
        i2c_read_registers(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_WHO_AM_I, &identity, 1U) != 0 ||
        identity != LSM6DSM_WHO_AM_I_VALUE ||
        i2c_read_registers(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_CTRL1_XL, &ctrl1, 1U) != 0 ||
        i2c_read_registers(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_CTRL2_G, &ctrl2, 1U) != 0)
        goto fail;
    imu->saved_ctrl1_xl = ctrl1;
    imu->saved_ctrl2_g = ctrl2;
    if (i2c_write_register(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_CTRL1_XL,
                           (unsigned char)((ctrl1 & 0x0fU) |
                                           LSM6DSM_ACCEL_ODR_104HZ)) != 0)
        goto fail;
    if (i2c_write_register(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_CTRL2_G,
                           (unsigned char)((ctrl2 & 0x0fU) |
                                           LSM6DSM_GYRO_ODR_208HZ)) != 0) {
        (void)i2c_write_register(imu->i2c_fd, imu->i2c_address,
                                 LSM6DSM_CTRL1_XL, ctrl1);
        goto fail;
    }
    imu->restore_control_registers = true;
    imu->direct_i2c = true;
    return 0;

fail:
    if (imu->i2c_fd >= 0)
        (void)close(imu->i2c_fd);
    imu->i2c_fd = -1;
    return -1;
}

static int signed_word(const unsigned char *data)
{
    uint16_t value = (uint16_t)data[0] | (uint16_t)data[1] << 8U;
    return value < 0x8000U ? (int)value : (int)value - 0x10000;
}

static int read_direct_axes(struct ngcd_imu *imu, int gyro[3], int accel[3],
                            uint64_t *timestamp)
{
    unsigned char data[12];
    uint64_t before = monotonic_nanoseconds();
    uint64_t after;
    if (before == 0U ||
        i2c_read_registers(imu->i2c_fd, imu->i2c_address,
                           LSM6DSM_OUTX_L_G, data, sizeof(data)) != 0)
        return -1;
    after = monotonic_nanoseconds();
    if (after == 0U || after < before)
        return -1;
    gyro[0] = signed_word(data);
    gyro[1] = signed_word(data + 2U);
    gyro[2] = signed_word(data + 4U);
    accel[0] = signed_word(data + 6U);
    accel[1] = signed_word(data + 8U);
    accel[2] = signed_word(data + 10U);
    *timestamp = before + (after - before) / 2U;
    return 0;
}

static int scaled_integer(int raw, double units_per_count)
{
    double value = (double)raw * units_per_count;
    if (value > (double)INT_MAX)
        return INT_MAX;
    if (value < (double)INT_MIN)
        return INT_MIN;
    return (int)value;
}

static int subtract_integer(int value, int bias)
{
    int64_t corrected = (int64_t)value - (int64_t)bias;
    if (corrected > INT_MAX)
        return INT_MAX;
    if (corrected < INT_MIN)
        return INT_MIN;
    return (int)corrected;
}

static int load_calibration(struct ngcd_imu *imu)
{
    static const char *const keys[] = {
        "gyro_data0", "gyro_data1", "gyro_data2",
        "acc_data0", "acc_data1", "acc_data2",
    };
    FILE *file = fopen(imu->calibration_path, "r");
    bool found[6] = {false, false, false, false, false, false};
    int bias[6] = {0, 0, 0, 0, 0, 0};
    char line[96];
    size_t index;
    if (file == NULL)
        return -1;
    while (fgets(line, sizeof(line), file) != NULL) {
        char key[32];
        int value;
        if (sscanf(line, "%31[^:]: %d", key, &value) != 2)
            continue;
        for (index = 0U; index < 6U; ++index) {
            if (strcmp(key, keys[index]) == 0) {
                bias[index] = value;
                found[index] = true;
                break;
            }
        }
    }
    if (fclose(file) != 0)
        return -1;
    for (index = 0U; index < 6U; ++index)
        if (!found[index])
            return -1;
    memcpy(imu->bias, bias, sizeof(bias));
    return 0;
}

static int save_calibration(struct ngcd_imu *imu)
{
    char temporary[NGCD_PATH_MAX];
    char text[256];
    int bias[6];
    int descriptor;
    int count;
    size_t used = 0U;
    pthread_mutex_lock(&imu->mutex);
    memcpy(bias, imu->bias, sizeof(bias));
    pthread_mutex_unlock(&imu->mutex);
    count = snprintf(temporary, sizeof(temporary), "%s.ngcd-%ld.tmp",
                     imu->calibration_path, (long)getpid());
    if (count <= 0 || (size_t)count >= sizeof(temporary))
        return -1;
    count = snprintf(text, sizeof(text),
                     "gyro_data0: %d\ngyro_data1: %d\ngyro_data2: %d\n"
                     "acc_data0: %d\nacc_data1: %d\nacc_data2: %d",
                     bias[0], bias[1], bias[2], bias[3], bias[4], bias[5]);
    if (count <= 0 || (size_t)count >= sizeof(text))
        return -1;
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                     O_NOFOLLOW,
                      0644);
    if (descriptor < 0)
        return -1;
    while (used < (size_t)count) {
        ssize_t written = write(descriptor, text + used, (size_t)count - used);
        if (written <= 0)
            goto fail;
        used += (size_t)written;
    }
    if (fsync(descriptor) != 0)
        goto fail;
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto fail;
    }
    descriptor = -1;
    if (rename(temporary, imu->calibration_path) != 0)
        goto fail;
    return 0;

fail:
    if (descriptor >= 0)
        (void)close(descriptor);
    (void)unlink(temporary);
    return -1;
}

static void queue_gyro(struct ngcd_imu *imu, uint64_t timestamp,
                       const int corrected[3])
{
    static const double radians_per_millidegree =
        0.00001745329251994329577;
    size_t index;
    if (imu->queue_count == IMU_CAMM_QUEUE_CAPACITY) {
        imu->queue_head = (imu->queue_head + 1U) % IMU_CAMM_QUEUE_CAPACITY;
        --imu->queue_count;
    }
    index = (imu->queue_head + imu->queue_count) % IMU_CAMM_QUEUE_CAPACITY;
    imu->queue[index].monotonic_ns = timestamp;
    imu->queue[index].x_radians_per_second =
        (float)((double)corrected[0] * radians_per_millidegree);
    imu->queue[index].y_radians_per_second =
        (float)((double)corrected[1] * radians_per_millidegree);
    imu->queue[index].z_radians_per_second =
        (float)((double)corrected[2] * radians_per_millidegree);
    ++imu->queue_count;
}

static void publish_gyro(struct ngcd_imu *imu, const int raw[3],
                         uint64_t timestamp)
{
    int transformed[3];
    int corrected[3];
    bool save = false;
    size_t index;
    transformed[0] = scaled_integer(raw[1], imu->gyro_units_per_count);
    transformed[1] = scaled_integer(raw[0], imu->gyro_units_per_count);
    transformed[2] = scaled_integer(-raw[2], imu->gyro_units_per_count);
    pthread_mutex_lock(&imu->mutex);
    if (imu->calibration_state == 1)
        imu->calibration_state = 2;
    if (imu->calibration_state == 2) {
        for (index = 0U; index < 3U; ++index)
            imu->calibration_sum[index] += transformed[index];
        ++imu->calibration_used;
        if (imu->calibration_used >= imu->calibration_target) {
            for (index = 0U; index < 3U; ++index)
                imu->bias[index] =
                    (int)(imu->calibration_sum[index] /
                          imu->calibration_used);
            imu->calibration_state = 3;
            save = imu->calibration_save;
        }
    }
    for (index = 0U; index < 3U; ++index)
        corrected[index] = subtract_integer(transformed[index],
                                             imu->bias[index]);
    imu->latest.gyro_x = corrected[0];
    imu->latest.gyro_y = corrected[1];
    imu->latest.gyro_z = corrected[2];
    if (timestamp > imu->latest.monotonic_ns)
        imu->latest.monotonic_ns = timestamp;
    imu->gyro_valid = true;
    queue_gyro(imu, timestamp, corrected);
    pthread_mutex_unlock(&imu->mutex);
    if (save && save_calibration(imu) != 0)
        fprintf(stderr, "ngcd: could not save IMU calibration to %s\n",
                imu->calibration_path);
}

static void publish_accel(struct ngcd_imu *imu, const int raw[3],
                          uint64_t timestamp)
{
    int transformed[3];
    transformed[0] = scaled_integer(raw[1], imu->accel_units_per_count);
    transformed[1] = scaled_integer(raw[0], imu->accel_units_per_count);
    transformed[2] = scaled_integer(-raw[2], imu->accel_units_per_count);
    pthread_mutex_lock(&imu->mutex);
    imu->latest.acceleration_x =
        subtract_integer(transformed[0], imu->bias[3]);
    imu->latest.acceleration_y =
        subtract_integer(transformed[1], imu->bias[4]);
    imu->latest.acceleration_z =
        subtract_integer(transformed[2], imu->bias[5]);
    if (timestamp > imu->latest.monotonic_ns)
        imu->latest.monotonic_ns = timestamp;
    imu->accel_valid = true;
    pthread_mutex_unlock(&imu->mutex);
}

static void sleep_until(uint64_t target)
{
    uint64_t now = monotonic_nanoseconds();
    struct timespec delay;
    if (now == 0U || target <= now)
        return;
    delay.tv_sec = (long)((target - now) / NANOSECONDS_PER_SECOND);
    delay.tv_nsec = (long)((target - now) % NANOSECONDS_PER_SECOND);
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static void advance_deadline(uint64_t *deadline, uint64_t interval,
                             uint64_t now)
{
    do {
        *deadline += interval;
    } while (*deadline <= now);
}

static bool sampler_stopping(struct ngcd_imu *imu)
{
    bool stopping;
    pthread_mutex_lock(&imu->mutex);
    stopping = imu->stopping;
    pthread_mutex_unlock(&imu->mutex);
    return stopping;
}

static void *gyro_sampler_thread(void *argument)
{
    struct ngcd_imu *imu = argument;
    uint64_t next_sample = monotonic_nanoseconds();
    for (;;) {
        uint64_t now;
        int raw[3];
        uint64_t timestamp;
        if (sampler_stopping(imu))
            break;
        now = monotonic_nanoseconds();
        if (now == 0U)
            break;
        if (now >= next_sample) {
            if (imu->direct_i2c) {
                int accel[3];
                if (read_direct_axes(imu, raw, accel, &timestamp) == 0) {
                    publish_gyro(imu, raw, timestamp);
                    publish_accel(imu, accel, timestamp);
                }
            } else if (read_axes(imu->gyro_fd, raw, &timestamp) == 0) {
                publish_gyro(imu, raw, timestamp);
            }
            advance_deadline(&next_sample, imu->gyro_interval_ns, now);
        }
        sleep_until(next_sample);
    }
    return NULL;
}

static void *accel_sampler_thread(void *argument)
{
    struct ngcd_imu *imu = argument;
    uint64_t next_sample = monotonic_nanoseconds();
    for (;;) {
        uint64_t now;
        int raw[3];
        uint64_t timestamp;
        if (sampler_stopping(imu))
            break;
        now = monotonic_nanoseconds();
        if (now == 0U)
            break;
        if (now >= next_sample) {
            if (read_axes(imu->accel_fd, raw, &timestamp) == 0)
                publish_accel(imu, raw, timestamp);
            advance_deadline(&next_sample, imu->accel_interval_ns, now);
        }
        sleep_until(next_sample);
    }
    return NULL;
}

static void close_descriptors(int descriptors[3])
{
    size_t index;
    for (index = 0U; index < 3U; ++index) {
        if (descriptors[index] >= 0)
            (void)close(descriptors[index]);
        descriptors[index] = -1;
    }
}

int ngcd_imu_open_at(struct ngcd_imu **output, const char *root,
                     const char *calibration_path)
{
    static const double radians_to_millidegrees =
        57295.7795130823208768;
    static const double meters_per_second2_to_milligravity =
        101.9716212977928243;
    struct ngcd_imu *imu;
    char gyro[NGCD_PATH_MAX];
    char accel[NGCD_PATH_MAX];
    char path[NGCD_PATH_MAX];
    double gyro_frequency;
    double accel_frequency;
    size_t index;
    if (output == NULL || root == NULL || calibration_path == NULL ||
        strlen(calibration_path) >= NGCD_PATH_MAX)
        return -1;
    *output = NULL;
    imu = calloc(1U, sizeof(*imu));
    if (imu == NULL)
        return -1;
    imu->i2c_fd = -1;
    for (index = 0U; index < 3U; ++index) {
        imu->gyro_fd[index] = -1;
        imu->accel_fd[index] = -1;
    }
    memcpy(imu->calibration_path, calibration_path,
           strlen(calibration_path) + 1U);
    if (pthread_mutex_init(&imu->mutex, NULL) != 0)
        goto fail;
    if (discover_devices(root, gyro, sizeof(gyro), accel, sizeof(accel)) != 0 ||
        open_axis_files(gyro, "anglvel", imu->gyro_fd) != 0 ||
        open_axis_files(accel, "accel", imu->accel_fd) != 0)
        goto fail_mutex;
    if (path_join(path, sizeof(path), gyro, "in_anglvel_scale") != 0 ||
        parse_double_file(path, &imu->gyro_radians_per_count) != 0 ||
        path_join(path, sizeof(path), accel, "in_accel_scale") != 0 ||
        parse_double_file(path,
                          &imu->accel_meters_per_second2_per_count) != 0)
        goto fail_mutex;
    /* The vendor 2.1.6 LSM6DSM driver reports the 245 dps scale while the
     * chip is configured for 125 dps. Stock calibration data and stationary
     * live samples independently confirm the effective half-scale. */
    if (imu->gyro_radians_per_count > 0.00014 &&
        imu->gyro_radians_per_count < 0.00017)
        imu->gyro_radians_per_count *= 0.5;
    imu->gyro_units_per_count =
        imu->gyro_radians_per_count * radians_to_millidegrees;
    imu->accel_units_per_count =
        imu->accel_meters_per_second2_per_count *
        meters_per_second2_to_milligravity;
    if (path_join(imu->gyro_frequency_path,
                  sizeof(imu->gyro_frequency_path), gyro,
                  "sampling_frequency") != 0 ||
        path_join(imu->accel_frequency_path,
                  sizeof(imu->accel_frequency_path), accel,
                  "sampling_frequency") != 0)
        goto fail_mutex;
    if (read_text(imu->gyro_frequency_path, imu->saved_gyro_frequency,
                  sizeof(imu->saved_gyro_frequency)) == 0 &&
        write_text(imu->gyro_frequency_path, IMU_GYRO_RATE) == 0)
        imu->restore_gyro_frequency = true;
    if (read_text(imu->accel_frequency_path, imu->saved_accel_frequency,
                  sizeof(imu->saved_accel_frequency)) == 0 &&
        write_text(imu->accel_frequency_path, IMU_ACCEL_RATE) == 0)
        imu->restore_accel_frequency = true;
    if (parse_double_file(imu->gyro_frequency_path, &gyro_frequency) != 0 ||
        parse_double_file(imu->accel_frequency_path, &accel_frequency) != 0)
        goto fail_mutex;
    if (gyro_frequency < 1.0 || gyro_frequency > 1000.0 ||
        accel_frequency < 1.0 || accel_frequency > 1000.0)
        goto fail_mutex;
    imu->gyro_interval_ns =
        (uint64_t)((double)NANOSECONDS_PER_SECOND / gyro_frequency);
    imu->accel_interval_ns =
        (uint64_t)((double)NANOSECONDS_PER_SECOND / accel_frequency);
    if (imu->gyro_interval_ns == 0U || imu->accel_interval_ns == 0U)
        goto fail_mutex;
    (void)setup_direct_i2c(imu, gyro);
    (void)load_calibration(imu);
    if (!imu->direct_i2c) {
        if (pthread_create(&imu->accel_thread, NULL, accel_sampler_thread,
                           imu) != 0)
            goto fail_mutex;
        imu->accel_thread_started = true;
    }
    if (pthread_create(&imu->gyro_thread, NULL, gyro_sampler_thread, imu) != 0) {
        pthread_mutex_lock(&imu->mutex);
        imu->stopping = true;
        pthread_mutex_unlock(&imu->mutex);
        if (imu->accel_thread_started) {
            (void)pthread_join(imu->accel_thread, NULL);
            imu->accel_thread_started = false;
        }
        goto fail_mutex;
    }
    imu->gyro_thread_started = true;
    *output = imu;
    return 0;

fail_mutex:
    if (imu->restore_control_registers) {
        (void)i2c_write_register(imu->i2c_fd, imu->i2c_address,
                                 LSM6DSM_CTRL2_G, imu->saved_ctrl2_g);
        (void)i2c_write_register(imu->i2c_fd, imu->i2c_address,
                                 LSM6DSM_CTRL1_XL, imu->saved_ctrl1_xl);
    }
    if (imu->i2c_fd >= 0)
        (void)close(imu->i2c_fd);
    if (imu->restore_gyro_frequency)
        (void)write_text(imu->gyro_frequency_path,
                         imu->saved_gyro_frequency);
    if (imu->restore_accel_frequency)
        (void)write_text(imu->accel_frequency_path,
                         imu->saved_accel_frequency);
    close_descriptors(imu->gyro_fd);
    close_descriptors(imu->accel_fd);
    (void)pthread_mutex_destroy(&imu->mutex);
    free(imu);
    return -1;

fail:
    close_descriptors(imu->gyro_fd);
    close_descriptors(imu->accel_fd);
    free(imu);
    return -1;
}

int ngcd_imu_open(struct ngcd_imu **imu)
{
    return ngcd_imu_open_at(imu, IMU_DEFAULT_ROOT, IMU_DEFAULT_CALIBRATION);
}

void ngcd_imu_close(struct ngcd_imu *imu)
{
    if (imu == NULL)
        return;
    pthread_mutex_lock(&imu->mutex);
    imu->stopping = true;
    pthread_mutex_unlock(&imu->mutex);
    if (imu->gyro_thread_started)
        (void)pthread_join(imu->gyro_thread, NULL);
    if (imu->accel_thread_started)
        (void)pthread_join(imu->accel_thread, NULL);
    close_descriptors(imu->gyro_fd);
    close_descriptors(imu->accel_fd);
    if (imu->restore_control_registers) {
        (void)i2c_write_register(imu->i2c_fd, imu->i2c_address,
                                 LSM6DSM_CTRL2_G, imu->saved_ctrl2_g);
        (void)i2c_write_register(imu->i2c_fd, imu->i2c_address,
                                 LSM6DSM_CTRL1_XL, imu->saved_ctrl1_xl);
    }
    if (imu->i2c_fd >= 0)
        (void)close(imu->i2c_fd);
    if (imu->restore_gyro_frequency)
        (void)write_text(imu->gyro_frequency_path,
                         imu->saved_gyro_frequency);
    if (imu->restore_accel_frequency)
        (void)write_text(imu->accel_frequency_path,
                         imu->saved_accel_frequency);
    (void)pthread_mutex_destroy(&imu->mutex);
    free(imu);
}

int ngcd_imu_read(struct ngcd_imu *imu, struct ngcd_imu_sample *sample)
{
    int result;
    if (imu == NULL || sample == NULL)
        return -1;
    pthread_mutex_lock(&imu->mutex);
    result = imu->gyro_valid && imu->accel_valid ? 0 : -1;
    if (result == 0)
        *sample = imu->latest;
    pthread_mutex_unlock(&imu->mutex);
    return result;
}

int ngcd_imu_start_calibration(struct ngcd_imu *imu, int type, bool save,
                               int count)
{
    if (imu == NULL || type != 0 || count < IMU_MIN_CALIBRATION_SAMPLES ||
        count > IMU_MAX_CALIBRATION_SAMPLES)
        return -1;
    pthread_mutex_lock(&imu->mutex);
    if (imu->calibration_state == 1 || imu->calibration_state == 2) {
        pthread_mutex_unlock(&imu->mutex);
        return -1;
    }
    imu->calibration_state = 1;
    imu->calibration_target = count;
    imu->calibration_used = 0;
    memset(imu->calibration_sum, 0, sizeof(imu->calibration_sum));
    imu->calibration_save = save;
    pthread_mutex_unlock(&imu->mutex);
    return 0;
}

int ngcd_imu_calibration_state(struct ngcd_imu *imu, int *state)
{
    if (imu == NULL || state == NULL)
        return -1;
    pthread_mutex_lock(&imu->mutex);
    *state = imu->calibration_state;
    pthread_mutex_unlock(&imu->mutex);
    return 0;
}

int ngcd_imu_pop_camm_gyro(struct ngcd_imu *imu,
                           struct ngcd_camm_gyro_sample *sample)
{
    if (imu == NULL || sample == NULL)
        return -1;
    pthread_mutex_lock(&imu->mutex);
    if (imu->queue_count == 0U) {
        pthread_mutex_unlock(&imu->mutex);
        return 1;
    }
    *sample = imu->queue[imu->queue_head];
    imu->queue_head = (imu->queue_head + 1U) % IMU_CAMM_QUEUE_CAPACITY;
    --imu->queue_count;
    pthread_mutex_unlock(&imu->mutex);
    return 0;
}
