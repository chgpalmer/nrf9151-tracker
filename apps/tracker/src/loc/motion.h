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

/* Feed one fix-valid position. Never call with an unfixed frame's lat/lon. */
void motion_note_gps(double lat_deg, double lon_deg, int64_t now_ms);

/* Feed the serving cell ID whenever one is read. */
void motion_note_cell(uint32_t cell_id, int64_t now_ms);

/* The verdict. False until evidence of ENTRY_S of stillness accumulates. */
bool motion_stationary(int64_t now_ms);

#endif /* MOTION_H__ */
