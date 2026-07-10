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
 *                          (from ANY state)
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
 *          v                                            v
 *   +----------------------------------------------------------------+
 *   | GNSS_ACQUIRE      GNSS on, NO publishes (radio silent).         |
 *   |                   Hunting ephemeris: ~30 s of uninterrupted     |
 *   |                   tracking per satellite, needs 4.              |
 *   +----------------------------------------------------------------+
 *      | "fix acquired"        ^        | "not enough sky"
 *      |                       |        |   (SKY_LOST_S without one
 *      |     "sky returned"    |        |    observed-good epoch)
 *      |     (sky streak hits  |        | "acquire timeout"
 *      |      SKY_OK_S good    |        |   (ACQUIRE_TIMEOUT_S cap:
 *      |      observed epochs; |        |    sky teases but no fix)
 *      |      pauses while LTE |        |
 *      |      blanks GNSS)     |        |
 *      v                       |        v
 *   +---------------------+    |    +-----------------------------------+
 *   | REPORT_GNSS         |    +----| CELL_LOOP                         |
 *   | GNSS on, publish    |         | GNSS on, publish cell @           |
 *   | fix @ POST_INTERVAL |         | CELL_POST_INTERVAL_S (slow on     |
 *   | (stale fix -> cell  |         | purpose: the ~25 s gaps are how   |
 *   |  until re-fix)      |         | GNSS gets to sense the sky)       |
 *   +---------------------+         +-----------------------------------+
 *      |         |                      ^         | "fix returned"
 *      |         | "sky lost"           |         v REPORT_GNSS
 *      |         | (fix stale +         |
 *      |         |  SKY_LOST_S) --------+
 *      |
 *      | "ephemeris expiring" (< EPHE_REFRESH_MIN left; refresh now to
 *      |   avoid a future cold start)
 *      | "fix stale, ephemeris not visible" (can't hot-start: fewer than
 *      |   4 ephemeris-bearing satellites observed for VISIBLE_LOST_MS)
 *      +--> GNSS_ACQUIRE
 *
 * Sky evidence rules (the part that bit us three times): all judgements use
 * OBSERVED epochs only -- an epoch where LTE blanked GNSS (tracked == 0 right
 * after a publish) is absence of observation, not observation of absence.
 * Entering ACQUIRE needs positive evidence (a streak of good observed
 * epochs); leaving it needs sustained lack of any (SKY_LOST_S) -- asymmetric
 * on purpose, so a device in a Faraday cage never reads as "sky returned",
 * and a publish never resets progress toward an exit.
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
	 * radio. */
	LOC_GNSS_ACQUIRE,
	/* Fix in hand and ephemeris visible. Re-fixes are hot starts (seconds),
	 * so LTE may transmit freely; publishes carry the GNSS position. */
	LOC_REPORT_GNSS,
	/* Not enough sky to be worth starving LTE for. Report cells on a slow
	 * cadence whose gaps are long enough for GNSS to *sense* the sky, so the
	 * exit is signal-driven rather than a blind timer. */
	LOC_CELL_LOOP,

	LOC_STATE_COUNT,
};

struct loc_status {
	enum loc_state state;

	/* Should GNSS be running at all? False while unregistered. */
	bool gnss_wanted;
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
	/* Consecutive observed-good epochs; at TRACKER_LOC_SKY_OK_S the sky is
	 * declared usable (CELL_LOOP -> GNSS_ACQUIRE). Logged so a trace shows
	 * progress toward that exit, not just the exit. */
	uint8_t sky_streak;

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
		    int64_t now_ms, bool lte_registered, struct loc_status *out);

/* Human-readable progress: which stage of the cold start we are at, plus C/N0
 * per satellite. Drive it from the status block, not the PVT rate. */
void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt);

#endif /* LOC_FSM_H__ */
