/*
 * IMU wake source: a LIS3DH any-motion interrupt, latched for the main loop.
 *
 * Boot picks the mode: imu_init() succeeding means the tracker trusts the
 * interrupt absolutely for parked departure (no runtime health checks —
 * decided 2026-07-14: the part is reliable silicon and its interrupt engine
 * runs standalone once configured; a wiring fault is a bench problem, not a
 * firmware contingency). init failing (chip absent: native_sim, no-IMU
 * builds) selects the legacy GPS-check parked behavior, untouched.
 */
#ifndef IMU_H__
#define IMU_H__

#include <stdbool.h>

/* Probe and configure the accelerometer (AN3308 wake-up: HP-filtered
 * any-motion on INT1). Returns true when present and armed. */
bool imu_init(void);

bool imu_present(void);

/* True once per motion event since the last call (latched in the trigger
 * callback; reading clears). Cheap enough for every main-loop pass. */
bool imu_take_motion(void);

#endif /* IMU_H__ */
