#include "imu.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, CONFIG_TRACKER_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(st_lis2dh)

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>
#include "leds.h"

/* Wake threshold on HP-FILTERED acceleration (gravity already removed):
 * dynamic movement in any direction, at any orientation. ~0.8 m/s² fires
 * on a desk nudge (imuprobe bench, 2026-07-14); a false wake costs one
 * GNSS look and a 60 s re-park, a missed movement costs a journey — so
 * bias sensitive. Runtime knob: `imu th <thousandths of m/s²>`. */
#define IMU_SLOPE_TH_UM  800000  /* sensor_value.val2 units (µm/s²) */
#define IMU_SLOPE_DUR    1       /* consecutive samples over threshold */
#define IMU_ODR_HZ       10      /* low-power mode sampling rate */

static const struct device *imu_dev;
static bool present;
static atomic_t kick;
static atomic_t event_count;

static void motion_trigger(const struct device *dev,
			   const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);
	atomic_set(&kick, 1);
	atomic_inc(&event_count);
	leds_imu_pulse(); /* instant bench feedback, straight from the trigger */
}

static int set_threshold(int32_t val1, int32_t val2)
{
	struct sensor_value th = { .val1 = val1, .val2 = val2 };

	return sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
			       SENSOR_ATTR_SLOPE_TH, &th);
}

bool imu_init(void)
{
	imu_dev = DEVICE_DT_GET_ONE(st_lis2dh);

	if (!device_is_ready(imu_dev)) {
		return false;
	}

	/* Driver default ODR is "runtime" = sensor left at power-on 0 Hz;
	 * set it before anything can sample or trigger. */
	struct sensor_value odr = { .val1 = IMU_ODR_HZ };
	struct sensor_value dur = { .val1 = IMU_SLOPE_DUR };
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DELTA,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	int err;

	err = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (!err) {
		err = set_threshold(0, IMU_SLOPE_TH_UM);
	}
	if (!err) {
		err = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
				      SENSOR_ATTR_SLOPE_DUR, &dur);
	}
	if (!err) {
		err = sensor_trigger_set(imu_dev, &trig, motion_trigger);
	}

	/* AN3308 wake-up recipe: route the high-pass filter into the INT1
	 * engine so gravity never reaches the comparator — without it,
	 * OR-mode fires forever at rest (bench-measured). The Zephyr driver
	 * has no knob for this filter; poke CTRL_REG2.HP_IA1 and zero the
	 * filter by reading REFERENCE. */
	if (!err) {
		const struct i2c_dt_spec bus =
			I2C_DT_SPEC_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(st_lis2dh));
		uint8_t ref;

		err = i2c_reg_write_byte_dt(&bus, 0x21 /* CTRL_REG2 */,
					    0x01 /* HP_IA1 */);
		if (!err) {
			err = i2c_reg_read_byte_dt(&bus, 0x26 /* REFERENCE */,
						   &ref);
		}
	}

	if (err) {
		LOG_ERR("IMU config failed: %d — falling back to GPS checks", err);
		return false;
	}
	present = true;
	return true;
}

bool imu_present(void)
{
	return present;
}

bool imu_take_motion(void)
{
	return present && atomic_clear(&kick) != 0;
}

static int cmd_imu(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "th") == 0) {
		int32_t um = atoi(argv[2]) * 1000; /* arg in mm/s² */
		int err = set_threshold(um / 1000000, um % 1000000);

		shell_print(sh, "threshold -> %d mm/s² (%d)", atoi(argv[2]), err);
		return 0;
	}
	shell_print(sh, "imu: %s, %ld events  (imu th <mm/s²> to retune)",
		    present ? "present" : "ABSENT",
		    (long)atomic_get(&event_count));
	return 0;
}
SHELL_CMD_REGISTER(imu, NULL, "IMU status / threshold", cmd_imu);

#else /* no st,lis2dh node (native_sim, no-IMU hardware) */

/* Sim hooks so the parked/wake path is exercisable end-to-end without
 * hardware: TRACKER_SIM_IMU=1 makes the IMU "present"; TRACKER_SIM_IMU_KICK_S
 * injects a motion event every N seconds. Same pattern as the modem mock's
 * TRACKER_SIM_* knobs. */
#ifdef CONFIG_BOARD_NATIVE_SIM
#include <stdlib.h>
#include "nsi_host_trampolines.h"

static bool present;
static int kick_s;
static int64_t last_kick_ms;

bool imu_init(void)
{
	const char *env = nsi_host_getenv("TRACKER_SIM_IMU");

	present = (env != NULL && env[0] == '1');
	env = nsi_host_getenv("TRACKER_SIM_IMU_KICK_S");
	kick_s = env ? atoi(env) : 0;
	last_kick_ms = k_uptime_get();
	return present;
}

bool imu_present(void)
{
	return present;
}

bool imu_take_motion(void)
{
	if (!present || kick_s <= 0) {
		return false;
	}
	int64_t now = k_uptime_get();

	if ((now - last_kick_ms) >= (int64_t)kick_s * 1000) {
		last_kick_ms = now;
		LOG_INF("sim: IMU kick");
		return true;
	}
	return false;
}

#else /* real hardware without the sensor in DT */

bool imu_init(void)      { return false; }
bool imu_present(void)   { return false; }
bool imu_take_motion(void) { return false; }

#endif /* CONFIG_BOARD_NATIVE_SIM */
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_lis2dh) */
