#include "loc_fsm.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(loc_fsm, CONFIG_TRACKER_LOG_LEVEL);

/* C/N0 is reported in 0.1 dB/Hz. Below roughly 27 dB-Hz a satellite can still
 * be tracked but its navigation message cannot be demodulated reliably, which
 * is the difference between "tracked" and "ephemeris". */
#define CN0_STRONG_01DBHZ (CONFIG_TRACKER_LOC_CN0_MIN_DBHZ * 10)

/* Four unknowns -- x, y, z and the receiver clock bias -- so four satellites,
 * each with a valid ephemeris to say where it was when it transmitted. */
#define SATS_FOR_FIX 4

#define ACQUIRE_CAP_MS   (CONFIG_TRACKER_LOC_ACQUIRE_TIMEOUT_S * 1000)
#define GPS_STALE_MS     (CONFIG_TRACKER_GPS_STALE_S * 1000)
#define POST_MS          (CONFIG_TRACKER_POST_INTERVAL_S * 1000)
#define CELL_POST_MS     (CONFIG_TRACKER_CELL_POST_INTERVAL_S * 1000)
/* Sky judgements are debounced, never instantaneous: satellite counts jitter
 * epoch to epoch, and one unlucky sample must not discard a usable sky (nor one
 * lucky sample start an acquisition). */
#define SKY_LOST_MS      (CONFIG_TRACKER_LOC_SKY_LOST_S * 1000)
#define SKY_OK_MS        (CONFIG_TRACKER_LOC_SKY_OK_S * 1000)

/* Reading ephemeris state is a modem call; it does not change at 1 Hz. */
#define EPHE_POLL_MS 5000

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
	[LOC_REPORT_GNSS] = "REPORT_GNSS",
	[LOC_CELL_LOOP]   = "CELL_LOOP",
};
BUILD_ASSERT(ARRAY_SIZE(loc_state_names) == LOC_STATE_COUNT,
	     "loc_state_names out of step with enum loc_state");

static enum loc_state state;
static int64_t state_entered_ms;
static int64_t last_fix_ms;
static int64_t last_ephe_poll_ms;
static int64_t last_sky_ok_ms;
static int64_t last_sky_bad_ms;
/* Last *observed* epoch with enough ephemeris-bearing satellites in view. */
static int64_t last_visible_ok_ms;
static bool cell_sent;

/* Cached ephemeris inventory, refreshed at most every EPHE_POLL_MS. Kept as the
 * raw frame so `visible` can be recomputed against every PVT frame. ~800 B, so
 * static rather than on a stack. */
static struct nrf_modem_gnss_agnss_expiry ephe;
static bool ephe_valid;

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
	last_sky_ok_ms      = now_ms;
	last_sky_bad_ms     = now_ms;
	last_visible_ok_ms  = now_ms;
}

void loc_fsm_init(int64_t now_ms)
{
	state = LOC_LTE_ATTACH;
	state_entered_ms  = now_ms;
	last_fix_ms       = now_ms - GPS_STALE_MS;
	last_ephe_poll_ms = now_ms - EPHE_POLL_MS;
	last_sky_ok_ms     = now_ms;
	last_sky_bad_ms    = now_ms;
	last_visible_ok_ms = now_ms;
	cell_sent = false;
	ephe_valid = false;
}

void loc_fsm_note_cell_sent(void)
{
	cell_sent = true;
}

static void poll_ephemeris(int64_t now_ms)
{
	static struct nrf_modem_gnss_agnss_expiry fresh;

	if ((now_ms - last_ephe_poll_ms) < EPHE_POLL_MS) {
		return;
	}
	last_ephe_poll_ms = now_ms;

	/* On failure keep the previous inventory: a failed read is not evidence
	 * that the ephemerides were lost. */
	if (nrf_modem_gnss_agnss_expiry_get(&fresh) == 0) {
		ephe = fresh;
		ephe_valid = true;
	}
}

/* Does the cached inventory hold a usable ephemeris for this satellite? */
static bool sv_has_ephemeris(uint16_t sv_id)
{
	if (!ephe_valid) {
		return false;
	}
	for (int i = 0; i < ephe.sv_count; i++) {
		/* Numeric match is sufficient: with GPS(+QZSS) enabled the ID
		 * spaces don't collide (GPS 1..32, QZSS 193..202). */
		if (ephe.sv[i].sv_id == sv_id) {
			return ephe.sv[i].ephe_expiry > 0;
		}
	}
	return false;
}

static void read_sky(const struct nrf_modem_gnss_pvt_data_frame *pvt,
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
		if (sv_has_ephemeris(sv->sv)) {
			visible++;
		}
	}

	if (st->tracked > 0) {
		last_visible = visible;
	}
	st->ephemeris_visible = last_visible;

	st->ephemeris_held = 0;
	st->min_ephe_expiry_min = 0;
	st->gps_time_known = false;
	if (ephe_valid) {
		st->gps_time_known = !(ephe.data_flags &
			NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST);
		for (int i = 0; i < ephe.sv_count; i++) {
			uint16_t e = ephe.sv[i].ephe_expiry;

			if (e == 0) {
				continue;
			}
			st->ephemeris_held++;
			if (st->min_ephe_expiry_min == 0 ||
			    e < st->min_ephe_expiry_min) {
				st->min_ephe_expiry_min = e;
			}
		}
	}

	st->pdop = pvt->pdop;
}

void loc_fsm_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		    int64_t now_ms, bool lte_registered, struct loc_status *out)
{
	bool fix = pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;

	if (fix) {
		last_fix_ms = now_ms;
	}

	poll_ephemeris(now_ms);
	read_sky(pvt, out);
	out->gps_current = (now_ms - last_fix_ms) < GPS_STALE_MS;

	/* Debounced sky quality. "Enough sky" means enough satellites above the
	 * demodulation floor to eventually earn SATS_FOR_FIX ephemerides. */
	if (out->strong >= SATS_FOR_FIX) {
		last_sky_ok_ms = now_ms;
	} else {
		last_sky_bad_ms = now_ms;
	}
	bool sky_dead  = (now_ms - last_sky_ok_ms) > SKY_LOST_MS;
	bool sky_alive = (now_ms - last_sky_bad_ms) > SKY_OK_MS;

	/* Hot-start viability, debounced on observed epochs only (blanked epochs
	 * carry the cached count and must not refresh the clock either way). */
	if (out->tracked > 0 && out->ephemeris_visible >= SATS_FOR_FIX) {
		last_visible_ok_ms = now_ms;
	}
	bool hotstart_dead = (now_ms - last_visible_ok_ms) > VISIBLE_LOST_MS;
	int64_t in_state = now_ms - state_entered_ms;

	/* Registration loss overrides everything: an unregistered modem runs a
	 * continuous cell search, which both starves GNSS and is starved by it. */
	if (!lte_registered && state != LOC_LTE_ATTACH) {
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
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix acquired");
		} else if (in_state > ACQUIRE_CAP_MS) {
			next_state(LOC_CELL_LOOP, now_ms, "acquire timeout");
		} else if (sky_dead) {
			next_state(LOC_CELL_LOOP, now_ms, "not enough sky");
		}
		break;

	case LOC_REPORT_GNSS:
		/* Losing the fix is not the same as losing the ephemeris: while
		 * *visible* satellites still carry valid ephemeris, a re-fix is a
		 * hot start (seconds) and LTE need not be disturbed. */
		if (!out->gps_current) {
			if (sky_dead) {
				next_state(LOC_CELL_LOOP, now_ms, "sky lost");
			} else if (hotstart_dead) {
				next_state(LOC_GNSS_ACQUIRE, now_ms,
					   "fix stale, ephemeris not visible");
			}
			/* else: hot-start conditions present; wait for re-fix */
		} else if (out->ephemeris_held > 0 && !sky_dead &&
			   out->min_ephe_expiry_min <=
				CONFIG_TRACKER_LOC_EPHE_REFRESH_MIN) {
			/* Refresh before it lapses: one quiet window now avoids a
			 * full cold start later. */
			next_state(LOC_GNSS_ACQUIRE, now_ms, "ephemeris expiring");
		}
		break;

	case LOC_CELL_LOOP:
		/* The exit is signal-driven: the slow cell cadence leaves gaps
		 * long enough for GNSS to sense the sky, so no blind probe timer
		 * is needed. */
		if (fix) {
			next_state(LOC_REPORT_GNSS, now_ms, "fix returned");
		} else if (sky_alive) {
			next_state(LOC_GNSS_ACQUIRE, now_ms, "sky returned");
		}
		break;

	default:
		break;
	}

	out->state = state;
	out->gnss_wanted = lte_registered;
	out->publish_allowed = (state == LOC_REPORT_CELL ||
				state == LOC_REPORT_GNSS ||
				state == LOC_CELL_LOOP);
	out->prefer_cell = (state != LOC_REPORT_GNSS) || !out->gps_current;
	out->publish_interval_ms = (state == LOC_CELL_LOOP) ? CELL_POST_MS : POST_MS;
}

void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	if (st->gps_current) {
		LOG_INF("loc:  %s | fix | ephemeris %u held / %u visible | next expiry %u min",
			loc_state_str(st->state), st->ephemeris_held,
			st->ephemeris_visible, st->min_ephe_expiry_min);
		return;
	}

	/* A satellite goes tracked -> strong (demodulable) -> ephemeris decoded
	 * -> counted toward a fix, which additionally needs geometry (pdop).
	 * Print the whole ladder so a stall is attributable to one rung.
	 * held != visible: held ephemerides may belong to satellites that have
	 * set since -- only visible ones can contribute to a fix. */
	LOG_INF("loc:  %s | gps-time %s | tracked %u strong %u | ephemeris %u held / %u visible (need %d) | pdop %.1f",
		loc_state_str(st->state), st->gps_time_known ? "known" : "UNKNOWN",
		st->tracked, st->strong, st->ephemeris_held, st->ephemeris_visible,
		SATS_FOR_FIX, (double)st->pdop);

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		const struct nrf_modem_gnss_sv *sv = &pvt->sv[i];

		if (sv->sv == 0) {
			continue;
		}
		LOG_INF("  sv %3u  C/N0 %.1f dB-Hz%s%s%s", sv->sv,
			(double)sv->cn0 / 10.0,
			sv->cn0 >= CN0_STRONG_01DBHZ ? "  strong" : "  weak",
			sv_has_ephemeris(sv->sv) ? "  ephe" : "",
			(sv->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) ? "  used" : "");
	}
}
