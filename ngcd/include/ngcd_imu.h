#ifndef CALF_NGCD_IMU_H
#define CALF_NGCD_IMU_H

#include "ngcd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ngcd_imu;

struct ngcd_camm_gyro_sample {
    uint64_t monotonic_ns;
    float x_radians_per_second;
    float y_radians_per_second;
    float z_radians_per_second;
};

int ngcd_imu_open(struct ngcd_imu **imu);
int ngcd_imu_open_at(struct ngcd_imu **imu, const char *iio_root,
                     const char *calibration_path);
void ngcd_imu_close(struct ngcd_imu *imu);
int ngcd_imu_read(struct ngcd_imu *imu, struct ngcd_imu_sample *sample);
int ngcd_imu_start_calibration(struct ngcd_imu *imu, int type, bool save,
                               int count);
int ngcd_imu_calibration_state(struct ngcd_imu *imu, int *state);
int ngcd_imu_pop_camm_gyro(struct ngcd_imu *imu,
                           struct ngcd_camm_gyro_sample *sample);

#endif
