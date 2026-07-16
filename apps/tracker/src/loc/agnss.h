/*
 * A-GNSS: fetch assistance over CoAP and inject it into the modem.
 *
 * Cuts acquisition from minutes (50 bps ephemeris demodulation, which motion
 * keeps corrupting) to seconds: the server pre-scales IGS broadcast
 * ephemeris + GPS time + a coarse position into exactly the integers
 * nrf_modem_gnss_agnss_write() wants (proto/tracker.proto, AgnssData).
 *
 * Pull-only and best-effort by design: one POST /agnss exchange on the
 * already-connected CoAP socket, triggered when the modem's own need report
 * says the inventory is thin, rate-limited, and only in states where LTE is
 * already awake — never during ACQUIRE/EXCLUSIVE (their radio silence is the
 * point) and never in the parked states (their radio sleep is the point).
 * If the server is unreachable, behaviour degrades to exactly today's.
 */
#ifndef AGNSS_H__
#define AGNSS_H__

#include <stdint.h>
#include "loc_fsm.h"

#if defined(CONFIG_TRACKER_AGNSS)

/* The request carries the device id so the server can seed a location and
 * elevation-filter the ephemerides. Pointer is kept, not copied. */
void agnss_set_device_id(const char *id);

/* Evaluate need (from the shared inventory digest) against the FSM's fetch
 * permission (loc->agnss_fetch_allowed) and, at most once per pass, run one
 * fetch+inject exchange (blocks <= ~2.5 s while LTE is already active).
 * Call once per main-loop pass, after the FSM update. */
void agnss_poll(int64_t now_ms, const struct loc_status *loc,
		const struct ephe_inventory *inv);

/* Is the assistance supply believed alive? True unless disabled (`agnss
 * off` on the shell) or fetch-locked-out (5 consecutive failures). Feeds
 * loc_fsm_set_agnss_supply(): the FSM refuses the radio-dark EXCLUSIVE
 * escalation while this is true and the inventory is cold. Deliberately
 * optimistic at boot — the lockout is the debounced proof of failure. */
bool agnss_supply_ok(void);

#else

static inline void agnss_set_device_id(const char *id) { (void)id; }
static inline void agnss_poll(int64_t now_ms, const struct loc_status *loc,
			      const struct ephe_inventory *inv)
{
	(void)now_ms;
	(void)loc;
	(void)inv;
}
static inline bool agnss_supply_ok(void) { return false; }

#endif /* CONFIG_TRACKER_AGNSS */

#endif /* AGNSS_H__ */
