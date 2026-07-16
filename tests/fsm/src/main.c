/*
 * loc_fsm unit test: encodes docs/loc-fsm-decision-table.md row by row.
 *
 * The table is normative. Every test names the row(s) it pins (A1, B1, ...).
 * If a test fails after an FSM change, either the change is wrong or the
 * table needs a deliberate edit — never silently adjust the test.
 *
 * The FSM takes its clock as a parameter, so time here is a plain variable:
 * the 300 s acquire timeout costs one addition, not five minutes.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include <nrf_modem_gnss.h>
#include "loc_fsm.h"

/* Internal loc_fsm.c constants without Kconfig symbols, mirrored here.
 * If one changes there, the corresponding row test fails — intended. */
#define SKY_GRACE_MS    30000
#define VISIBLE_LOST_MS 10000
#define CHOPPED_EPOCHS  10

#define ACQUIRE_CAP_MS (CONFIG_TRACKER_LOC_ACQUIRE_TIMEOUT_S * 1000)
#define STALE_MS       (CONFIG_TRACKER_GPS_STALE_S * 1000)
#define EMPTY_EPOCHS   CONFIG_TRACKER_LOC_SKY_EMPTY_EPOCHS
#define POST_MS        (CONFIG_TRACKER_POST_INTERVAL_S * 1000)
#define CELL_POST_MS   (CONFIG_TRACKER_CELL_POST_INTERVAL_S * 1000)

#define CN0_STRONG ((CONFIG_TRACKER_LOC_CN0_MIN_DBHZ + 13) * 10)
#define CN0_WEAK   ((CONFIG_TRACKER_LOC_CN0_MIN_DBHZ - 7) * 10)

#define REST_MIN_S CONFIG_TRACKER_REST_BACKOFF_MIN_S
#define REST_MAX_S CONFIG_TRACKER_REST_BACKOFF_MAX_S

/* healthy's validity floor (mirrors gnss_ctrl.c's fallback: the symbol
 * depends on TRACKER_AGNSS). */
#ifndef CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN
#define CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN 0
#endif

/* ── fake clock ─────────────────────────────────────────────────────────── */

static int64_t t;
static bool g_stationary;

/* ── the ephemeris inventory: a plain input since 2026-07-16 ─────────────── */

static struct ephe_inventory g_inv;

/* Inventory of n satellites (IDs 1..n) each holding an ephemeris that expires
 * in expiry_min minutes — digested the way gnss_ctrl would. Effective from
 * the next step(): the FSM reads it per update, no poll cadence involved. */
static void set_ephe(int n, uint16_t expiry_min)
{
	memset(&g_inv, 0, sizeof(g_inv));
	g_inv.valid = true;
	for (int i = 0; i < n && expiry_min > 0; i++) {
		g_inv.held_mask |= 1ULL << i;
		g_inv.held++;
		g_inv.min_expiry_min = expiry_min;
		if (expiry_min >= CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN) {
			g_inv.healthy_mask |= 1ULL << i;
			g_inv.healthy++;
		}
	}
}

/* ── one 1 Hz epoch ─────────────────────────────────────────────────────── */

#define OBSERVED 0
#define BLANKED  NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED
#define CHOPPED  NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME

/* Advance the clock one second and feed one PVT frame. Satellites get IDs
 * 1..tracked so they line up with set_ephe()'s inventory. */
static struct loc_status step_full(bool fix, int tracked, uint16_t cn0,
				   uint32_t flags, bool lte)
{
	struct nrf_modem_gnss_pvt_data_frame pvt = { 0 };
	struct loc_status st;

	pvt.flags = flags | (fix ? NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID : 0);
	for (int i = 0; i < tracked && i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		pvt.sv[i].sv = i + 1;
		pvt.sv[i].cn0 = cn0;
	}

	t += 1000;
	loc_fsm_update(&pvt, &g_inv, t, lte, g_stationary, &st);
	return st;
}

static struct loc_status step(bool fix, int tracked, uint32_t flags)
{
	return step_full(fix, tracked, CN0_STRONG, flags, true);
}

/* Evaluate the status expression exactly once: zassert's message args are
 * function arguments, evaluated even on pass — a bare step() in them would
 * silently run every epoch twice. */
#define ASSERT_STATE(expr, want) do { \
	struct loc_status _st = (expr); \
	zassert_equal(_st.state, (want), "in %s, expected %s", \
		      loc_state_str(_st.state), loc_state_str(want)); \
} while (0)

static void check_outputs(struct loc_status st, enum loc_gnss_mode mode,
			  bool lte, bool publish, uint32_t interval);

/* ── drivers: shortest table-sanctioned path to each state ──────────────── */

static void reset_fsm(void)
{
	t = 1000000;
	g_stationary = false;
	set_ephe(0, 0);
	loc_fsm_init(t);
	/* read_sky()'s ephemeris_visible carries the last OBSERVED value in a
	 * function-static that init cannot reset; every scenario below feeds
	 * tracked > 0 before depending on it, which recomputes it. */
}

static void to_report_cell(void)
{
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_CELL); /* A1 */
}

static void to_acquire(void)
{
	to_report_cell();
	loc_fsm_note_cell_sent();
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE); /* B2 */
}

static void to_report_gnss(void)
{
	to_report_cell();
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS); /* B1 */
}

static void to_parked_fix(void)
{
	to_report_gnss();
	g_stationary = true;
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_QUIESCENT); /* E4' */
}

static void to_exclusive(void)
{
	to_acquire();
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE); /* C2 */
}

static void to_cell_loop(void)
{
	to_acquire();
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP); /* C3 */
}

/* Two failed acquisitions while cell-stationary: the no-fix PARKED entry
 * evidence. (to_cell_loop() is the first failure — acquire timeout.) */
static void to_parked_nofix(void)
{
	to_cell_loop();
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE); /* sighted */
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP); /* failure #2 */
	g_stationary = true;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_QUIESCENT);
}

/* ── LTE_ATTACH / REPORT_CELL ───────────────────────────────────────────── */

ZTEST(loc_fsm, test_a1_registered)
{
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_CELL);
}

/* A1 clears cell_sent on entry: a latch left over from a previous cycle must
 * not satisfy B2 (F-3 documents that a *queued-stale* cell can; a latch from
 * before this registration cannot). */
ZTEST(loc_fsm, test_a1_clears_stale_cell_sent)
{
	loc_fsm_note_cell_sent();
	to_report_cell();
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_CELL);
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_CELL);
}

ZTEST(loc_fsm, test_b1_beats_b2)
{
	to_report_cell();
	loc_fsm_note_cell_sent();
	/* Both rows armed: B1 (fix) must win over B2 (cell reported). */
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS);
}

ZTEST(loc_fsm, test_b2_cell_reported)
{
	to_acquire();
}

/* ── GNSS_ACQUIRE ───────────────────────────────────────────────────────── */

ZTEST(loc_fsm, test_c1_fix)
{
	to_acquire();
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS);
}

ZTEST(loc_fsm, test_c2_chopped_escalates)
{
	to_exclusive();
}

/* One observed clean window resets the chopped streak: escalation needs
 * CHOPPED_EPOCHS *consecutive* chopped epochs. */
ZTEST(loc_fsm, test_c2_clean_window_resets_streak)
{
	to_acquire();
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE);
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE);
}

/* Parked trackers never escalate to EXCLUSIVE: no urgent fix need, and each
 * escalation costs an LTE deactivate + re-attach (7 pointless cycles in one
 * parked night). Chopped windows while stationary ride out to the C3/C4
 * exits and resolve via CELL_LOOP -> REST. */
ZTEST(loc_fsm, test_c2_stationary_never_escalates)
{
	to_acquire();
	g_stationary = true;
	for (int i = 0; i < 3 * CHOPPED_EPOCHS; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	/* ...and the timeout still ends it the parked way. */
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, CHOPPED), LOC_CELL_LOOP);
}

/* F-4: a blanked epoch (DEADLINE_MISSED, GNSS never had the radio) neither
 * advances nor resets the chopped streak. */
ZTEST(loc_fsm, test_c2_blanked_carries_chopped_streak)
{
	to_acquire();
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, BLANKED), LOC_GNSS_ACQUIRE);
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE);
}

/* C2 cold-with-supply gate: an inventory too thin to fix (< SATS_FOR_FIX)
 * plus a live A-GNSS supply refuses the escalation — LTE IS the supply
 * line, and going dark trades a seconds-scale fetch for minutes of 50 bps
 * demodulation (2026-07-12 ride + 2026-07-13 commute: 10-11 min blind
 * each). The C3 timeout still bounds the stay, exiting to CELL_LOOP with
 * LTE up. reset_fsm()'s set_ephe(0,0) is the cold inventory. */
ZTEST(loc_fsm, test_c2_cold_with_supply_never_goes_dark)
{
	loc_fsm_set_agnss_supply(true);
	to_acquire();
	for (int i = 0; i < 3 * CHOPPED_EPOCHS; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, CHOPPED), LOC_CELL_LOOP);
}

/* A warm inventory (>= SATS_FOR_FIX ephemerides held) escalates as before:
 * the chop is then genuinely blocking tracking of usable satellites, and
 * radio silence is the correct cure. */
ZTEST(loc_fsm, test_c2_warm_inventory_still_escalates)
{
	loc_fsm_set_agnss_supply(true);
	set_ephe(SATS_FOR_FIX, 60);
	to_acquire();
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE);
}

/* The supply verdict going false mid-hold (agnss lockout after repeated
 * fetch failures) re-opens the escalation: EXCLUSIVE stays available as
 * the genuine no-server fallback. */
ZTEST(loc_fsm, test_c2_supply_lost_restores_escalation)
{
	loc_fsm_set_agnss_supply(true);
	to_acquire();
	for (int i = 0; i < 2 * CHOPPED_EPOCHS; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	loc_fsm_set_agnss_supply(false);
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE);
}

/* The definitions row: an AGING inventory — plenty held, nothing healthy
 * (all under the 30 min floor). The C2 gate keys on `held`, so with the
 * supply alive it still escalates; `healthy` says the fetch was warranted.
 * This is the held/healthy divergence band made explicit — the gate flip
 * (commit 2 of the inventory refactor) changes this row to hold in ACQUIRE. */
ZTEST(loc_fsm, test_c2_aging_inventory)
{
	loc_fsm_set_agnss_supply(true);
	set_ephe(6, 10); /* held=6, healthy=0 */
	to_acquire();
	for (int i = 0; i < CHOPPED_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, CHOPPED), LOC_GNSS_EXCLUSIVE);
}

/* agnss_fetch_allowed: the radio-policy half of the fetch decision, per
 * state. Publishing states allow it; ACQUIRE only while the inventory
 * cannot support a fix (healthy < SATS_FOR_FIX); EXCLUSIVE (silence) and
 * QUIESCENT (sleep) never. */
ZTEST(loc_fsm, test_agnss_fetch_allowed_by_state)
{
	struct loc_status st;

	st = step_full(false, 0, 0, OBSERVED, false); /* LTE_ATTACH */
	zassert_false(st.agnss_fetch_allowed, "LTE_ATTACH");

	st = step(false, 0, OBSERVED); /* -> REPORT_CELL */
	zassert_true(st.agnss_fetch_allowed, "REPORT_CELL");

	loc_fsm_note_cell_sent();
	st = step(false, 0, OBSERVED); /* -> GNSS_ACQUIRE, cold inventory */
	zassert_true(st.agnss_fetch_allowed, "ACQUIRE cold: fetch is the fast path");

	set_ephe(SATS_FOR_FIX, 60); /* healthy inventory: radio quiet matters */
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_GNSS_ACQUIRE);
	zassert_false(st.agnss_fetch_allowed, "ACQUIRE warm");

	set_ephe(0, 0);
	for (int i = 0; i < CHOPPED_EPOCHS; i++) {
		st = step(false, 0, CHOPPED); /* -> EXCLUSIVE (no supply) */
	}
	ASSERT_STATE(st, LOC_GNSS_EXCLUSIVE);
	zassert_false(st.agnss_fetch_allowed, "EXCLUSIVE: silence is the point");

	st = step_full(true, 4, CN0_STRONG, OBSERVED, false); /* -> REPORT_GNSS */
	ASSERT_STATE(st, LOC_REPORT_GNSS);
	zassert_true(st.agnss_fetch_allowed, "REPORT_GNSS");

	reset_fsm();
	to_cell_loop();
	st = step(false, 0, OBSERVED);
	zassert_true(st.agnss_fetch_allowed, "CELL_LOOP");

	reset_fsm();
	to_parked_fix();
	st = step(true, 4, OBSERVED);
	zassert_false(st.agnss_fetch_allowed, "QUIESCENT: sleep is the point");
}

ZTEST(loc_fsm, test_c3_acquire_timeout)
{
	/* tracked=1 the whole way: a teasing sky must ride out the FULL cap
	 * (sky-empty never fires), then C3 takes it to CELL_LOOP. */
	to_cell_loop();
}

ZTEST(loc_fsm, test_c4_sky_empty_after_grace)
{
	to_acquire();
	/* The streak accumulates during the cold-start grace but must not
	 * exit inside it... */
	for (int i = 0; i < EMPTY_EPOCHS + 2; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	}
	/* ...and the first empty epoch past the grace completes the exit. */
	t += SKY_GRACE_MS;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

/* F-4, the core case: blanked epochs pause the empty-sky count — absence of
 * observation is not observation of absence. */
ZTEST(loc_fsm, test_c4_blanked_pauses_empty_streak)
{
	to_acquire();
	t += SKY_GRACE_MS; /* past the grace; only the streak matters now */
	for (int i = 0; i < EMPTY_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	}
	for (int i = 0; i < 20; i++) {
		ASSERT_STATE(step(false, 0, BLANKED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

/* A sighting resets the empty streak: giving up needs CONSECUTIVE observed
 * emptiness. */
ZTEST(loc_fsm, test_c4_sighting_resets_empty_streak)
{
	to_acquire();
	t += SKY_GRACE_MS;
	for (int i = 0; i < EMPTY_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE);
	for (int i = 0; i < EMPTY_EPOCHS - 1; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	}
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

/* ── GNSS_EXCLUSIVE ─────────────────────────────────────────────────────── */

/* D-rows run with lte_registered=false throughout — registration lapsing is
 * the point of the state, and the global override must NOT bounce it. */

ZTEST(loc_fsm, test_d1_fix)
{
	to_exclusive();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_GNSS_EXCLUSIVE);
	ASSERT_STATE(step_full(true, 4, CN0_STRONG, OBSERVED, false),
		     LOC_REPORT_GNSS);
}

ZTEST(loc_fsm, test_d2_timeout)
{
	to_exclusive();
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step_full(false, 1, CN0_WEAK, OBSERVED, false),
		     LOC_CELL_LOOP);
}

ZTEST(loc_fsm, test_d3_sky_empty)
{
	to_exclusive();
	t += SKY_GRACE_MS;
	for (int i = 0; i < EMPTY_EPOCHS - 1; i++) {
		ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false),
			     LOC_GNSS_EXCLUSIVE);
	}
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_CELL_LOOP);
}

/* ── global override ────────────────────────────────────────────────────── */

ZTEST(loc_fsm, test_override_from_report_cell)
{
	to_report_cell();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
}

ZTEST(loc_fsm, test_override_from_acquire)
{
	to_acquire();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
}

ZTEST(loc_fsm, test_override_from_report_gnss)
{
	to_report_gnss();
	ASSERT_STATE(step_full(true, 4, CN0_STRONG, OBSERVED, false),
		     LOC_LTE_ATTACH);
}

ZTEST(loc_fsm, test_override_from_cell_loop)
{
	to_cell_loop();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
}

/* ── REPORT_GNSS ────────────────────────────────────────────────────────── */

ZTEST(loc_fsm, test_e1_sky_lost)
{
	to_report_gnss();
	/* Observed-empty epochs: the streak passes EMPTY_EPOCHS long before
	 * the fix goes stale, so the moment it does, E1 (not E2) fires. */
	for (int i = 1; i < STALE_MS / 1000; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_GNSS);
	}
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

ZTEST(loc_fsm, test_e2_stale_hotstart_dead)
{
	/* No ephemeris inventory at all: satellites are tracked (so the sky
	 * is not empty) but none can seed a hot start. */
	to_report_gnss();
	for (int i = 1; i < STALE_MS / 1000; i++) {
		ASSERT_STATE(step(false, 3, OBSERVED), LOC_REPORT_GNSS);
	}
	ASSERT_STATE(step(false, 3, OBSERVED), LOC_GNSS_ACQUIRE);
}

ZTEST(loc_fsm, test_e3_stale_but_hotstart_viable_waits)
{
	struct loc_status st;

	/* 4 tracked satellites with valid ephemeris: a re-fix is a hot start,
	 * so REPORT_GNSS waits — indefinitely — instead of disturbing LTE. */
	set_ephe(4, 120);
	to_report_gnss();
	for (int i = 1; i <= STALE_MS / 1000 + 10; i++) {
		st = step(false, 4, OBSERVED);
		ASSERT_STATE(st, LOC_REPORT_GNSS);
	}
	zassert_false(st.gps_current, "fix should be stale by now");
	zassert_true(st.prefer_cell, "stale fix must fall back to cell");
	/* The awaited re-fix arrives: still REPORT_GNSS, GPS preferred again. */
	st = step(true, 4, OBSERVED);
	ASSERT_STATE(st, LOC_REPORT_GNSS);
	zassert_false(st.prefer_cell, "current fix must prefer GPS");
}

/* F-1 regression: a current fix with ephemeris about to expire must STAY in
 * REPORT_GNSS. The deleted E4 transition bounced to ACQUIRE and back at 1 Hz
 * for the whole refresh window — two INF lines per second onto the uplink. */
ZTEST(loc_fsm, test_f1_expiring_ephemeris_never_exits)
{
	struct loc_status st;

	set_ephe(4, 5); /* well under the old 15 min refresh threshold */
	to_report_gnss();
	for (int i = 0; i < 15; i++) {
		st = step(true, 4, OBSERVED);
		ASSERT_STATE(st, LOC_REPORT_GNSS);
	}
	/* Prove the trap was actually armed, not vacuously green. */
	zassert_equal(st.ephemeris_held, 4, "expected 4 held, got %u",
		      st.ephemeris_held);
	zassert_equal(st.min_ephe_expiry_min, 5, "expected expiry 5, got %u",
		      st.min_ephe_expiry_min);
}

/* ── PARKED, fix flavor (legacy: periodic checks) ───────────────────────── */

ZTEST(loc_fsm, test_p_entry_requires_stationary_and_fix)
{
	to_report_gnss();
	/* moving: stays put no matter how long */
	for (int i = 0; i < 30; i++) {
		ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS);
	}
	g_stationary = true;
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_QUIESCENT);
}

ZTEST(loc_fsm, test_p1_motion_resumes_tracking)
{
	to_parked_fix();
	g_stationary = false;
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS);
}

/* Staleness is cadence-aware: a fix is current until 3 check intervals
 * pass without one, not 30 s. And the sky-empty evidence is NOT evaluated
 * here — empty checks lead to ACQUIRE via staleness, never to CELL_LOOP. */
ZTEST(loc_fsm, test_p2_checks_not_fixing)
{
	to_parked_fix();
	/* six observed-empty epochs: >SKY_EMPTY_EPOCHS, way past 30 s-stale —
	 * both would have fired in REPORT_GNSS; QUIESCENT must hold. */
	for (int i = 0; i < EMPTY_EPOCHS + 1; i++) {
		ASSERT_STATE(step(false, 0, OBSERVED), LOC_QUIESCENT);
	}
	/* ...but three missed checks means the parked spot lost its sky. */
	t += 3 * (int64_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
}

ZTEST(loc_fsm, test_p_fix_outputs)
{
	struct loc_status st;

	to_parked_fix();
	st = step(true, 4, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	check_outputs(st, LOC_GNSS_PERIODIC, true, true,
		      (uint32_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000);
	zassert_equal(st.gnss_interval_s, CONFIG_TRACKER_QUIESCENT_CHECK_S,
		      "check interval");
	zassert_false(st.prefer_cell, "parked with fix: GPS, not cell");
}

ZTEST(loc_fsm, test_p_override_registration_loss)
{
	to_parked_fix();
	ASSERT_STATE(step_full(true, 4, CN0_STRONG, OBSERVED, false),
		     LOC_LTE_ATTACH);
}

/* ── CELL_LOOP ──────────────────────────────────────────────────────────── */

ZTEST(loc_fsm, test_g1_fix_returned)
{
	to_cell_loop();
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_REPORT_GNSS);
}

/* G2: ONE tracked satellite — however weak, even in a chopped window —
 * earns a real acquisition attempt. Chopped windows understate the sky. */
ZTEST(loc_fsm, test_g2_single_weak_sighting)
{
	to_cell_loop();
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
	ASSERT_STATE(step(false, 0, BLANKED), LOC_CELL_LOOP);
	ASSERT_STATE(step_full(false, 1, CN0_WEAK, CHOPPED, true),
		     LOC_GNSS_ACQUIRE);
}

/* ── PARKED, no-fix flavor (legacy: backoff retries + heartbeat) ────────── */

ZTEST(loc_fsm, test_p_nofix_entry_needs_two_failures)
{
	to_cell_loop(); /* one failure only */
	g_stationary = true;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

ZTEST(loc_fsm, test_p_nofix_entry_needs_stationary)
{
	to_cell_loop();
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE);
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP);
	/* two failures but moving: keep trying, never rest */
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP);
}

/* A check landing a fix upgrades the flavor IN PLACE: the old REST bounced
 * REST -> REPORT_GNSS -> QUIESCENT, two transitions narrating nothing. */
ZTEST(loc_fsm, test_p_nofix_fix_upgrades_in_place)
{
	struct loc_status st;

	to_parked_nofix();
	st = step(true, 4, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	zassert_true(st.gps_current, "fix taken");
	zassert_equal(st.gnss_interval_s, CONFIG_TRACKER_QUIESCENT_CHECK_S,
		      "flavor upgraded to periodic checks");
	zassert_false(st.prefer_cell, "parked with fix: GPS, not cell");
}

ZTEST(loc_fsm, test_p_nofix_sighting_earns_acquire)
{
	to_parked_nofix();
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE);
}

ZTEST(loc_fsm, test_p_nofix_motion_exits_and_resets_backoff)
{
	struct loc_status st;

	to_parked_nofix();
	/* grow the backoff first */
	t += (int64_t)REST_MIN_S * 1000;
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	zassert_equal(st.gnss_interval_s, 2 * REST_MIN_S, "doubled");
	g_stationary = false;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	/* fail again twice, re-enter REST: backoff starts at MIN again */
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP);
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_GNSS_ACQUIRE);
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP);
	g_stationary = true;
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	zassert_equal(st.gnss_interval_s, (uint32_t)REST_MIN_S, "backoff reset");
}

ZTEST(loc_fsm, test_p_nofix_backoff_doubles_to_cap)
{
	struct loc_status st;
	uint32_t expect = REST_MIN_S;

	to_parked_nofix();
	while (expect < REST_MAX_S) {
		t += (int64_t)expect * 1000;
		st = step(false, 0, OBSERVED);
		ASSERT_STATE(st, LOC_QUIESCENT);
		expect = MIN(expect * 2, (uint32_t)REST_MAX_S);
		zassert_equal(st.gnss_interval_s, expect, "backoff step");
	}
	/* at the cap it stays there */
	t += (int64_t)REST_MAX_S * 1000;
	st = step(false, 0, OBSERVED);
	zassert_equal(st.gnss_interval_s, (uint32_t)REST_MAX_S, "capped");
}

/* One wake delivers several PVT frames (the retry window); the backoff
 * must double once per wake, not once per frame. */
ZTEST(loc_fsm, test_p_nofix_backoff_once_per_wake)
{
	struct loc_status st;

	to_parked_nofix();
	t += (int64_t)REST_MIN_S * 1000;
	st = step(false, 0, OBSERVED); /* first frame of the wake: doubles */
	zassert_equal(st.gnss_interval_s, 2 * REST_MIN_S, "first frame");
	st = step(false, 0, OBSERVED); /* second frame, 1 s later: no change */
	zassert_equal(st.gnss_interval_s, 2 * REST_MIN_S, "second frame");
}

ZTEST(loc_fsm, test_p_nofix_outputs)
{
	struct loc_status st;

	to_parked_nofix();
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	check_outputs(st, LOC_GNSS_PERIODIC, true, true,
		      (uint32_t)CONFIG_TRACKER_REST_HEARTBEAT_S * 1000);
	zassert_true(st.prefer_cell, "no-fix PARKED publishes heartbeat cells");
}

ZTEST(loc_fsm, test_p_nofix_override_registration_loss)
{
	to_parked_nofix();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
}

ZTEST(loc_fsm, test_failures_reset_on_fix)
{
	/* A successful check-fix clears the failure count (and upgrades the
	 * flavor): after going stale again, CELL_LOOP needs two FRESH
	 * failures before parking no-fix. */
	to_parked_nofix();
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_QUIESCENT); /* upgraded */
	t += 3 * (int64_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000;
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
	t += ACQUIRE_CAP_MS;
	ASSERT_STATE(step(false, 1, OBSERVED), LOC_CELL_LOOP); /* failure #1 */
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_CELL_LOOP); /* needs two */
}

/* ── PARKED, IMU mode (GNSS off; the accelerometer owns departure) ──────── */

ZTEST(loc_fsm, test_pi_gnss_off_and_heartbeat)
{
	struct loc_status st;

	loc_fsm_set_imu_wake(true);
	to_parked_fix();
	st = step(true, 4, OBSERVED); /* frozen-frame artifacts change nothing */
	ASSERT_STATE(st, LOC_QUIESCENT);
	check_outputs(st, LOC_GNSS_OFF, true, true,
		      (uint32_t)CONFIG_TRACKER_REST_HEARTBEAT_S * 1000);
	zassert_equal(st.gnss_interval_s, 0, "no periodic checks");
}

/* No GNSS evidence can move an IMU-parked tracker: not staleness, not
 * sightings, not fixes. The ONLY exits are the stationary verdict (IMU,
 * via motion.c) and the registration-loss override. */
ZTEST(loc_fsm, test_pi_ignores_gnss_evidence)
{
	loc_fsm_set_imu_wake(true);
	to_parked_fix();
	t += 3 * (int64_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000; /* stale */
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_QUIESCENT);
	ASSERT_STATE(step(false, 3, OBSERVED), LOC_QUIESCENT); /* sighting */
	ASSERT_STATE(step(true, 4, OBSERVED), LOC_QUIESCENT);  /* stray fix */
	t += 24 * 60 * 60 * 1000; /* a full parked day */
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_QUIESCENT);
}

/* IMU wake goes to REPORT_CELL, NOT ACQUIRE: the server must learn of the
 * movement (alert + free cell fix) and the A-GNSS fetch must run before the
 * radio-silent acquisition. The wake path is the boot path. */
ZTEST(loc_fsm, test_pi_motion_wakes_to_report_cell)
{
	loc_fsm_set_imu_wake(true);
	to_parked_fix();
	g_stationary = false; /* motion_note_imu()'s verdict flip */
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_REPORT_CELL);
	/* ...and REPORT_CELL then walks the normal boot path to ACQUIRE. */
	loc_fsm_note_cell_sent();
	ASSERT_STATE(step(false, 0, OBSERVED), LOC_GNSS_ACQUIRE);
}

ZTEST(loc_fsm, test_pi_nofix_entry_also_off)
{
	struct loc_status st;

	loc_fsm_set_imu_wake(true);
	to_parked_nofix();
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_QUIESCENT);
	zassert_equal(st.gnss_mode, LOC_GNSS_OFF, "GNSS off, no-fix flavor too");
	zassert_true(st.prefer_cell, "heartbeat cells");
}

ZTEST(loc_fsm, test_pi_override_registration_loss)
{
	loc_fsm_set_imu_wake(true);
	to_parked_fix();
	ASSERT_STATE(step_full(false, 0, 0, OBSERVED, false), LOC_LTE_ATTACH);
}

/* ── outputs table ──────────────────────────────────────────────────────── */

static void check_outputs(struct loc_status st, enum loc_gnss_mode mode,
			  bool lte, bool publish, uint32_t interval)
{
	zassert_equal(st.gnss_mode, mode, "%s: gnss_mode",
		      loc_state_str(st.state));
	zassert_equal(st.lte_wanted, lte, "%s: lte_wanted",
		      loc_state_str(st.state));
	zassert_equal(st.publish_allowed, publish, "%s: publish_allowed",
		      loc_state_str(st.state));
	zassert_equal(st.publish_interval_ms, interval, "%s: interval",
		      loc_state_str(st.state));
}

ZTEST(loc_fsm, test_outputs_by_state)
{
	struct loc_status st;

	st = step_full(false, 0, 0, OBSERVED, false); /* LTE_ATTACH */
	check_outputs(st, LOC_GNSS_OFF, true, false, POST_MS);

	st = step(false, 0, OBSERVED); /* -> REPORT_CELL */
	check_outputs(st, LOC_GNSS_CONTINUOUS, true, true, POST_MS);
	zassert_true(st.prefer_cell, "REPORT_CELL: prefer_cell");

	loc_fsm_note_cell_sent();
	st = step(false, 0, OBSERVED); /* -> GNSS_ACQUIRE */
	check_outputs(st, LOC_GNSS_CONTINUOUS, true, false, POST_MS);

	for (int i = 0; i < CHOPPED_EPOCHS; i++) {
		st = step(false, 0, CHOPPED); /* -> GNSS_EXCLUSIVE */
	}
	ASSERT_STATE(st, LOC_GNSS_EXCLUSIVE);
	/* gnss stays CONTINUOUS even though registration lapses: the
	 * exception written into the output rule. */
	st = step_full(false, 0, 0, OBSERVED, false);
	check_outputs(st, LOC_GNSS_CONTINUOUS, false, false, POST_MS);

	st = step_full(true, 4, CN0_STRONG, OBSERVED, false); /* -> REPORT_GNSS */
	ASSERT_STATE(st, LOC_REPORT_GNSS);
	st = step(true, 4, OBSERVED);
	check_outputs(st, LOC_GNSS_CONTINUOUS, true, true, POST_MS);
	zassert_false(st.prefer_cell, "REPORT_GNSS+fix: prefer_cell");

	reset_fsm();
	to_cell_loop();
	st = step(false, 0, OBSERVED);
	ASSERT_STATE(st, LOC_CELL_LOOP);
	check_outputs(st, LOC_GNSS_CONTINUOUS, true, true, CELL_POST_MS);
	zassert_true(st.prefer_cell, "CELL_LOOP: prefer_cell");
}

/* ── suite ──────────────────────────────────────────────────────────────── */

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_fsm();
}

ZTEST_SUITE(loc_fsm, NULL, NULL, before_each, NULL, NULL);
