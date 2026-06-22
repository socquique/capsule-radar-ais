#pragma once
// Minimal QMI8658 (6-axis IMU) driver over I2C. Device-only. We only use the
// accelerometer's Z axis to detect "face-down" (screen toward the ground).
bool imu_begin();      // init; false if the chip isn't found
int  imu_facedown();   // 1 = face-down, 0 = not, -1 = read unavailable (don't change state)
