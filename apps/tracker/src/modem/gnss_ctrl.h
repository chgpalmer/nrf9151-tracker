/*
 * GNSS control: receiver configuration, the PVT mailbox and mode actuation.
 *
 * Owns every nrf_modem_gnss call on the receiver-control side (the A-GNSS
 * injection path lives in loc/agnss.c) and the latest PVT frame, delivered
 * by the modem's event handler and consumed by the main loop. Like lte_ctrl
 * this module decides nothing: loc_fsm picks the mode, main tells us, we
 * make the receiver match.
 */
#ifndef GNSS_CTRL_H__
#define GNSS_CTRL_H__

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <nrf_modem_gnss.h>

#include "loc_fsm.h"

/* Set up GNSS without starting it: loc_fsm decides when it may run (an
 * unregistered modem's cell search owns the radio). */
int gnss_ctrl_configure(void);

/* Block until the next PVT event or the timeout — the main loop's tick.
 * Returns true when a fresh frame arrived. */
bool gnss_ctrl_wait_pvt(k_timeout_t timeout);

/* The latest PVT frame. With the receiver stopped this is a FROZEN copy of
 * the last frame — often FIX_VALID from a parking fix; gate on
 * gnss_ctrl_mode() != LOC_GNSS_OFF before treating it as an observation. */
const struct nrf_modem_gnss_pvt_data_frame *gnss_ctrl_pvt(void);

/* The GNSS mode actually applied to the modem (OFF after a failed start, so
 * callers retry rather than latching a wrong mode). */
enum loc_gnss_mode gnss_ctrl_mode(void);

/* Make the receiver match the FSM's verdict. Reconfiguring runs the required
 * stop/set/start cycle; a failed start leaves the mode OFF and the next call
 * retries. Call once per main-loop pass. */
void gnss_ctrl_apply(enum loc_gnss_mode mode, uint16_t interval_s);

/* The modem was reset under us (CFUN cycle): the receiver is stopped and the
 * mode must be re-applied. Records OFF without touching the modem. */
void gnss_ctrl_mark_off(void);

/* EVT_BLOCKED/UNBLOCKED: is LTE currently holding the radio? (ISR-latched;
 * the caller logs the edge.) */
bool gnss_ctrl_blocked(void);

#endif /* GNSS_CTRL_H__ */
