/*
 * LTE control: bring-up, event bookkeeping and func-mode actuation.
 *
 * Owns every lte_lc call and the state the LTE callback thread reports
 * (registration, PDN, RRC, PSM), exposed to the main loop as getters. The
 * loop stays the only *decision* point — this module never acts on its own;
 * it executes what it is told and remembers what the modem said.
 *
 * Also home to the attach-speed machinery: the %XSIM timing monitor, the
 * front-loaded search pattern, the CEER post-mortem after a registration
 * loss, and the once-per-boot network-state checkpoint (see lte_ctrl.c for
 * each one's written justification).
 */
#ifndef LTE_CTRL_H__
#define LTE_CTRL_H__

#include <stdbool.h>
#include <stdint.h>

/* Configure %XSIM notifications + the fast search pattern, then start the
 * modem asynchronously (lte_lc_connect_async + default-PDN events). Status
 * arrives via the getters below as the callback thread reports it. */
int lte_ctrl_start(void);

/* Registered (home or roaming). NOT the same as having an IP — see below. */
bool lte_ctrl_registered(void);

/* Default PDN context active: sockets and DNS actually work. Always true on
 * native_sim (no PDN module there). */
bool lte_ctrl_pdn_active(void);

/* Is the LTE stack meant to be on? Flips only via lte_ctrl_set_stack. */
bool lte_ctrl_stack_on(void);

/* ACTIVATE/DEACTIVATE_LTE for the GNSS_EXCLUSIVE edges. Tracks stack_on on
 * success; returns the lte_lc error. GNSS keeps running across both edges. */
int lte_ctrl_set_stack(bool on);

/* Once-per-boot NVM checkpoint (persist network state so the NEXT reset
 * warm-starts): due only while enabled, not yet done, and the caller says
 * the moment is quiet. lte_ctrl_checkpoint() runs the power-off -> normal
 * cycle; the caller must close the CoAP socket first and re-apply the GNSS
 * mode after (CFUN=0 takes everything down). */
bool lte_ctrl_checkpoint_due(void);
void lte_ctrl_checkpoint(void);

/* Deferred work that must not run in the callback thread: one AT+CEER read
 * per registration-loss edge. Call once per main-loop pass. */
void lte_ctrl_poll(void);

/* Modem post-mortem: snapshot what the modem CLAIMS its state is
 * (%CONEVAL: RRC state + link quality; +CEER; +CEREG?) into the log —
 * flash always, wire when sends recover. Called by main on the first
 * send timeout of an episode. May itself block if the modem IPC is dead:
 * that is diagnostic — the watchdog reboots and the tape ends here. */
void lte_ctrl_post_mortem(void);

/* A publish left the device: stamp the RRC-tail measurement (the handler
 * logs how long RRC lingers after the last send — RAI should shrink it). */
void lte_ctrl_note_publish(int64_t now_ms);

/* Status-block inputs. */
const char *lte_ctrl_status_str(void);
bool lte_ctrl_rrc_connected(void);
/* PSM grant: *active_s < 0 means PSM not granted. */
void lte_ctrl_psm(int *tau_s, int *active_s);

#endif /* LTE_CTRL_H__ */
