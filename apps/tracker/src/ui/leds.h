/*
 * Glanceable state on the DK's four user LEDs, in FSM order: link, fix, data.
 *
 *   LED1  LTE   off = radio deliberately off (GNSS_EXCLUSIVE)
 *               blink = attaching        solid = registered
 *   LED2  GPS   off = GNSS not running   solid = fix current
 *               acquiring = a counted progress meter every 2 s:
 *                 one LONG slow pulse    searching, 0 usable satellites
 *                 1..4 crisp blips       that many tracked satellites with
 *                                        ephemeris (4 = fix imminent)
 *   LED3  TX    short pulse on every successful CoAP send
 *   LED4  HELP  off = normal
 *               blink = CELL_LOOP (gave up acquiring, retrying on sight)
 *               solid = GNSS_EXCLUSIVE (radio dark, deep hunt)
 *
 * The glance rules: LED2 solid means "safe to start moving" (fix held, hot
 * re-fix from here is seconds); its blip count while acquiring is "how
 * close" — 4 blips means the geometry is assembled. LED4 lit at all means
 * GPS is struggling and explains an unlit LED1. LED3 flickering
 * periodically means data is flowing.
 *
 * On boards without LED aliases (native_sim) everything compiles to no-ops.
 */
#ifndef LEDS_H__
#define LEDS_H__

#include <stdbool.h>
#include "loc_fsm.h"

void leds_init(void);

/* Reflect the current state; call once per main-loop pass. Blink phase
 * advances per call, so blinking follows the loop cadence (~1-2 s). */
void leds_update(const struct loc_status *loc, bool lte_up);

/* Pulse LED3 to confirm a successful send (cleared ~500 ms later by the
 * next update pass). */
void leds_tx_pulse(void);

#endif /* LEDS_H__ */
