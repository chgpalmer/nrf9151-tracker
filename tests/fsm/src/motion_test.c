/*
 * motion.c unit test: the stationary-verdict scenarios from the design
 * (park, jitter, creep, outlier exit, cell flap, novel cell).
 */
#include <zephyr/ztest.h>
#include "motion.h"

#define RADIUS_M  CONFIG_TRACKER_MOTION_RADIUS_M
#define ENTRY_MS  (CONFIG_TRACKER_MOTION_ENTRY_S * 1000)

static int64_t t;

/* Offset a base position by metres (equirectangular, fine at test scale). */
#define BASE_LAT 51.5
#define BASE_LON -0.1
static double lat_at(double north_m) { return BASE_LAT + north_m / 110540.0; }
static double lon_at(double east_m)
{
	return BASE_LON + east_m / (111320.0 * 0.62251); /* cos(51.5 deg) */
}

/* One fix per second at the given offset from base; acc defaults sharp so
 * the classic scenarios behave exactly as before the accuracy gate. */
static void gps_acc(double north_m, double east_m, double acc_m)
{
	t += 1000;
	motion_note_gps(lat_at(north_m), lon_at(east_m), acc_m, t);
}

static void gps(double north_m, double east_m)
{
	gps_acc(north_m, east_m, 5.0);
}

static void cell(uint32_t cid)
{
	t += 1000;
	motion_note_cell(cid, t);
}

ZTEST(motion, test_parked_after_entry_dwell)
{
	/* Identical fixes: stationary only once ENTRY_S of dwell accumulates. */
	for (int i = 0; i < CONFIG_TRACKER_MOTION_ENTRY_S - 1; i++) {
		gps(0, 0);
		zassert_false(motion_stationary(t), "premature at %d s", i);
	}
	gps(0, 0);
	gps(0, 0);
	zassert_true(motion_stationary(t), "should be parked after dwell");
}

ZTEST(motion, test_jitter_inside_radius_stays_parked)
{
	for (int i = 0; i < CONFIG_TRACKER_MOTION_ENTRY_S + 2; i++) {
		/* bounce around well inside the radius */
		gps((i % 2) ? RADIUS_M / 3.0 : 0, (i % 3) ? -RADIUS_M / 3.0 : 0);
	}
	zassert_true(motion_stationary(t), "jitter must not break parking");
}

ZTEST(motion, test_creep_never_parks)
{
	/* Walking pace, 1.3 m per fix: each fix is in-radius of the last
	 * anchor, but the anchor keeps resetting before dwell accumulates. */
	for (int i = 0; i < 2 * CONFIG_TRACKER_MOTION_ENTRY_S; i++) {
		gps(i * 1.3, 0);
		zassert_false(motion_stationary(t), "creep parked at fix %d", i);
	}
}

ZTEST(motion, test_outlier_exits_immediately)
{
	for (int i = 0; i <= CONFIG_TRACKER_MOTION_ENTRY_S; i++) {
		gps(0, 0);
	}
	zassert_true(motion_stationary(t), "precondition: parked");
	gps(4.0 * RADIUS_M, 0); /* the bike leaves (sharp fix) */
	zassert_false(motion_stationary(t), "one outlier must exit");
}

/* The overnight field data: indoor multipath jumps 30-130 m with accuracy
 * figures to match woke the tracker 17 times. A jump inside its own error
 * bars is evidence of nothing — no wake, and the dwell survives. */
ZTEST(motion, test_blurry_jump_does_not_wake)
{
	for (int i = 0; i <= CONFIG_TRACKER_MOTION_ENTRY_S; i++) {
		gps(0, 0);
	}
	zassert_true(motion_stationary(t), "precondition: parked");
	gps_acc(3.0 * RADIUS_M, 0, 60.0); /* 75 m jump, 60 m accuracy */
	zassert_true(motion_stationary(t), "blurry jump must not wake");
	gps(0, 0); /* back in the cluster: still parked, dwell intact */
	zassert_true(motion_stationary(t), "cluster return keeps parking");
	gps_acc(3.0 * RADIUS_M, 0, 10.0); /* same jump, SHARP: real motion */
	zassert_false(motion_stationary(t), "sharp displacement must wake");
}

/* A false wake must not cost a fresh 300 s dwell: fixes returning to the
 * remembered parked spot restore stationary after ~60 s (measured cost of
 * the old behaviour: ~300 shipped jitter points per wake). */
ZTEST(motion, test_return_to_parked_shortcut)
{
	for (int i = 0; i <= CONFIG_TRACKER_MOTION_ENTRY_S; i++) {
		gps(0, 0);
	}
	zassert_true(motion_stationary(t), "precondition: parked");
	gps(3.0 * RADIUS_M, 0); /* sharp jump: wake, spot stays remembered */
	zassert_false(motion_stationary(t), "woke");
	for (int i = 0; i < 62; i++) {
		gps(0, 0);
	}
	zassert_true(motion_stationary(t), "return shortcut must restore");
}

/* A genuine departure (beyond FORGET_MULT x radius) forgets the spot:
 * the next arrival anywhere needs the full entry dwell. */
ZTEST(motion, test_departure_forgets_parked_spot)
{
	for (int i = 0; i <= CONFIG_TRACKER_MOTION_ENTRY_S; i++) {
		gps(0, 0);
	}
	zassert_true(motion_stationary(t), "precondition: parked");
	gps(6.0 * RADIUS_M, 0); /* really gone */
	gps(7.0 * RADIUS_M, 0);
	zassert_false(motion_stationary(t), "departed");
	for (int i = 0; i < 62; i++) {
		gps(0, 0);
	}
	zassert_false(motion_stationary(t), "no shortcut after departure");
}

ZTEST(motion, test_cell_flap_within_lru_is_stationary)
{
	/* No GPS at all: cell evidence only. Two towers ping-ponging.
	 * (+5 here and below: each tower's FIRST sighting is novel and
	 * restarts the calm clock, so dwell starts a couple seconds in.) */
	for (int i = 0; i < CONFIG_TRACKER_MOTION_ENTRY_S + 5; i++) {
		cell((i % 2) ? 100 : 200);
	}
	zassert_true(motion_stationary(t), "flap inside the set is parked");
}

ZTEST(motion, test_novel_cell_is_motion)
{
	for (int i = 0; i < CONFIG_TRACKER_MOTION_ENTRY_S + 5; i++) {
		cell((i % 2) ? 100 : 200);
	}
	zassert_true(motion_stationary(t), "precondition: parked");
	cell(300); /* a tower we have not seen: the device travelled */
	zassert_false(motion_stationary(t), "novel cell must exit");
}

ZTEST(motion, test_fresh_gps_overrides_cell)
{
	/* Cells look calm, but fresh fixes show movement: GPS wins. */
	for (int i = 0; i < CONFIG_TRACKER_MOTION_ENTRY_S + 5; i++) {
		cell(100);
	}
	zassert_true(motion_stationary(t), "precondition: cell-parked");
	gps(0, 0);
	gps(200, 0); /* moving fast under the same tower */
	zassert_false(motion_stationary(t), "fresh GPS movement must win");
}

static void motion_before(void *fixture)
{
	ARG_UNUSED(fixture);
	t = 5000000;
	motion_init(t);
}

ZTEST_SUITE(motion, NULL, NULL, motion_before, NULL, NULL);
