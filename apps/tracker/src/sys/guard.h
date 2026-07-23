/*
 * guard: the "it must never sit comatose" module.
 *
 * Two field incidents (2026-07-17: 45 h silent; 2026-07-21: 3 h) froze the
 * main loop AND the modem callbacks mid-ride — at 12-14 h uptime, under
 * handover churn — with no watchdog to catch it and nothing visible but
 * timer-driven LEDs still blinking. This module makes that class of death
 * short and self-documenting:
 *
 *   - a task watchdog channel fed once per main-loop pass; a stalled loop
 *     reboots the SoC after TRACKER_GUARD_WDT_S (flash flight recorder and
 *     the server's boot-while-armed alert narrate the rest);
 *   - a modem-core fault (CONFIG_NRF_MODEM_LIB_ON_FAULT_APPLICATION_SPECIFIC)
 *     reboots the same way — deterministic recovery over hot modem surgery;
 *   - both stamp a note in __noinit RAM (survives soft reset, dies with
 *     power) that the next boot logs at ERR: every death gets a name.
 *
 * Everything is IS_ENABLED-gated: native_sim builds carry the API as no-ops.
 */
#ifndef GUARD_H__
#define GUARD_H__

/* Report how the previous session died (if the note survived) and start
 * the watchdog. Call once, early — right after the boot identity lines. */
void guard_init(void);

/* Feed the loop's watchdog channel. Call once per main-loop pass; the
 * `guard hang` shell command turns this call into a deliberate stall so
 * the bite-reboot-report path stays testable on the bench. */
void guard_feed(void);

/* The modem stopped answering sends (two consecutive SO_SNDTIMEO
 * expiries across flushes — main.c's send-health policy): reboot with a
 * named note, same as a watchdog bite. A modem that answers nothing is
 * a modem only a reset talks to. No-op-ish on native_sim (logs ERR). */
void guard_modem_dead(void);

#endif /* GUARD_H__ */
