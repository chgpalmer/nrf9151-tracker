/*
 * Location policy: decides who gets the radio, GNSS or LTE.
 *
 * The nRF91 shares one RF front end. GNSS only runs when LTE leaves the radio
 * alone, and a cold start needs ~30 s of *uninterrupted* tracking to demodulate
 * each satellite's ephemeris off the air. Publishing every 10 s (plus a 60 s
 * MQTT keepalive) never leaves a gap that long, so GNSS tracks satellites
 * forever and never decodes them.
 *
 * This module is deliberately ignorant of MQTT: it consumes PVT frames plus the
 * LTE registration state and emits a policy (should GNSS run, may we publish,
 * what kind, how often). main.c executes the policy. That keeps the FSM pure
 * and buildable under native_sim.
 */
#ifndef LOC_FSM_H__
#define LOC_FSM_H__

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

/* States are modes with distinct policy (is GNSS running? may we publish?
 * which source? what cadence?). Events like "got a fix" or "timed out" are
 * NOT states -- they are the labelled edges between states, carried by the
 * reason string in the transition log. A zero-dwell pass-through state would
 * add trace lines without adding information.
 *
 * Edge labels below are the exact reason strings from the transition log, so
 * a UART trace can be read against this diagram.
 *
 *                    (from ANY state except GNSS_EXCLUSIVE)
 *                             | "registration lost"
 *                             v
 *   +----------------------------------------------------------------+
 *   | LTE_ATTACH        GNSS off, no publishes.                       |
 *   |                   A searching modem owns the radio; GNSS beside |
 *   |                   it starves the search and gets nothing.       |
 *   +----------------------------------------------------------------+
 *                             | "registered"
 *                             v
 *   +----------------------------------------------------------------+
 *   | REPORT_CELL       GNSS on, publish cell @ POST_INTERVAL_S.      |
 *   |                   Coarse position onto the map first -- a cold  |
 *   |                   GNSS start takes minutes of radio silence.    |
 *   +----------------------------------------------------------------+
 *          | "cell reported" (publish confirmed,        | "fix acquired"
 *          |  not a timer)                              | (warm start)
 *          v                                            v REPORT_GNSS
 *   +----------------------------------------------------------------+
 *   | GNSS_ACQUIRE      GNSS on, NO publishes (app radio-silent;      |
 *   |                   LTE stays registered, PSM parks the radio).   |
 *   |                   Hunting ephemeris: ~30 s of uninterrupted     |
 *   |                   tracking per satellite, needs 4.              |
 *   +----------------------------------------------------------------+
 *      | "fix acquired"     | "LTE chops GNSS"        | "sky empty"
 *      |                    |  (NOT_ENOUGH_WINDOW_    |  (SKY_EMPTY_EPOCHS
 *      |                    |   TIME persists: idle   |   observed-empty,
 *      |                    |   DRX/reselection is    |   after 30 s cold-
 *      |                    |   slicing GNSS even     |   start grace)
 *      |                    |   though we're silent)  | "acquire timeout"
 *      |                    v                         |  (ACQUIRE_TIMEOUT_S:
 *      |   +-------------------------------------+    |   sky teases, no fix)
 *      |   | GNSS_EXCLUSIVE   GNSS on, LTE OFF.  |    |
 *      |   | Registration deliberately lapses;   |    |
 *      |   | GNSS owns the radio outright. The   |    |
 *      |   | re-attach bill is paid after the    |    |
 *      |   | fix, when it's amortized.           |    |
 *      |   +-------------------------------------+    |
 *      |      | "fix acquired"     | "sky empty" /    |
 *      |      | (then LTE          | "acquire timeout"|
 *      |      |  reactivates;      | (LTE reactivates)|
 *      |      |  expect a bounce   |                  |
 *      |      |  through           |                  |
 *      |      |  LTE_ATTACH)       |                  |
 *      v      v                    v                  v
 *   +---------------------+     +-----------------------------------+
 *   | REPORT_GNSS         |     | CELL_LOOP                         |
 *   | GNSS on, publish    |     | GNSS on, publish cell @           |
 *   | fix @ POST_INTERVAL |     | CELL_POST_INTERVAL_S (slow on     |
 *   | (stale fix -> cell  |     | purpose: the gaps are how GNSS    |
 *   |  until re-fix)      |     | gets to sense the sky)            |
 *   +---------------------+     +-----------------------------------+
 *      |         |                  ^      | "satellites sighted"
 *      |         | "sky lost"       |      |   (ONE observed epoch with
 *      |         | (fix stale +     |      |    tracked > 0: chopped
 *      |         |  sky empty) -----+      |    windows understate the
 *      |         |                         |    sky, so any life earns a
 *      |         |                         |    real attempt)
 *      |         |                         | "fix returned" -> REPORT_GNSS
 *      |         |                         v GNSS_ACQUIRE
 *      |
 *      | "fix stale, ephemeris not visible" (can't hot-start: fewer than
 *      |   4 ephemeris-bearing satellites observed for VISIBLE_LOST_MS)
 *      +--> GNSS_ACQUIRE
 *
 * There is deliberately no proactive ephemeris-refresh exit from REPORT_GNSS:
 * it ping-ponged with ACQUIRE's fix-valid exit at 1 Hz (the fix was current,
 * so the very next PVT bounced back). Refresh happens for free during normal
 * tracking -- subframes repeat every 30 s and the uplink is silent between
 * flushes -- and a genuinely lapsed ephemeris recovers via the stale-fix exit.
 *
 * Sky evidence rules (the fallacy that bit us three times): only OBSERVED
 * epochs count. tracked > 0 is an observation by definition; an empty frame
 * counts only if GNSS actually had the radio (DEADLINE_MISSED clear). Blanked
 * epochs count toward NEITHER verdict -- absence of observation is not
 * observation of absence, in either direction. Asymmetry is deliberate:
 * trying costs little (wrong entries self-correct via "sky empty"), so entry
 * needs just one sighting, while giving up needs sustained observed evidence.
 */
enum loc_state {
	/* Modem not registered. A cell search uses the radio continuously, and
	 * GNSS alongside it steals timeslots from the search while itself getting
	 * nowhere -- so GNSS is off entirely until registration. */
	LOC_LTE_ATTACH,
	/* Registered; get one coarse serving-cell position onto the map before
	 * handing the radio to GNSS (which may then go quiet for minutes). */
	LOC_REPORT_CELL,
	/* Hunting ephemeris. No publishes at all: UDP is silent between sends,
	 * so zero application traffic means PSM parks LTE and GNSS owns the
	 * radio -- in good coverage. */
	LOC_GNSS_ACQUIRE,
	/* Escalation of GNSS_ACQUIRE for marginal coverage: LTE is deactivated
	 * entirely. Even application-silent, an idle registered modem does DRX
	 * paging and (at the coverage edge) constant reselection, chopping GNSS
	 * into sub-second slices -- the modem reports NOT_ENOUGH_WINDOW_TIME and
	 * a cold start can never complete. Registration is deliberately let
	 * lapse; the re-attach is paid after the fix, when it is amortized. */
	LOC_GNSS_EXCLUSIVE,
	/* Fix in hand and ephemeris visible. Re-fixes are hot starts (seconds),
	 * so LTE may transmit freely; publishes carry the GNSS position. */
	LOC_REPORT_GNSS,
	/* Not enough sky to be worth starving LTE for. Report cells on a slow
	 * cadence whose gaps are long enough for GNSS to *sense* the sky, so the
	 * exit is signal-driven rather than a blind timer. */
	LOC_CELL_LOOP,
	/* Parked with a held fix (stationary input from motion.c). GNSS drops
	 * to periodic position checks -- the modem sleeps between them -- and
	 * the check that lands outside the parked radius flips the stationary
	 * verdict, which snaps us back to REPORT_GNSS at 1 Hz. */
	LOC_QUIESCENT,

	LOC_STATE_COUNT,
};

/* How GNSS should run. PERIODIC uses the modem's native periodic-navigation
 * mode (fix_interval > 1): the modem duty-cycles itself and schedules its
 * own ephemeris upkeep, so a parked tracker idles at uA instead of the
 * ~40 mA of continuous tracking. */
enum loc_gnss_mode {
	LOC_GNSS_OFF,
	LOC_GNSS_CONTINUOUS,
	LOC_GNSS_PERIODIC,
};

struct loc_status {
	enum loc_state state;

	/* How GNSS should run right now. OFF while unregistered (a searching
	 * modem owns the radio) -- except in GNSS_EXCLUSIVE, where being
	 * unregistered is the whole point. PERIODIC only in the parked states;
	 * gnss_interval_s is the seconds between position checks then. */
	enum loc_gnss_mode gnss_mode;
	uint16_t gnss_interval_s;
	/* Should the LTE stack be active? False only in GNSS_EXCLUSIVE; main.c
	 * maps edges to LTE_LC_FUNC_MODE_{DE,}ACTIVATE_LTE. */
	bool lte_wanted;
	/* May main.c publish (and do socket setup)? False in ACQUIRE:
	 * application silence is what hands the radio to GNSS. */
	bool publish_allowed;
	/* What to publish: serving cell (true) or the GNSS fix (false). */
	bool prefer_cell;
	/* Publish cadence for the current state. */
	uint32_t publish_interval_ms;

	/* A fix newer than CONFIG_TRACKER_GPS_STALE_S. */
	bool gps_current;

	uint8_t tracked;   /* satellites whose signal we follow */
	uint8_t strong;    /* ...of those, strong enough to demodulate */
	uint8_t used;      /* ...used in the position calculation */
	/* Consecutive observed epochs with zero satellites; at
	 * TRACKER_LOC_SKY_EMPTY_EPOCHS the sky is declared empty. Logged so a
	 * trace shows progress toward that exit, not just the exit. */
	uint8_t sky_empty;

	/* Ephemerides held (cached orbit data, valid ~2 h) vs held AND currently
	 * tracked. Held counts satellites that may be below the horizon by now;
	 * only visible ones can contribute to a fix. This distinction is why
	 * "ephemeris 4/4 but no fix" is not a contradiction. */
	uint8_t ephemeris_held;
	uint8_t ephemeris_visible;
	uint16_t min_ephe_expiry_min; /* 0 when none held */
	bool gps_time_known;

	/* Position dilution of precision: satellite geometry quality. Four
	 * satellites bunched together give a valid-looking count and an unusable
	 * solution; this is the number that says so. */
	float pdop;
};

const char *loc_state_str(enum loc_state s);

void loc_fsm_init(int64_t now_ms);

/* main.c calls this once a serving-cell position has actually reached the
 * broker. CELL_REPORT holds the radio for LTE until then -- keyed on the
 * publish rather than a timer because LTE attach on a roaming SIM can take
 * longer than any timeout worth waiting. */
void loc_fsm_note_cell_sent(void);

/* Feed one PVT frame (or a stale one, if GNSS produced no event) and get the
 * current policy back. Call at the PVT rate; it is cheap and has no side
 * effects beyond its own state. */
void loc_fsm_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		    int64_t now_ms, bool lte_registered, bool stationary,
		    struct loc_status *out);

/* Human-readable progress: which stage of the cold start we are at, plus C/N0
 * per satellite. Drive it from the status block, not the PVT rate. */
void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt);

#endif /* LOC_FSM_H__ */
