#include "loc_fsm.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(loc_fsm, CONFIG_TRACKER_LOG_LEVEL);

/* C/N0 is reported in 0.1 dB/Hz. Below roughly 27 dB-Hz a satellite can still
 * be tracked but its navigation message cannot be demodulated reliably, which
 * is the difference between "tracked" and "ephemeris". */
#define CN0_STRONG_01DBHZ (CONFIG_TRACKER_LOC_CN0_MIN_DBHZ * 10)

/* Four unknowns -- x, y, z and the receiver clock bias -- so four satellites,
 * each with a valid ephemeris to say where it was when it transmitted. */
#define SATS_FOR_FIX 4

#define BOOT_CELL_MAX_MS   (CONFIG_TRACKER_LOC_BOOT_CELL_S * 1000)
#define ACQUIRE_TIMEOUT_MS (CONFIG_TRACKER_LOC_ACQUIRE_TIMEOUT_S * 1000)
#define CELL_RETRY_MS      (CONFIG_TRACKER_LOC_CELL_RETRY_S * 1000)
#define GPS_STALE_MS       (CONFIG_TRACKER_GPS_STALE_S * 1000)

/* Give GNSS this long with a quiet radio to show us satellites before writing
 * the sky off. A cold receiver needs tens of seconds just to acquire signals,
 * before any ephemeris demodulation begins, so this cannot be tight. */
#define SKY_GRACE_MS 60000

/* Reading ephemeris state is a modem call; it does not change at 1 Hz. */
#define EPHE_POLL_MS 5000

static enum loc_state state;
static int64_t state_entered_ms;
static int64_t last_fix_ms;
static int64_t last_cell_retry_ms;
static int64_t last_ephe_poll_ms;
/* Last time the sky looked good enough. Satellite counts jitter second to
 * second, so decisions must be made on "hasn't been good for a while", never on
 * a single sample -- otherwise one unlucky epoch discards a usable sky. */
static int64_t last_sky_ok_ms;
/* Boot: hold the radio for LTE until one cell position is actually published. */
static bool boot_cell_pending;
static bool boot_cell_sent;

const char *loc_state_str(enum loc_state s)
{
	switch (s) {
	case LOC_STATE_ACQUIRE:   return "ACQUIRE";
	case LOC_STATE_REPORT:    return "REPORT";
	case LOC_STATE_CELL_ONLY: return "CELL_ONLY";
	default:                  return "?";
	}
}

void loc_fsm_init(int64_t now_ms)
{
	/* Start in CELL_ONLY, not ACQUIRE. At boot we are cold, so GNSS is minutes
	 * away; going quiet immediately would leave the device off the map for that
	 * whole time. Report the serving cell first (coarse location now, precise
	 * later), then hand the radio to GNSS.
	 *
	 * We leave as soon as that cell position is actually published -- not on a
	 * timer. LTE attach on a roaming SIM routinely takes longer than any
	 * timeout worth waiting, and a timer would expire mid-attach and go quiet
	 * having reported nothing. BOOT_CELL_S is only a backstop for the case
	 * where the modem never attaches at all. */
	state = LOC_STATE_CELL_ONLY;
	state_entered_ms   = now_ms;
	last_fix_ms        = now_ms - GPS_STALE_MS;
	last_cell_retry_ms = now_ms;
	last_ephe_poll_ms  = now_ms - EPHE_POLL_MS;
	last_sky_ok_ms     = now_ms;
	boot_cell_pending  = true;
	boot_cell_sent     = false;
}

void loc_fsm_note_cell_sent(void)
{
	boot_cell_sent = true;
}

static void enter(enum loc_state next, int64_t now_ms, const char *why)
{
	if (next == state) {
		return;
	}
	LOG_INF("loc: %s -> %s (%s)", loc_state_str(state), loc_state_str(next), why);
	state = next;
	state_entered_ms = now_ms;
	/* Fresh start: don't judge the new state on sky history from the old one. */
	last_sky_ok_ms = now_ms;
}

/* Ask GNSS which ephemerides it holds. This is the only reliable "how close to
 * a fix am I" signal: tracked/used cannot distinguish "no signal" from "signal
 * too weak to decode". */
static void read_ephemeris(struct loc_status *st, int64_t now_ms)
{
	/* ~800 B: too big for the main stack. */
	static struct nrf_modem_gnss_agnss_expiry exp;
	/* Cached so a throttled call still reports the last known truth rather
	 * than zeroes, which the state machine would read as "ephemeris lost". */
	static uint8_t  cached_ephemeris;
	static uint16_t cached_min_expiry;
	static bool     cached_time_known;

	if ((now_ms - last_ephe_poll_ms) < EPHE_POLL_MS) {
		st->ephemeris = cached_ephemeris;
		st->min_ephe_expiry_min = cached_min_expiry;
		st->gps_time_known = cached_time_known;
		return;
	}
	last_ephe_poll_ms = now_ms;

	st->ephemeris = 0;
	st->min_ephe_expiry_min = 0;
	st->gps_time_known = false;

	if (nrf_modem_gnss_agnss_expiry_get(&exp) != 0) {
		/* Keep the cache: a failed read is not evidence of loss. */
		st->ephemeris = cached_ephemeris;
		st->min_ephe_expiry_min = cached_min_expiry;
		st->gps_time_known = cached_time_known;
		return;
	}

	st->gps_time_known =
		!(exp.data_flags & NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST);

	for (int i = 0; i < exp.sv_count; i++) {
		uint16_t e = exp.sv[i].ephe_expiry;

		if (e == 0) {
			continue;
		}
		st->ephemeris++;
		if (st->min_ephe_expiry_min == 0 || e < st->min_ephe_expiry_min) {
			st->min_ephe_expiry_min = e;
		}
	}

	cached_ephemeris  = st->ephemeris;
	cached_min_expiry = st->min_ephe_expiry_min;
	cached_time_known = st->gps_time_known;
}

static void read_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			    struct loc_status *st)
{
	st->tracked = 0;
	st->strong  = 0;
	st->used    = 0;

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
	}
}

void loc_fsm_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		    int64_t now_ms, bool lte_registered, struct loc_status *out)
{
	bool fix = pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;

	if (fix) {
		last_fix_ms = now_ms;
	}

	read_satellites(pvt, out);
	read_ephemeris(out, now_ms);
	out->gps_current = (now_ms - last_fix_ms) < GPS_STALE_MS;

	/* Enough sky to be worth starving LTE for? Ephemeris demodulation needs
	 * SATS_FOR_FIX satellites above the C/N0 floor, not merely tracked ones. */
	bool sky_ok = out->strong >= SATS_FOR_FIX;
	bool have_ephe = out->ephemeris >= SATS_FOR_FIX;
	int64_t in_state = now_ms - state_entered_ms;

	if (sky_ok) {
		last_sky_ok_ms = now_ms;
	}
	/* Sustained, not instantaneous: the sky has been inadequate for the whole
	 * grace period. Sampling sky_ok alone at the deadline would bin a good sky
	 * on one bad epoch. */
	bool sky_dead = (now_ms - last_sky_ok_ms) > SKY_GRACE_MS;

	switch (state) {
	case LOC_STATE_ACQUIRE:
		if (fix) {
			enter(LOC_STATE_REPORT, now_ms, "fix acquired");
		} else if (in_state > ACQUIRE_TIMEOUT_MS) {
			last_cell_retry_ms = now_ms;
			enter(LOC_STATE_CELL_ONLY, now_ms, "acquire timeout");
		} else if (sky_dead && in_state > SKY_GRACE_MS) {
			last_cell_retry_ms = now_ms;
			enter(LOC_STATE_CELL_ONLY, now_ms, "not enough sky");
		}
		break;

	case LOC_STATE_REPORT:
		/* Losing the fix is not the same as losing the ephemeris. While
		 * ephemeris is valid a re-fix is a hot start (seconds), so there
		 * is no reason to take the radio away from LTE. Only go quiet
		 * when the orbit data itself is missing or about to expire. */
		if (!out->gps_current && !have_ephe) {
			enter(sky_dead ? LOC_STATE_CELL_ONLY : LOC_STATE_ACQUIRE,
			      now_ms, "ephemeris lost");
		} else if (have_ephe && !sky_dead &&
			   out->min_ephe_expiry_min <= CONFIG_TRACKER_LOC_EPHE_REFRESH_MIN) {
			enter(LOC_STATE_ACQUIRE, now_ms, "ephemeris expiring");
		}
		break;

	case LOC_STATE_CELL_ONLY:
		/* Do NOT gate this on sky_ok. CELL_ONLY lets LTE transmit freely,
		 * which starves GNSS, so `strong` reads 0 no matter how clear the
		 * sky is -- the condition could never become true and the FSM would
		 * be stuck here forever. Instead probe: drop into ACQUIRE on a timer
		 * and let its own grace period decide. A wasted probe costs one quiet
		 * window; never probing costs every GNSS fix. */
		if (fix) {
			boot_cell_pending = false;
			enter(LOC_STATE_REPORT, now_ms, "fix returned");
		} else if (boot_cell_pending) {
			/* Hold until the coarse position is on the map. The backstop
			 * clock only starts once we are registered -- an unregistered
			 * modem is mid-cell-search, and timing that out would hand the
			 * radio to a GNSS that cannot use it anyway. */
			if (boot_cell_sent ||
			    (lte_registered && in_state > BOOT_CELL_MAX_MS)) {
				boot_cell_pending = false;
				last_cell_retry_ms = now_ms;
				enter(LOC_STATE_ACQUIRE, now_ms,
				      boot_cell_sent ? "cell reported" : "cell unavailable");
			}
		} else if ((now_ms - last_cell_retry_ms) > CELL_RETRY_MS) {
			last_cell_retry_ms = now_ms;
			enter(LOC_STATE_ACQUIRE, now_ms, "probing for sky");
		}
		break;
	}

	out->state = state;
	/* Only ACQUIRE needs the air. REPORT has ephemeris; CELL_ONLY has no sky
	 * and would gain nothing from silence. */
	out->radio_quiet_wanted = (state == LOC_STATE_ACQUIRE);

	/* An unregistered modem is running a continuous cell search -- the most
	 * radio-hungry thing it does. GNSS alongside it steals timeslots from the
	 * search and still gets nothing: satellites are acquired, then dropped the
	 * moment the search reclaims the radio. Register first. */
	out->gnss_wanted = lte_registered;
}

void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	if (st->gps_current) {
		LOG_INF("loc:  %s | fix | ephemeris %u sv, expires in %u min",
			loc_state_str(st->state), st->ephemeris,
			st->min_ephe_expiry_min);
		return;
	}

	/* A satellite goes tracked -> (strong enough to demodulate) -> ephemeris
	 * -> counted toward a fix. Print all four so a stall is attributable. */
	LOG_INF("loc:  %s | gps-time %s | tracked %u, strong %u, ephemeris %u/%d",
		loc_state_str(st->state), st->gps_time_known ? "known" : "UNKNOWN",
		st->tracked, st->strong, st->ephemeris, SATS_FOR_FIX);

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		const struct nrf_modem_gnss_sv *sv = &pvt->sv[i];

		if (sv->sv == 0) {
			continue;
		}
		LOG_INF("  sv %3u  C/N0 %.1f dB-Hz%s%s", sv->sv,
			(double)sv->cn0 / 10.0,
			sv->cn0 >= CN0_STRONG_01DBHZ ? "  strong" : "  weak",
			(sv->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) ? "  used" : "");
	}
}
