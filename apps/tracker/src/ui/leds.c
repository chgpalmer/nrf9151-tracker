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

static void set(int led, bool on)
{
	gpio_pin_set_dt(&leds[led], on);
}

/* ── GPS acquisition progress: counted blips ─────────────────────────
 * While acquiring, LED2 blips N times per 2 s cycle, N = satellites both
 * tracked AND carrying an ephemeris (loc_status.ephemeris_visible, capped
 * at the 4 a fix needs) — a glanceable "how close is the fix" meter.
 * Zero usable satellites renders as one long slow pulse instead
 * ("searching, nothing usable yet"). Sub-second timing needs its own
 * clock: a 100 ms k_timer renders the pattern, and it runs ONLY while
 * acquiring — solid/off are set directly and the timer is stopped. */
static struct k_timer gps_timer;
static atomic_t gps_blips; /* 0 = long pulse; 1..4 = counted blips */
static bool gps_pattern_on;

/* LED4 = accelerometer activity. Rendered by a one-shot k_timer, NOT by
 * leds_update: the main loop blocks for seconds on network I/O (the A-GNSS
 * CON exchange), which would otherwise strand the pulse lit. The IMU trigger
 * lights it and (re)arms the timer; the timer clears it independently. */
static struct k_timer imu_timer;
static void imu_pulse_off(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	set(LED_HELP, false);
}

static void gps_pattern_tick(struct k_timer *timer)
{
	static uint8_t slot;               /* 20 slots x 100 ms = 2 s cycle */
	int n = atomic_get(&gps_blips);
	bool on;

	ARG_UNUSED(timer);
	slot = (slot + 1) % 20;
	if (n == 0) {
		on = slot < 6;             /* 600 ms pulse, 1.4 s dark */
	} else {
		/* Blip k occupies slot 3k: 100 ms on, 200 ms off, crisp. */
		on = slot < 3 * n && (slot % 3) == 0;
	}
	set(LED_GPS, on);
}

void leds_init(void)
{
	for (int i = 0; i < LED_COUNT; i++) {
		if (!gpio_is_ready_dt(&leds[i]) ||
		    gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0) {
			LOG_WRN("LED %d not available", i);
			return;
		}
	}
	k_timer_init(&gps_timer, gps_pattern_tick, NULL);
	k_timer_init(&imu_timer, imu_pulse_off, NULL);
	ready = true;
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

	/* GPS: solid on a current fix; dark when off; while acquiring, the
	 * pattern timer blips the usable-satellite count (see above). */
	bool acquiring = !loc->gps_current && loc->gnss_mode != LOC_GNSS_OFF;

	if (acquiring) {
		atomic_set(&gps_blips,
			   MIN(loc->ephemeris_visible, 4));
		if (!gps_pattern_on) {
			gps_pattern_on = true;
			k_timer_start(&gps_timer, K_MSEC(100), K_MSEC(100));
		}
	} else {
		if (gps_pattern_on) {
			gps_pattern_on = false;
			k_timer_stop(&gps_timer);
		}
		set(LED_GPS, loc->gps_current);
	}

	/* TX: pulse armed by leds_tx_pulse(), cleared here once it expires. */
	set(LED_TX, k_uptime_get() < tx_until_ms);

	/* LED4 (accelerometer activity) is NOT touched here — it is owned by
	 * imu_timer so a blocked loop can't strand it lit; see leds_imu_pulse. */
}

void leds_imu_pulse(void)
{
	if (!ready) {
		return;
	}
	/* Light it and (re-)arm the one-shot: continuous movement keeps
	 * re-triggering so it reads solid, and it clears ~200 ms after the
	 * last interrupt regardless of what the main loop is doing. */
	set(LED_HELP, true);
	k_timer_start(&imu_timer, K_MSEC(200), K_NO_WAIT);
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

void leds_imu_pulse(void)
{
}

#endif
