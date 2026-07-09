/*
 * Stub implementation of lte_lc API for native_sim builds.
 * Immediately "registers" on a roaming network via a short k_timer delay,
 * triggering the same lte_handler path as the real modem.
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <modem/lte_lc.h>

static lte_lc_evt_handler_t stored_handler;

static void lte_connect_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	if (!stored_handler) {
		return;
	}

	struct lte_lc_evt evt = {
		.type          = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_REGISTERED_ROAMING,
	};
	stored_handler(&evt);
}

K_TIMER_DEFINE(lte_connect_timer, lte_connect_timer_fn, NULL);

int lte_lc_connect_async(lte_lc_evt_handler_t handler)
{
	stored_handler = handler;
	/* Fire after 100ms so main() gets past lte_lc_connect_async() first */
	k_timer_start(&lte_connect_timer, K_MSEC(100), K_NO_WAIT);
	return 0;
}

int lte_lc_offline(void)   { return 0; }
int lte_lc_power_off(void) { return 0; }
int lte_lc_normal(void)    { return 0; }
