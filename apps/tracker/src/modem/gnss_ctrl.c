#include "gnss_ctrl.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss, CONFIG_TRACKER_LOG_LEVEL);

#define GNSS_INTERVAL_S   1   /* PVT event rate in continuous mode */
/* In periodic mode, how long each check may hunt before giving up and
 * sleeping. Bounds the awake time (the whole point of periodic mode); a hot
 * start needs 1-3 s, so 30 s also covers a warm start without burning
 * minutes indoors where no fix is coming. */
#define GNSS_CHECK_RETRY_S 30

/* Reading ephemeris state is a modem call; it does not change at 1 Hz. */
#define EPHE_POLL_MS 5000

/* healthy's validity floor. The Kconfig symbol depends on TRACKER_AGNSS;
 * with assistance compiled out, healthy degenerates to held (floor 0) —
 * nothing consults the difference then (the FSM's supply verdict is
 * constantly false and agnss is stubbed). */
#ifndef CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN
#define CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN 0
#endif

static struct nrf_modem_gnss_pvt_data_frame pvt;
static K_SEM_DEFINE(pvt_sem, 0, 1);

/* Set by EVT_BLOCKED/UNBLOCKED; the handler runs in ISR context, so it only
 * flags the change and the caller logs the edge. */
static volatile bool gnss_blocked;

/* The GNSS mode actually applied to the modem. Reconfiguring requires a
 * stop/set/start cycle; a failed start leaves this at OFF so the next
 * apply retries rather than latching a wrong mode. */
static enum loc_gnss_mode mode_applied = LOC_GNSS_OFF;
static uint16_t interval_applied;

static void gnss_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			k_sem_give(&pvt_sem);
		}
		break;
	case NRF_MODEM_GNSS_EVT_BLOCKED:
		gnss_blocked = true;
		break;
	case NRF_MODEM_GNSS_EVT_UNBLOCKED:
		gnss_blocked = false;
		break;
	default:
		break;
	}
}

int gnss_ctrl_configure(void)
{
	int err;

	err = nrf_modem_gnss_event_handler_set(gnss_handler);
	if (err) {
		LOG_ERR("gnss event handler: %d", err);
		return err;
	}

	uint16_t nmea_mask = 0; /* no NMEA — we use PVT only */
	nrf_modem_gnss_nmea_mask_set(nmea_mask);

	nrf_modem_gnss_fix_interval_set(GNSS_INTERVAL_S);
	nrf_modem_gnss_fix_retry_set(0); /* run continuously */

	uint8_t use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;
	nrf_modem_gnss_use_case_set(use_case);

	return 0;
}

bool gnss_ctrl_wait_pvt(k_timeout_t timeout)
{
	return k_sem_take(&pvt_sem, timeout) == 0;
}

const struct nrf_modem_gnss_pvt_data_frame *gnss_ctrl_pvt(void)
{
	return &pvt;
}

enum loc_gnss_mode gnss_ctrl_mode(void)
{
	return mode_applied;
}

void gnss_ctrl_mark_off(void)
{
	mode_applied = LOC_GNSS_OFF;
}

bool gnss_ctrl_blocked(void)
{
	return gnss_blocked;
}

static struct ephe_inventory inv; /* .valid = false until the first read */
static int64_t last_ephe_poll_ms = -EPHE_POLL_MS; /* first call reads */

void gnss_ctrl_poll_inventory(int64_t now_ms)
{
	/* Raw frame is ~800 B: static, not stack. */
	static struct nrf_modem_gnss_agnss_expiry raw;

	if ((now_ms - last_ephe_poll_ms) < EPHE_POLL_MS) {
		return;
	}
	last_ephe_poll_ms = now_ms;

	/* On failure keep the previous digest: a failed read is not evidence
	 * that the ephemerides were lost. */
	if (nrf_modem_gnss_agnss_expiry_get(&raw) != 0) {
		return;
	}

	struct ephe_inventory fresh = {
		.valid = true,
		.data_flags = raw.data_flags,
	};

	for (int i = 0; i < raw.sv_count; i++) {
		uint16_t id = raw.sv[i].sv_id;
		uint16_t left = raw.sv[i].ephe_expiry;

		if (id < 1 || id > 32 || left == 0) {
			continue; /* GPS only; see the struct comment */
		}
		fresh.held_mask |= 1ULL << (id - 1);
		fresh.held++;
		if (fresh.min_expiry_min == 0 || left < fresh.min_expiry_min) {
			fresh.min_expiry_min = left;
		}
		if (left >= CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN) {
			fresh.healthy_mask |= 1ULL << (id - 1);
			fresh.healthy++;
		}
	}
	inv = fresh;
}

const struct ephe_inventory *gnss_ctrl_inventory(void)
{
	return &inv;
}

void gnss_ctrl_apply(enum loc_gnss_mode mode, uint16_t interval_s)
{
	if (mode == mode_applied &&
	    !(mode == LOC_GNSS_PERIODIC && interval_s != interval_applied)) {
		return;
	}

	if (mode_applied != LOC_GNSS_OFF) {
		(void)nrf_modem_gnss_stop();
	}
	mode_applied = LOC_GNSS_OFF;
	if (mode == LOC_GNSS_OFF) {
		/* Off while unregistered (search owns the radio) or
		 * IMU-parked (nothing to check). */
		LOG_INF("GNSS off");
		return;
	}

	bool cont = (mode == LOC_GNSS_CONTINUOUS);
	uint16_t ivl = cont ? GNSS_INTERVAL_S : interval_s;

	nrf_modem_gnss_fix_interval_set(ivl);
	nrf_modem_gnss_fix_retry_set(cont ? 0 : GNSS_CHECK_RETRY_S);

	int err = nrf_modem_gnss_start();

	if (err) {
		LOG_ERR("gnss start: %d", err); /* stays OFF, retried next pass */
		return;
	}
	LOG_INF("GNSS %s (interval %u s)",
		cont ? "continuous" : "periodic", ivl);
	mode_applied = mode;
	interval_applied = interval_s;
}
