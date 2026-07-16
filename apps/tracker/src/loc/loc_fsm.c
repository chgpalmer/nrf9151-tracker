#include "loc_fsm.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(loc_fsm, CONFIG_TRACKER_LOG_LEVEL);

/* C/N0 is reported in 0.1 dB/Hz. Below roughly 27 dB-Hz a satellite can still
 * be tracked but its navigation message cannot be demodulated reliably, which
 * is the difference between "tracked" and "ephemeris". */
#define CN0_STRONG_01DBHZ (CONFIG_TRACKER_LOC_CN0_MIN_DBHZ * 10)

#define ACQUIRE_CAP_MS   (CONFIG_TRACKER_LOC_ACQUIRE_TIMEOUT_S * 1000)
#define GPS_STALE_MS     (CONFIG_TRACKER_GPS_STALE_S * 1000)
#define POST_MS          (CONFIG_TRACKER_POST_INTERVAL_S * 1000)
#define CELL_POST_MS     (CONFIG_TRACKER_CELL_POST_INTERVAL_S * 1000)
/* Consecutive observed-empty epochs before declaring the sky dead. */
#define SKY_EMPTY_EPOCHS CONFIG_TRACKER_LOC_SKY_EMPTY_EPOCHS
/* A cold receiver shows nothing for its first tens of seconds even under a
 * perfect sky; the empty-sky bail must not count epochs before this, or every
 * boot would abandon its first acquisition while GNSS was still warming up. */
#define SKY_GRACE_MS     30000

/* "Hot start impossible" (fewer than SATS_FOR_FIX ephemeris-bearing satellites
 * in view) must be sustained before REPORT gives up on a stale fix and takes a
 * quiet window: the visible count jitters epoch to epoch, and one dip from 4 to
 * 3 costs an unnecessary silence. Same treatment as every other sky judgement. */
#define VISIBLE_LOST_MS 10000

/* Indexed by enum loc_state; keep in step with it. */
static const char *const loc_state_names[] = {
	[LOC_LTE_ATTACH]  = "LTE_ATTACH",
	[LOC_REPORT_CELL] = "REPORT_CELL",
	[LOC_GNSS_ACQUIRE] = "GNSS_ACQUIRE",
	[LOC_GNSS_EXCLUSIVE] = "GNSS_EXCLUSIVE",
	[LOC_REPORT_GNSS] = "REPORT_GNSS",
	[LOC_CELL_LOOP]   = "CELL_LOOP",
	[LOC_QUIESCENT]      = "QUIESCENT",
};
BUILD_ASSERT(ARRAY_SIZE(loc_state_names) == LOC_STATE_COUNT,
	     "loc_state_names out of step with enum loc_state");

static enum loc_state state;
static int64_t state_entered_ms;
static int64_t last_fix_ms;
/* Consecutive OBSERVED epochs with zero satellites. Blanked epochs (LTE held
 * the radio; DEADLINE_MISSED set) count toward neither side. */
static uint8_t sky_empty_streak;
/* Consecutive epochs with NOT_ENOUGH_WINDOW_TIME: the modem's own testimony
 * that LTE idle-mode work (DRX paging, cell reselection at the coverage edge)
 * is slicing GNSS too thin even though the application is silent. PSM being
 * *granted* does not mean PSM sleep is *happening* -- measured at RSRP -141:
 * the modem never settles, and 0x10 persists for minutes in GNSS_ACQUIRE. */
static uint8_t chopped_streak;
#define CHOPPED_EPOCHS 10
/* Last *observed* epoch with enough ephemeris-bearing satellites in view. */
static int64_t last_visible_ok_ms;
static bool cell_sent;

/* A-GNSS supply verdict from main.c (default false = no supply). See the
 * setter's contract in loc_fsm.h; consumed by the C2 escalation gate. */
static bool agnss_supply;

/* IMU wake mode (boot verdict from main.c; default false = legacy). */
static bool imu_wake;
/* Did PARKED get entered holding a fix? Selects the legacy flavor (checks
 * vs backoff-retries); flips true if a check lands a fix. Meaningless in
 * IMU mode, where GNSS is off and there is nothing to flavor. */
static bool parked_had_fix;

/* Consecutive ACQUIRE/EXCLUSIVE attempts since the last fix that ended in
 * timeout or an empty sky. Unlike the sky streaks this survives state
 * changes: it is the "stop trying so hard" evidence for REST. Reset by any
 * fix and by leaving REST on motion. */
static uint8_t acquire_failures;
/* REST's current retry interval and when it last grew. */
static uint32_t rest_backoff_s;
static int64_t rest_backoff_bumped_ms;

const char *loc_state_str(enum loc_state s)
{
	return (s < LOC_STATE_COUNT) ? loc_state_names[s] : "?";
}

/* The ONLY place `state` is ever written. Every transition is logged with its
 * reason and the dwell time of the state being left, so a UART capture reads
 * as a complete trace: what ended, why, and how long it lasted. (For
 * GNSS_ACQUIRE -> REPORT_GNSS the dwell IS the time-to-fix.) */
static void next_state(enum loc_state next, int64_t now_ms, const char *why)
{
	if (next == state) {
		return; /* self-transitions are not events */
	}
	LOG_INF("loc: %s -> %s (%s; after %lld s)",
		loc_state_names[state], loc_state_names[next], why,
		(long long)((now_ms - state_entered_ms) / 1000));
	state = next;
	state_entered_ms = now_ms;
	/* Judge the new state on its own evidence, not sky history from the old. */
	sky_empty_streak    = 0;
	chopped_streak      = 0;
	last_visible_ok_ms  = now_ms;
}

void loc_fsm_init(int64_t now_ms)
{
	state = LOC_LTE_ATTACH;
	state_entered_ms  = now_ms;
	last_fix_ms       = now_ms - GPS_STALE_MS;
	sky_empty_streak   = 0;
	chopped_streak     = 0;
	last_visible_ok_ms = now_ms;
	cell_sent = false;
	acquire_failures = 0;
	agnss_supply = false;
	imu_wake = false;
	parked_had_fix = false;
	rest_backoff_s = CONFIG_TRACKER_REST_BACKOFF_MIN_S;
	rest_backoff_bumped_ms = now_ms;
}

void loc_fsm_note_cell_sent(void)
{
	cell_sent = true;
}

void loc_fsm_set_agnss_supply(bool available)
{
	agnss_supply = available;
}

void loc_fsm_set_imu_wake(bool present)
{
	imu_wake = present;
}

/* Does the inventory hold a usable ephemeris for this satellite? GPS only:
 * the masks cover sv_id 1..32 (see the struct comment in loc_fsm.h). */
static bool sv_has_ephemeris(const struct ephe_inventory *inv, uint16_t sv_id)
{
	return sv_id >= 1 && sv_id <= 32 &&
	       (inv->held_mask & (1ULL << (sv_id - 1)));
}

static void read_sky(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		     const struct ephe_inventory *inv,
		     struct loc_status *st)
{
	/* ephemeris_visible drives the REPORT->ACQUIRE decision, so an epoch in
	 * which GNSS got no radio window (every satellite slot empty -- routine
	 * right after a publish) must not read as "no ephemeris visible". Absence
	 * of observation is not observation of absence: carry the last real
	 * observation instead. */
	static uint8_t last_visible;
	uint8_t visible = 0;

	st->tracked = 0;
	st->strong = 0;
	st->used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		const struct nrf_modem_gnss_sv *sv = &pvt->sv[i];

		if (sv->sv == 0) {
			continue;
		}
		st->tracked++;
		if (sv->cn0 >= CN0_STRONG_01DBHZ) {
			st->strong++;
		}
		if (sv->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			st->used++;
		}
		if (sv_has_ephemeris(inv, sv->sv)) {
			visible++;
		}
	}

	if (st->tracked > 0) {
		last_visible = visible;
	}
	st->ephemeris_visible = last_visible;

	st->ephemeris_held = inv->valid ? inv->held : 0;
	st->min_ephe_expiry_min = inv->valid ? inv->min_expiry_min : 0;
	st->gps_time_known = inv->valid &&
		!(inv->data_flags &
		  NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST);

	st->pdop = pvt->pdop;
}

void loc_fsm_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		    const struct ephe_inventory *inv,
		    int64_t now_ms, bool lte_registered, bool stationary,
		    struct loc_status *out)
{
	bool fix = pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;

	if (fix) {
		last_fix_ms = now_ms;
		acquire_failures = 0;
	}

	read_sky(pvt, inv, out);
	/* Staleness is judged against the CURRENT sampling cadence: at 1 Hz a
	 * 30 s hole means trouble, but under periodic checks a fix is only
	 * expected every check interval -- three missed checks is the
	 * equivalent evidence. */
	int64_t stale_ms = (state == LOC_QUIESCENT && parked_had_fix && !imu_wake)
		? 3 * (int64_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000
		: GPS_STALE_MS;
	out->gps_current = (now_ms - last_fix_ms) < stale_ms;

	/* Sky evidence. CELL_LOOP's chopped windows systematically UNDERSTATE the
	 * sky (C/N0 estimates need integration time a 20 s gap doesn't give), so
	 * demanding fix-grade signals before granting radio silence means the
	 * evidence can never accumulate. Ask instead the one question a chopped
	 * window can answer -- is anything up there at all? -- and let ACQUIRE's
	 * uninterrupted silence answer the rest.
	 *
	 * Only OBSERVED epochs count. tracked > 0 is observation by definition;
	 * an empty frame is an observation only if GNSS actually had the radio
	 * (DEADLINE_MISSED/NOT_ENOUGH_WINDOW_TIME clear). A blanked epoch counts
	 * toward neither side -- absence of observation is not observation of
	 * absence, in either direction. */
	bool observed_window = !(pvt->flags &
		(NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED |
		 NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME));
	if (out->tracked > 0) {
		sky_empty_streak = 0;
	} else if (observed_window && sky_empty_streak < UINT8_MAX) {
		sky_empty_streak++;
	}
	if (pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
		if (chopped_streak < UINT8_MAX) {
			chopped_streak++;
		}
	} else if (observed_window) {
		chopped_streak = 0;
	}
	out->sky_empty = sky_empty_streak;
	bool sats_in_view = (out->tracked > 0);
	bool sky_empty = sky_empty_streak >= SKY_EMPTY_EPOCHS;
	bool lte_chops_gnss = chopped_streak >= CHOPPED_EPOCHS;

	/* Hot-start viability, debounced on observed epochs only (blanked epochs
	 * carry the cached count and must not refresh the clock either way). */
	if (out->tracked > 0 && out->ephemeris_visible >= SATS_FOR_FIX) {
		last_visible_ok_ms = now_ms;
	}
	bool hotstart_dead = (now_ms - last_visible_ok_ms) > VISIBLE_LOST_MS;
	int64_t in_state = now_ms - state_entered_ms;

	/* Registration loss overrides everything -- an unregistered modem runs a
	 * continuous cell search, which both starves GNSS and is starved by it --
	 * EXCEPT in GNSS_EXCLUSIVE, where losing registration is deliberate. */
	if (!lte_registered && state != LOC_LTE_ATTACH &&
	    state != LOC_GNSS_EXCLUSIVE) {
		next_state(LOC_LTE_ATTACH, now_ms, "registration lost");
	}

	switch (state) {
	case LOC_LTE_ATTACH:
		if (lte_registered) {
			cell_sent = false;
			next_state(LOC_REPORT_CELL, now_ms, "registered");
		}
		break;

	case LOC_REPORT_CELL:
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix acquired");
		} else if (cell_sent) {
			next_state(LOC_GNSS_ACQUIRE, now_ms, "cell reported");
		}
		break;

	case LOC_GNSS_ACQUIRE:
		/* Once here, LET IT TRY: satellites in view -- however weak --
		 * ride out the full cap, because quiet continuous tracking is
		 * exactly what strengthens them. Give up early only on a truly
		 * empty sky, and never inside the cold-start grace. */
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix acquired");
		} else if (lte_chops_gnss && !stationary &&
			   !(agnss_supply && inv->healthy < SATS_FOR_FIX)) {
			/* Registered-but-silent isn't enough here: idle DRX or
			 * coverage-edge reselection is slicing GNSS. Take the
			 * radio outright; the re-attach is paid post-fix.
			 * NOT while stationary: a parked tracker has no urgent
			 * need for a fix, and each escalation costs an LTE
			 * deactivate + ~35 s re-attach (measured 7 pointless
			 * cycles in one parked night). Parked no-fix resolves
			 * through CELL_LOOP -> REST instead.
			 * NOT while the inventory cannot support a fix AND
			 * the A-GNSS supply is alive: LTE *is* the supply
			 * line, and going dark trades a seconds fetch for
			 * minutes of 50 bps demodulation — twice measured at
			 * 10-11 min blind (2026-07-12 ride, 2026-07-13
			 * commute). "Cannot support a fix" is judged on
			 * HEALTHY (>= 30 min left), not held: held counts
			 * ephemerides whose satellites may have set hours
			 * ago or die mid-hunt — the gate must not bless a
			 * radio-dark hunt on stock like that while one fetch
			 * would replace it (same verdict the fetch trigger
			 * uses, same snapshot). Cold-with-supply holds here:
			 * LTE stays up, agnss_poll retries on its cold
			 * cadence, and the acquire-timeout exit still bounds
			 * the stay. The supply verdict goes false after
			 * repeated fetch failures (lockout), which re-opens
			 * this gate — the genuine no-server fallback is
			 * preserved. */
			next_state(LOC_GNSS_EXCLUSIVE, now_ms, "LTE chops GNSS");
		} else if (in_state > ACQUIRE_CAP_MS) {
			if (acquire_failures < UINT8_MAX) {
				acquire_failures++;
			}
			next_state(LOC_CELL_LOOP, now_ms, "acquire timeout");
		} else if (in_state > SKY_GRACE_MS && sky_empty) {
			if (acquire_failures < UINT8_MAX) {
				acquire_failures++;
			}
			next_state(LOC_CELL_LOOP, now_ms, "sky empty");
		}
		break;

	case LOC_GNSS_EXCLUSIVE:
		/* Same exits as GNSS_ACQUIRE minus the escalation (we're already
		 * escalated). Every exit re-activates LTE (lte_wanted goes true);
		 * a "fix acquired" then bounces through LTE_ATTACH while the
		 * modem re-registers -- expected, and the fix survives it. */
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix acquired");
		} else if (in_state > ACQUIRE_CAP_MS) {
			if (acquire_failures < UINT8_MAX) {
				acquire_failures++;
			}
			next_state(LOC_CELL_LOOP, now_ms, "acquire timeout");
		} else if (in_state > SKY_GRACE_MS && sky_empty) {
			if (acquire_failures < UINT8_MAX) {
				acquire_failures++;
			}
			next_state(LOC_CELL_LOOP, now_ms, "sky empty");
		}
		break;

	case LOC_REPORT_GNSS:
		/* Losing the fix is not the same as losing the ephemeris: while
		 * *visible* satellites still carry valid ephemeris, a re-fix is a
		 * hot start (seconds) and LTE need not be disturbed.
		 *
		 * There is deliberately NO proactive "ephemeris expiring" exit
		 * here. It existed once, fired only while the fix was CURRENT,
		 * and ACQUIRE's fix-valid exit bounced it straight back -- a
		 * 1 Hz transition ping-pong for the whole refresh window. It was
		 * also unnecessary: ephemeris subframes repeat every 30 s and
		 * the uplink is silent for ~50-120 s between flushes, so the
		 * receiver refreshes ephemeris during normal tracking; a lapsed
		 * one is recovered by the stale-fix exit below. */
		if (!out->gps_current) {
			if (sky_empty) {
				next_state(LOC_CELL_LOOP, now_ms, "sky lost");
			} else if (hotstart_dead) {
				next_state(LOC_GNSS_ACQUIRE, now_ms,
					   "fix stale, ephemeris not visible");
			}
			/* else: hot-start conditions present; wait for re-fix */
		} else if (stationary) {
			/* Parked with a good fix. motion.c already applied
			 * the entry hysteresis. */
			parked_had_fix = true;
			next_state(LOC_QUIESCENT, now_ms, "stationary");
		}
		break;

	case LOC_QUIESCENT:
		/* Sky evidence (empty/chopped streaks) is 1 Hz bookkeeping and
		 * is NOT consulted here. In IMU mode GNSS is OFF: every
		 * GNSS-evidence row below is legacy-only, because with the
		 * receiver stopped those inputs are a frozen frame, not
		 * observations. Departure is the stationary verdict, which
		 * the IMU interrupt flips via motion_note_imu(). */
		if (!stationary) {
			acquire_failures = 0;
			if (imu_wake) {
				/* LTE FIRST. The server must learn "your asset
				 * moved" within seconds so it can alert — and
				 * REPORT_CELL is exactly that: it publishes the
				 * alert + a free coarse cell position, lets the
				 * A-GNSS fetch run (ACQUIRE is radio-silent by
				 * design and would gag both), and hands off to
				 * ACQUIRE once the cell is out. The wake path
				 * IS the boot path. */
				cell_sent = false;
				next_state(LOC_REPORT_CELL, now_ms, "IMU wake");
			} else if (out->gps_current) {
				next_state(LOC_REPORT_GNSS, now_ms, "motion");
			} else {
				next_state(LOC_GNSS_ACQUIRE, now_ms, "motion");
			}
		} else if (imu_wake) {
			break; /* armed and quiet: nothing else can happen */
		} else if (fix) {
			/* A legacy check landed a fix: upgrade the flavor in
			 * place (the old REST bounced through REPORT_GNSS and
			 * back — two transitions narrating nothing). */
			parked_had_fix = true;
		} else if (parked_had_fix && !out->gps_current) {
			next_state(LOC_GNSS_ACQUIRE, now_ms, "checks not fixing");
		} else if (!parked_had_fix && sats_in_view) {
			next_state(LOC_GNSS_ACQUIRE, now_ms, "satellites sighted");
		} else if (!parked_had_fix && observed_window &&
			   (now_ms - rest_backoff_bumped_ms) >=
				(int64_t)rest_backoff_s * 1000) {
			/* An empty check completed: back off. Guarded by the
			 * elapsed interval so one wake's several PVT frames
			 * bump only once. */
			rest_backoff_bumped_ms = now_ms;
			rest_backoff_s = MIN(rest_backoff_s * 2,
				(uint32_t)CONFIG_TRACKER_REST_BACKOFF_MAX_S);
			LOG_DBG("loc: QUIESCENT backoff -> %u s", rest_backoff_s);
		}
		break;

	case LOC_CELL_LOOP:
		/* The exit is signal-driven: the slow cell cadence leaves gaps
		 * long enough for GNSS to sense the sky. One sighted satellite is
		 * enough to try -- entering ACQUIRE is cheap (a wrong entry
		 * self-corrects via "sky empty" in ~SKY_GRACE + a few epochs),
		 * while NOT entering is what starved acquisition forever: the
		 * next publish always interrupted before evidence could build. */
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix returned");
		} else if (stationary && acquire_failures >= 2) {
			/* Parked and failing: a teasing satellite must not
			 * restart the churn -- QUIESCENT's own sighting row
			 * still reacts, but at the backoff cadence. */
			rest_backoff_s = CONFIG_TRACKER_REST_BACKOFF_MIN_S;
			rest_backoff_bumped_ms = now_ms;
			parked_had_fix = false;
			next_state(LOC_QUIESCENT, now_ms, "stationary, repeated failures");
		} else if (sats_in_view) {
			next_state(LOC_GNSS_ACQUIRE, now_ms, "satellites sighted");
		}
		break;

	default:
		break;
	}

	out->state = state;
	if (!lte_registered && state != LOC_GNSS_EXCLUSIVE) {
		out->gnss_mode = LOC_GNSS_OFF;
	} else if (state == LOC_QUIESCENT) {
		/* IMU mode: GNSS off outright — no checks, no scheduled
		 * downloads, no parked fixes (the fake-trip class dies here).
		 * The accelerometer owns departure. */
		out->gnss_mode = imu_wake ? LOC_GNSS_OFF : LOC_GNSS_PERIODIC;
	} else {
		out->gnss_mode = LOC_GNSS_CONTINUOUS;
	}
	out->gnss_interval_s =
		(state != LOC_QUIESCENT || imu_wake) ? 0 :
		parked_had_fix ? CONFIG_TRACKER_QUIESCENT_CHECK_S
			       : rest_backoff_s;
	out->lte_wanted = (state != LOC_GNSS_EXCLUSIVE);
	out->publish_allowed = (state == LOC_REPORT_CELL ||
				state == LOC_REPORT_GNSS ||
				state == LOC_CELL_LOOP ||
				state == LOC_QUIESCENT);
	/* Fetch permission (see loc_fsm.h): publishing states with LTE already
	 * awake, or ACQUIRE while the inventory cannot support a fix — the
	 * radio-policy half of what lived in agnss.c's allowed-states list. */
	out->agnss_fetch_allowed =
		((state == LOC_REPORT_CELL || state == LOC_REPORT_GNSS ||
		  state == LOC_CELL_LOOP) && out->publish_allowed) ||
		(state == LOC_GNSS_ACQUIRE && inv->healthy < SATS_FOR_FIX);
	out->prefer_cell = (state != LOC_REPORT_GNSS &&
			    !(state == LOC_QUIESCENT && parked_had_fix &&
			      !imu_wake)) || !out->gps_current;
	out->publish_interval_ms =
		(state == LOC_CELL_LOOP)  ? CELL_POST_MS :
		(state != LOC_QUIESCENT)     ? POST_MS :
		(parked_had_fix && !imu_wake)
			? (uint32_t)CONFIG_TRACKER_QUIESCENT_CHECK_S * 1000
			: (uint32_t)CONFIG_TRACKER_REST_HEARTBEAT_S * 1000;
}

void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt,
		 const struct ephe_inventory *inv)
{
	if (st->gps_current) {
		LOG_DBG("loc:  %s | fix | ephemeris %u held / %u visible | next expiry %u min",
			loc_state_str(st->state), st->ephemeris_held,
			st->ephemeris_visible, st->min_ephe_expiry_min);
		return;
	}

	/* A satellite goes tracked -> strong (demodulable) -> ephemeris decoded
	 * -> counted toward a fix, which additionally needs geometry (pdop).
	 * Print the whole ladder so a stall is attributable to one rung.
	 * held != visible: held ephemerides may belong to satellites that have
	 * set since -- only visible ones can contribute to a fix. */
	LOG_DBG("loc:  %s | gps-time %s | tracked %u strong %u (empty %u/%u) | ephemeris %u held / %u visible (need %d) | pdop %.1f",
		loc_state_str(st->state), st->gps_time_known ? "known" : "UNKNOWN",
		st->tracked, st->strong, st->sky_empty, (unsigned)SKY_EMPTY_EPOCHS,
		st->ephemeris_held, st->ephemeris_visible,
		SATS_FOR_FIX, (double)st->pdop);

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		const struct nrf_modem_gnss_sv *sv = &pvt->sv[i];

		if (sv->sv == 0) {
			continue;
		}
		LOG_DBG("  sv %3u  C/N0 %.1f dB-Hz%s%s%s", sv->sv,
			(double)sv->cn0 / 10.0,
			sv->cn0 >= CN0_STRONG_01DBHZ ? "  strong" : "  weak",
			sv_has_ephemeris(inv, sv->sv) ? "  ephe" : "",
			(sv->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) ? "  used" : "");
	}
}
