#include "leds.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(leds, CONFIG_TRACKER_LOG_LEVEL);

/* All four or nothing: native_sim has a lone emulated led0, which would light
 * the wrong subset of the scheme below. */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay) &&                                \
	DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay) &&                            \
	DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay) &&                            \
	DT_NODE_HAS_STATUS(DT_ALIAS(led3), okay) && defined(CONFIG_GPIO)

#include <zephyr/drivers/gpio.h>

enum { LED_LTE, LED_GPS, LED_TX, LED_HELP, LED_COUNT };

static const struct gpio_dt_spec leds[LED_COUNT] = {
	[LED_LTE]  = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	[LED_GPS]  = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	[LED_TX]   = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	[LED_HELP] = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

static bool ready;
static bool blink_phase;
static int64_t tx_until_ms;

void leds_init(void)
{
	for (int i = 0; i < LED_COUNT; i++) {
		if (!gpio_is_ready_dt(&leds[i]) ||
		    gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0) {
			LOG_WRN("LED %d not available", i);
			return;
		}
	}
	ready = true;
}

static void set(int led, bool on)
{
	gpio_pin_set_dt(&leds[led], on);
}

void leds_update(const struct loc_status *loc, bool lte_up)
{
	if (!ready) {
		return;
	}

	blink_phase = !blink_phase;

	/* LTE: solid registered; blink while wanted-but-attaching; dark when the
	 * FSM turned the radio off on purpose (GNSS_EXCLUSIVE). */
	set(LED_LTE, lte_up || (loc->lte_wanted && blink_phase));

	/* GPS: solid on a current fix; blink while acquiring; dark when off. */
	set(LED_GPS, loc->gps_current ||
		     (loc->gnss_mode != LOC_GNSS_OFF && blink_phase));

	/* TX: pulse armed by leds_tx_pulse(), cleared here once it expires. */
	set(LED_TX, k_uptime_get() < tx_until_ms);

	/* HELP: GPS is struggling — solid in the radio-dark deep hunt, blink in
	 * the cell fallback loop. */
	set(LED_HELP, loc->state == LOC_GNSS_EXCLUSIVE ||
		      (loc->state == LOC_CELL_LOOP && blink_phase));
}

void leds_tx_pulse(void)
{
	if (!ready) {
		return;
	}
	tx_until_ms = k_uptime_get() + 500;
	set(LED_TX, true);
}

#else /* no LED aliases (native_sim): compile to no-ops */

void leds_init(void)
{
	LOG_DBG("no LEDs on this board");
}

void leds_update(const struct loc_status *loc, bool lte_up)
{
	ARG_UNUSED(loc);
	ARG_UNUSED(lte_up);
}

void leds_tx_pulse(void)
{
}

#endif
