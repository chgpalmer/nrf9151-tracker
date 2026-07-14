/*
 * Stationary verdict: is the device parked?
 *
 * Pure and clock-as-parameter, like loc_fsm. Two evidence sources, each
 * judged on its own terms, never mixed:
 *   GPS   - fixes stay within TRACKER_MOTION_RADIUS_M of a parked centroid
 *           for TRACKER_MOTION_ENTRY_S. One fix outside the radius exits
 *           immediately (waking is cheap; missing a theft is not).
 *   cell  - serving cell stays within the set of recently seen cells (a
 *           small LRU, because reselection ping-pongs between neighbours
 *           at cell boundaries while perfectly stationary).
 * The GPS verdict is authoritative while fixes are fresh; the cell verdict
 * takes over when they stop (CELL_LOOP/REST produce no fixes).
 *
 * This is the seam an IMU slots into later: same four functions, an
 * accelerometer replaces the internals, and wake-on-motion replaces the
 * check-interval detection lag.
 */
#ifndef MOTION_H__
#define MOTION_H__

#include <stdbool.h>
#include <stdint.h>

void motion_init(int64_t now_ms);

/* Feed one fix-valid position with its reported accuracy (metres). Never
 * call with an unfixed frame's lat/lon. Accuracy gates the EXIT: a fix 30 m
 * away with 50 m accuracy is not evidence of motion (overnight parked data:
 * 17 of 18 quiescent wakes were indoor multipath jumps). */
void motion_note_gps(double lat_deg, double lon_deg, double acc_m,
		     int64_t now_ms);

/* Feed the serving cell ID whenever one is read. */
void motion_note_cell(uint32_t cell_id, int64_t now_ms);

/* An IMU any-motion interrupt: physical evidence of movement, and the ONLY
 * thing that wakes a quiescent tracker. It breaks both dwell verdicts
 * instantly. The IMU is deliberately wake-only — GNSS still decides when to
 * sleep (an accelerometer cannot see constant velocity; a smooth motorway
 * cruise is inertially silent), so a stopped tracker re-parks on the usual
 * GPS dwell, and a false wake re-parks in ~60 s via the return shortcut. */
void motion_note_imu(int64_t now_ms);

/* The verdict. False until evidence of ENTRY_S of stillness accumulates. */
bool motion_stationary(int64_t now_ms);

#endif /* MOTION_H__ */
