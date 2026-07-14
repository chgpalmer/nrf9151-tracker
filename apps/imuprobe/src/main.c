/*
 * imuprobe: LIS3DH bring-up console — nothing else runs.
 *
 * Proves the wiring and the interrupt path before the tracker grows an
 * IMU motion_source. Two ways to see it work over `make uart`:
 *
 *   sensor get lis3dh@18 accel_xyz     read a sample on demand
 *   (shake the board)                  any-motion interrupt logs a line
 *
 * The any-motion threshold is set LOW so a desk nudge fires it; the
 * tracker's eventual threshold is a tuning question for later.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imuprobe, LOG_LEVEL_INF);

static uint32_t motion_count;

/* LED1 pulses per any-motion interrupt: the wiring demo needs no UART. */
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static void led_off_fn(struct k_timer *t) { gpio_pin_set_dt(&led, 0); }
static K_TIMER_DEFINE(led_off, led_off_fn, NULL);

static void motion_cb(const struct device *dev,
		      const struct sensor_trigger *trig)
{
	struct sensor_value v[3];

	if (led.port) {
		gpio_pin_set_dt(&led, 1);
		k_timer_start(&led_off, K_MSEC(200), K_NO_WAIT);
	}
	if (sensor_sample_fetch(dev) == 0 &&
	    sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v) == 0) {
		LOG_INF("MOTION #%u  x %d.%02d  y %d.%02d  z %d.%02d m/s2",
			++motion_count,
			v[0].val1, abs(v[0].val2) / 10000,
			v[1].val1, abs(v[1].val2) / 10000,
			v[2].val1, abs(v[2].val2) / 10000);
	} else {
		LOG_INF("MOTION #%u", ++motion_count);
	}
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET_ONE(st_lis2dh);

	if (led.port) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	if (!device_is_ready(dev)) {
		LOG_ERR("LIS3DH not ready — check wiring/address "
			"(SDA P0.30, SCL P0.31, addr 0x18)");
		return 0;
	}

	/* Driver default ODR is "runtime" = sensor left at power-on 0 Hz;
	 * pick 10 Hz low-power before anything can sample or trigger. */
	struct sensor_value odr = { .val1 = 10 };
	int err = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
				  SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (err) {
		LOG_ERR("set ODR: %d", err);
	}

	/* Wake-up threshold on HP-FILTERED accel (gravity already removed):
	 * ~0.8 m/s2 of dynamic movement in any direction. */
	struct sensor_value th = { .val1 = 0, .val2 = 800000 };
	struct sensor_value dur = { .val1 = 1 };
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DELTA,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	err = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SLOPE_TH, &th);
	if (!err) {
		err = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
				      SENSOR_ATTR_SLOPE_DUR, &dur);
	}
	if (!err) {
		err = sensor_trigger_set(dev, &trig, motion_cb);
	}
	if (err) {
		LOG_ERR("any-motion setup: %d (reads still work)", err);
	}

	/* ST AN3308 wake-up recipe: high-pass the INT1 engine's input so
	 * gravity is subtracted and any-direction movement trips the small
	 * threshold above. The Zephyr driver never touches this filter, so
	 * poke CTRL_REG2.HP_IA1 directly and zero the filter by reading
	 * REFERENCE. Without this, OR-mode fires forever at rest (gravity
	 * beats any usable threshold) — measured before adding it. */
	const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(DT_NODELABEL(lis3dh));
	uint8_t ref;

	if (i2c_reg_write_byte_dt(&bus, 0x21 /* CTRL_REG2 */,
				  0x01 /* HP_IA1 */) ||
	    i2c_reg_read_byte_dt(&bus, 0x26 /* REFERENCE */, &ref)) {
		LOG_ERR("HP filter setup failed");
	}

	LOG_INF("imuprobe ready (%s) — shake me, or: sensor get %s accel_xyz",
		dev->name, dev->name);
	return 0;
}
