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
#include <zephyr/kernel.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>

static nrf_modem_gnss_event_handler_type_t gnss_evt_handler;

static struct nrf_modem_gnss_pvt_data_frame fake_pvt = {
	.latitude   = 51.507400,
	.longitude  = -0.127800,
	.altitude   = 42.0f,
	.accuracy   = 3.5f,
	.flags      = NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID,
	.datetime = {
		.year    = 2026,
		.month   = 7,
		.day     = 8,
		.hour    = 12,
		.minute  = 0,
		.seconds = 0,
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
	if (gnss_evt_handler) {
		gnss_evt_handler(NRF_MODEM_GNSS_EVT_PVT);
	}
}

K_TIMER_DEFINE(gnss_tick, gnss_timer_fn, NULL);

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

int32_t nrf_modem_gnss_read(void *buf, int32_t buf_len, int type)
{
	ARG_UNUSED(type);
	if (buf_len < (int32_t)sizeof(fake_pvt)) {
		return -1;
	}
	memcpy(buf, &fake_pvt, sizeof(fake_pvt));
	return 0;
}

/* ── nrf_modem_at ── */

int nrf_modem_at_cmd(void *buf, size_t len, const char *fmt, ...)
{
	ARG_UNUSED(fmt);
	/* Canned %XMONITOR response: registered roaming, band 20, fake cell */
	static const char xmonitor[] =
		"%XMONITOR: 5,\"Vodafone UK\",\"Voda UK\",\"23415\","
		"\"1234\",9,20,\"DEADBEEF\",100,6400,45,50\r\nOK\r\n";

	if (len < sizeof(xmonitor)) {
		return -1;
	}
	memcpy(buf, xmonitor, sizeof(xmonitor));
	return 0;
}
