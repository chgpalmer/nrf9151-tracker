/*
 * atprobe: a bare AT console on the modem — nothing else runs.
 *
 * The tracker app owns the radio (FSM re-attach, GNSS duty cycle), which
 * starves or aborts long manual procedures: a 20-minute AT+COPS=? under
 * the live tracker never answered (2026-07-12). This app initializes the
 * modem library and then does nothing; the shell's `at` command is the
 * only radio user on the board.
 *
 * Usage over `make uart`:
 *   at AT+CFUN=1       radio on (the scan needs it)
 *   at AT+COPS=?       manual carrier scan, takes minutes;
 *                      AcT 7 = LTE-M, 9 = NB-IoT
 *   at AT%XMONITOR     serving-cell detail
 *   at AT+CFUN=0       radio off (persists modem state)
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>

LOG_MODULE_REGISTER(atprobe, LOG_LEVEL_INF);

int main(void)
{
	int err = nrf_modem_lib_init();

	if (err) {
		LOG_ERR("modem lib init: %d", err);
		return 0;
	}
	LOG_INF("atprobe ready — modem idle. Radio on: at AT+CFUN=1");
	return 0;
}
