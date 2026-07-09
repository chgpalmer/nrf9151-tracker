/*
 * Stub implementations of nrf_modem_lib and nrf_modem_gnss/at APIs for
 * native_sim builds (CONFIG_NRF_MODEM_LIB=n). Provides all symbols that
 * main.c links against, with no real modem hardware.
 *
 * GNSS: a k_timer fires at 1 Hz delivering a fake PVT fix (London).
 * AT:   AT%XMONITOR returns a canned response string.
 */

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>
#include <hw_id.h>
#include <nsi_host_trampolines.h>

static nrf_modem_gnss_event_handler_type_t gnss_evt_handler;

static struct nrf_modem_gnss_pvt_data_frame fake_pvt = {
	.latitude   = 51.507400,
	.longitude  = -0.127800,
	.altitude   = 15.0f,
	.accuracy   = 4.0f,
	.speed      = 1.3f,
	.heading    = 55.0f,   /* NE, matches the lat/lon drift below */
	.flags      = NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID |
		      NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID,
	.datetime = {
		.year = 2026, .month = 7, .day = 8, .hour = 12,
	},
	.sv = {
		[0] = { .sv = 10, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
		[1] = { .sv = 15, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
		[2] = { .sv = 20, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
		[3] = { .sv = 25, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
		[4] = { .sv = 30, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
		[5] = { .sv = 35, .flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX },
	},
};

static void gnss_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);

	/* Straight NE walk: +1e-5° lat (~1.1m) + 1e-5° lon (~0.7m) per second,
	 * so the track drifts at roughly walking pace and is visibly moving. */
	fake_pvt.latitude  += 0.00001;
	fake_pvt.longitude += 0.00001;

	/* Advance the fake clock so successive fixes have increasing time. */
	uint8_t s = fake_pvt.datetime.seconds + 1;
	fake_pvt.datetime.seconds = s % 60;
	if (s >= 60) {
		fake_pvt.datetime.minute = (fake_pvt.datetime.minute + 1) % 60;
	}

	if (gnss_evt_handler) {
		gnss_evt_handler(NRF_MODEM_GNSS_EVT_PVT);
	}
}

K_TIMER_DEFINE(gnss_tick, gnss_timer_fn, NULL);

/* ── hw_id (device identity) ── */

int hw_id_get(char *buf, size_t buf_len)
{
	if (buf == NULL || buf_len == 0) {
		return -EINVAL;
	}

	/* native_sim: read the host process env, not picolibc's empty env. */
	const char *id = nsi_host_getenv("TRACKER_DEVICE_ID");
	if (id == NULL || id[0] == '\0') {
		id = CONFIG_TRACKER_SIM_DEVICE_ID;
	}

	if (strlen(id) >= buf_len) {
		return -EINVAL;
	}
	strcpy(buf, id);
	return 0;
}

/* ── nrf_modem_lib ── */

int nrf_modem_lib_init(void)
{
	return 0;
}

int nrf_modem_lib_shutdown(void)
{
	return 0;
}

/* ── nrf_modem_gnss ── */

int32_t nrf_modem_gnss_event_handler_set(nrf_modem_gnss_event_handler_type_t handler)
{
	gnss_evt_handler = handler;
	return 0;
}

int32_t nrf_modem_gnss_nmea_mask_set(uint16_t nmea_mask)
{
	ARG_UNUSED(nmea_mask);
	return 0;
}

int32_t nrf_modem_gnss_fix_interval_set(uint16_t fix_interval)
{
	ARG_UNUSED(fix_interval);
	return 0;
}

int32_t nrf_modem_gnss_fix_retry_set(uint16_t fix_retry)
{
	ARG_UNUSED(fix_retry);
	return 0;
}

int32_t nrf_modem_gnss_use_case_set(uint8_t use_case)
{
	ARG_UNUSED(use_case);
	return 0;
}

/* The sim's fake PVT is always a valid fix, so pretend GNSS is warm: report
 * ephemeris for enough satellites, well short of expiry. Otherwise loc_fsm
 * would read zeroes and keep re-entering ACQUIRE, and the sim would never
 * publish. Expiry is in minutes; broadcast ephemeris lasts about two hours. */
int32_t nrf_modem_gnss_agnss_expiry_get(struct nrf_modem_gnss_agnss_expiry *agnss_expiry)
{
	if (agnss_expiry == NULL) {
		return -1;
	}
	memset(agnss_expiry, 0, sizeof(*agnss_expiry));

	agnss_expiry->data_flags = 0; /* GPS system time is known */
	agnss_expiry->sv_count = 6;
	for (int i = 0; i < agnss_expiry->sv_count; i++) {
		agnss_expiry->sv[i].sv_id = i + 1;
		agnss_expiry->sv[i].ephe_expiry = 120;
		agnss_expiry->sv[i].alm_expiry = 10080;
	}
	return 0;
}

int32_t nrf_modem_gnss_start(void)
{
	k_timer_start(&gnss_tick, K_SECONDS(1), K_SECONDS(1));
	return 0;
}

int32_t nrf_modem_gnss_stop(void)
{
	k_timer_stop(&gnss_tick);
	return 0;
}

/* When TRACKER_SIM_NO_GPS is set in the host env, report PVT events with no
 * valid fix so the firmware exercises its cell-location fallback. */
static bool no_gps(void)
{
	const char *v = nsi_host_getenv("TRACKER_SIM_NO_GPS");
	return v != NULL && v[0] != '\0' && v[0] != '0';
}

int32_t nrf_modem_gnss_read(void *buf, int32_t buf_len, int type)
{
	ARG_UNUSED(type);
	if (buf_len < (int32_t)sizeof(fake_pvt)) {
		return -1;
	}
	memcpy(buf, &fake_pvt, sizeof(fake_pvt));
	if (no_gps()) {
		((struct nrf_modem_gnss_pvt_data_frame *)buf)->flags &=
			~NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;
	}
	return 0;
}

/* ── nrf_modem_at ── */

int nrf_modem_at_cmd(void *buf, size_t len, const char *fmt, ...)
{
	ARG_UNUSED(fmt);
	/* Canned %XMONITOR: registered roaming, band 20, a real central-London
	 * cell so the local OpenCelliD lookup resolves it. PLMN 23410 (MCC 234
	 * MNC 10), TAC 0x10E0 (4320), cell-id 0x8233072 (136523890). */
	static const char xmonitor[] =
		"%XMONITOR: 5,\"EE\",\"EE\",\"23410\","
		"\"10E0\",9,20,\"08233072\",402,6400,45,50\r\nOK\r\n";

	if (len < sizeof(xmonitor)) {
		return -1;
	}
	memcpy(buf, xmonitor, sizeof(xmonitor));
	return 0;
}
