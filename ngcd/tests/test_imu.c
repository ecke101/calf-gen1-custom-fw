#include "ngcd_imu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void make_path(char *output, size_t size, const char *first,
                      const char *second)
{
    int count = snprintf(output, size, "%s/%s", first, second);
    assert(count > 0 && (size_t)count < size);
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static void write_member(const char *directory, const char *name,
                         const char *text)
{
    char path[512];
    make_path(path, sizeof(path), directory, name);
    write_file(path, text);
}

static void wait_milliseconds(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&delay, &delay) != 0)
        ;
}

static void remove_member(const char *directory, const char *name)
{
    char path[512];
    make_path(path, sizeof(path), directory, name);
    assert(unlink(path) == 0);
}

int main(void)
{
    char root[256];
    char gyro[512];
    char accel[512];
    char calibration[512];
    struct ngcd_imu *imu = NULL;
    struct ngcd_imu_sample sample;
    struct ngcd_camm_gyro_sample camm;
    int camm_count = 0;
    int state = -1;
    int count = snprintf(root, sizeof(root), "/tmp/calf-ngcd-imu-test-%ld",
                         (long)getpid());
    assert(count > 0 && (size_t)count < sizeof(root));
    make_path(gyro, sizeof(gyro), root, "iio:device1");
    make_path(accel, sizeof(accel), root, "iio:device2");
    make_path(calibration, sizeof(calibration), root, "imu_calib.yaml");
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(gyro, 0700) == 0);
    assert(mkdir(accel, 0700) == 0);
    write_member(gyro, "name", "lsm6dsm_gyro\n");
    write_member(gyro, "sampling_frequency", "12.5000\n");
    write_member(gyro, "in_anglvel_scale", "0.000152716\n");
    write_member(gyro, "in_anglvel_x_raw", "139\n");
    write_member(gyro, "in_anglvel_y_raw", "-219\n");
    write_member(gyro, "in_anglvel_z_raw", "13\n");
    write_member(accel, "name", "lsm6dsm_accel\n");
    write_member(accel, "sampling_frequency", "104.000\n");
    write_member(accel, "in_accel_scale", "0.000598205\n");
    write_member(accel, "in_accel_x_raw", "-102\n");
    write_member(accel, "in_accel_y_raw", "16409\n");
    write_member(accel, "in_accel_z_raw", "129\n");
    write_file(calibration,
               "gyro_data0: -1011\n"
               "gyro_data1: 726\n"
               "gyro_data2: -38\n"
               "acc_data0: 6\n"
               "acc_data1: 0\n"
               "acc_data2: 0");
    assert(ngcd_imu_open_at(&imu, root, calibration) == 0);
    wait_milliseconds(120L);
    assert(ngcd_imu_read(imu, &sample) == 0);
    assert(sample.gyro_x >= 51 && sample.gyro_x <= 54);
    assert(sample.gyro_y >= -120 && sample.gyro_y <= -117);
    assert(sample.gyro_z >= -20 && sample.gyro_z <= -17);
    assert(sample.acceleration_x >= 993 && sample.acceleration_x <= 995);
    assert(sample.acceleration_y == -6);
    assert(sample.acceleration_z == -7);
    assert(sample.monotonic_ns != 0U);
    assert(ngcd_imu_pop_camm_gyro(imu, &camm) == 0);
    ++camm_count;
    assert(camm.monotonic_ns != 0U);
    assert(camm.x_radians_per_second > 0.0008f &&
           camm.x_radians_per_second < 0.0010f);
    while (ngcd_imu_pop_camm_gyro(imu, &camm) == 0)
        ++camm_count;
    assert(camm_count >= 8);
    assert(ngcd_imu_calibration_state(imu, &state) == 0 && state == 0);
    assert(ngcd_imu_start_calibration(imu, 0, true, 5) == 0);
    assert(ngcd_imu_start_calibration(imu, 0, false, 5) != 0);
    wait_milliseconds(500L);
    assert(ngcd_imu_calibration_state(imu, &state) == 0 && state == 3);
    assert(ngcd_imu_read(imu, &sample) == 0);
    assert(sample.gyro_x == 0 && sample.gyro_y == 0 && sample.gyro_z == 0);
    ngcd_imu_close(imu);

    remove_member(gyro, "name");
    remove_member(gyro, "sampling_frequency");
    remove_member(gyro, "in_anglvel_scale");
    remove_member(gyro, "in_anglvel_x_raw");
    remove_member(gyro, "in_anglvel_y_raw");
    remove_member(gyro, "in_anglvel_z_raw");
    remove_member(accel, "name");
    remove_member(accel, "sampling_frequency");
    remove_member(accel, "in_accel_scale");
    remove_member(accel, "in_accel_x_raw");
    remove_member(accel, "in_accel_y_raw");
    remove_member(accel, "in_accel_z_raw");
    assert(unlink(calibration) == 0);
    assert(rmdir(gyro) == 0);
    assert(rmdir(accel) == 0);
    assert(rmdir(root) == 0);
    puts("ngcd IMU tests passed");
    return 0;
}
