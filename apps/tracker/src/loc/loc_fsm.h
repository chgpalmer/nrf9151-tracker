/*
 * Location policy: decides who gets the radio, GNSS or LTE.
 *
 * The nRF91 shares one RF front end. GNSS only runs when LTE leaves the radio
 * alone, and a cold start needs ~30 s of *uninterrupted* tracking to demodulate
 * each satellite's ephemeris off the air. Publishing every 10 s (plus a 60 s
 * MQTT keepalive) never leaves a gap that long, so GNSS tracks satellites
 * forever and never decodes them. See the ephemeris counter in loc_status.
 *
 * This module is deliberately ignorant of MQTT: it consumes PVT frames and says
 * whether it wants the radio quiet. main.c decides that means dropping the MQTT
 * connection (which also kills the keepalive). That keeps the policy pure and
 * buildable under native_sim.
 */
#ifndef LOC_FSM_H__
#define LOC_FSM_H__

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

enum loc_state {
	/* GNSS lacks ephemeris but can see enough sky to fetch it. Hold LTE off
	 * the air until it has a fix (or we give up). Also used to refresh
	 * ephemeris before it expires, which avoids a future cold start. */
	LOC_STATE_ACQUIRE,
	/* Fix in hand and ephemeris valid. Re-fixes are hot starts (seconds), so
	 * LTE can transmit freely. */
	LOC_STATE_REPORT,
	/* Not enough sky to be worth starving LTE for. Report the serving cell
	 * and re-test GNSS periodically. */
	LOC_STATE_CELL_ONLY,
};

struct loc_status {
	enum loc_state state;
	/* main.c must keep the radio off the air while this is true. */
	bool radio_quiet_wanted;
	/* Whether GNSS should be running at all. False while the modem is still
	 * searching for a network: a cell search uses the radio continuously, and
	 * GNSS running alongside it steals timeslots from the search (the modem
	 * reports NOT_ENOUGH_WINDOW_TIME) while itself getting nowhere. Register
	 * first, then hunt satellites. */
	bool gnss_wanted;
	/* A fix newer than CONFIG_TRACKER_GPS_STALE_S. */
	bool gps_current;

	uint8_t tracked;    /* satellites whose signal we follow */
	uint8_t strong;     /* ...of those, strong enough to demodulate */
	uint8_t used;       /* ...used in the position calculation */
	uint8_t ephemeris;  /* satellites whose orbit data we already hold */
	uint16_t min_ephe_expiry_min; /* 0 when we hold none */
	bool gps_time_known;
};

const char *loc_state_str(enum loc_state s);

void loc_fsm_init(int64_t now_ms);

/* main.c calls this once a serving-cell position has actually reached the
 * broker. Until then the FSM holds the radio for LTE at boot: a cold GNSS start
 * takes minutes, and going quiet first would leave the device off the map for
 * all of it. Keyed on the publish rather than a timer because LTE attach on a
 * roaming SIM can take longer than any timeout worth waiting. */
void loc_fsm_note_cell_sent(void);

/* Feed one PVT frame (or a stale one, if GNSS produced no event) and get the
 * current policy back. Call at the PVT rate; it is cheap and has no side
 * effects beyond its own state. */
void loc_fsm_update(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		    int64_t now_ms, bool lte_registered, struct loc_status *out);

/* Human-readable progress: which stage of the cold start we are at, plus C/N0
 * per satellite. Costs a modem call, so drive it from the status block. */
void loc_fsm_log(const struct loc_status *st,
		 const struct nrf_modem_gnss_pvt_data_frame *pvt);

#endif /* LOC_FSM_H__ */
