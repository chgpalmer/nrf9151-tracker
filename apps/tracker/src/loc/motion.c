#include "motion.h"

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(motion, CONFIG_TRACKER_LOG_LEVEL);

#define RADIUS_M CONFIG_TRACKER_MOTION_RADIUS_M
#define ENTRY_MS ((int64_t)CONFIG_TRACKER_MOTION_ENTRY_S * 1000)

/* How long a GPS verdict outranks the cell verdict. Fixes stop arriving in
 * CELL_LOOP/REST; after this long without one, cells are the best we have.
 * 10 min: longer than any quiescent check interval, shorter than a drive. */
#define GPS_HOLD_MS (10 * 60 * 1000)

/* Serving cells seen recently. 4 covers boundary ping-pong between towers;
 * a genuine journey burns through novel cells faster than it refills. */
#define CELL_LRU 4

/* GPS evidence: a parked centroid (the anchor) and how long fixes have
 * stayed inside its radius. Any out-of-radius fix re-anchors and restarts
 * the dwell clock, so creeping never accumulates dwell. */
static double  anchor_lat, anchor_lon;
static bool    anchor_valid;
static int64_t anchor_since_ms;
static bool    gps_stationary;
static int64_t last_gps_ms;

/* The last CONFIRMED parked spot. A false wake (multipath jump, brief walk
 * to the door) shouldn't cost a fresh 300 s dwell when fixes return to the
 * same place — measured: each such wake shipped ~300 points of 1 Hz jitter.
 * Returning within the radius for RETURN_MS restores stationary; wandering
 * far (a real departure) forgets the spot. */
#define RETURN_MS   60000
#define FORGET_MULT 4.0
static double  parked_lat, parked_lon;
static bool    parked_valid;
static bool    returning;
static int64_t return_since_ms;

/* Cell evidence: LRU of recent cell IDs + when the last NOVEL one appeared. */
static uint32_t cells[CELL_LRU];
static uint8_t  cell_count;
static int64_t  cell_calm_since_ms;
static bool     cell_seen;

void motion_init(int64_t now_ms)
{
	anchor_valid = false;
	gps_stationary = false;
	parked_valid = false;
	returning = false;
	last_gps_ms = now_ms - GPS_HOLD_MS; /* no fix yet = GPS not fresh */
	cell_count = 0;
	cell_seen = false;
	cell_calm_since_ms = now_ms;
}

/* Metres between two coordinates; equirectangular is exact enough at the
 * parked-radius scale (error < 0.1% under 1 km). */
static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
	double kx = 111320.0 * cos(lat1 * (3.14159265358979 / 180.0));
	double dx = (lon2 - lon1) * kx;
	double dy = (lat2 - lat1) * 110540.0;

	return sqrt(dx * dx + dy * dy);
}

void motion_note_gps(double lat_deg, double lon_deg, double acc_m,
		     int64_t now_ms)
{
	last_gps_ms = now_ms;

	if (!anchor_valid) {
		anchor_lat = lat_deg;
		anchor_lon = lon_deg;
		anchor_valid = true;
		anchor_since_ms = now_ms;
		return;
	}

	double d = dist_m(anchor_lat, anchor_lon, lat_deg, lon_deg);

	/* Return-to-parked shortcut: back inside the remembered spot's radius
	 * for RETURN_MS restores stationary without the full entry dwell. */
	if (!gps_stationary && parked_valid) {
		double dp = dist_m(parked_lat, parked_lon, lat_deg, lon_deg);

		if (dp <= RADIUS_M) {
			if (!returning) {
				returning = true;
				return_since_ms = now_ms;
			} else if ((now_ms - return_since_ms) >= RETURN_MS) {
				anchor_lat = parked_lat;
				anchor_lon = parked_lon;
				anchor_since_ms = now_ms;
				gps_stationary = true;
				returning = false;
				LOG_INF("motion: wake cancelled — GPS moved "
					"< %d m for %d s", RADIUS_M,
					(int)(RETURN_MS / 1000));
				return;
			}
		} else {
			returning = false;
			if (dp > FORGET_MULT * RADIUS_M) {
				parked_valid = false; /* genuinely departed */
			}
		}
	}

	if (d <= RADIUS_M) {
		/* In-radius: dwell accumulates. */
		if (!gps_stationary && (now_ms - anchor_since_ms) >= ENTRY_MS) {
			gps_stationary = true;
			parked_lat = anchor_lat;
			parked_lon = anchor_lon;
			parked_valid = true;
			LOG_INF("motion: stationary (within %d m for %d s)",
				RADIUS_M, CONFIG_TRACKER_MOTION_ENTRY_S);
		}
		return;
	}
	if (d <= 2.0 * acc_m) {
		/* Outside the radius but inside the fix's own error bars:
		 * evidence of NOTHING (indoor multipath jumps live here).
		 * Neither wake nor re-anchor — the dwell stands. */
		return;
	}
	/* A displacement the fix quality actually supports: motion. */
	if (gps_stationary) {
		LOG_INF("motion: moving (%d m from anchor, acc %d m)",
			(int)d, (int)acc_m);
	}
	anchor_lat = lat_deg;
	anchor_lon = lon_deg;
	anchor_since_ms = now_ms;
	gps_stationary = false;
}

void motion_note_cell(uint32_t cell_id, int64_t now_ms)
{
	for (int i = 0; i < cell_count; i++) {
		if (cells[i] == cell_id) {
			return; /* known neighbour: not evidence of motion */
		}
	}
	/* Novel cell: remember it (evict the oldest) and restart the calm
	 * clock. A journey produces a stream of novel cells and never calms. */
	if (cell_count == CELL_LRU) {
		memmove(&cells[0], &cells[1], (CELL_LRU - 1) * sizeof(cells[0]));
		cell_count--;
	}
	cells[cell_count++] = cell_id;
	if (cell_seen) { /* the very first cell of a boot is not motion */
		LOG_DBG("motion: novel cell %u", cell_id);
	}
	cell_seen = true;
	cell_calm_since_ms = now_ms;
}

void motion_note_imu(int64_t now_ms)
{
	/* Only the wake matters (interrupts also fire while already moving;
	 * those are noise the verdict already reflects). */
	if (gps_stationary || (now_ms - cell_calm_since_ms) >= ENTRY_MS) {
		LOG_INF("motion: IMU wake");
	}
	/* Break BOTH verdicts: the GPS one directly, the cell one via its
	 * calm clock (parked overnight the GPS verdict has long ceded to
	 * cells — GPS_HOLD_MS — and a wake must not be vetoed by a calm
	 * cell set). The anchor dwell restarts; parked_lat survives, so the
	 * return-to-parked shortcut re-parks a false wake in RETURN_MS. */
	gps_stationary = false;
	anchor_since_ms = now_ms;
	cell_calm_since_ms = now_ms;
}

bool motion_stationary(int64_t now_ms)
{
	if ((now_ms - last_gps_ms) < GPS_HOLD_MS) {
		return gps_stationary;
	}
	if (!cell_seen) {
		return false;
	}
	return (now_ms - cell_calm_since_ms) >= ENTRY_MS;
}
