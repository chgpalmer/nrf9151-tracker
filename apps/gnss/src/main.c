/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <stdio.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>

static struct nrf_modem_gnss_pvt_data_frame pvt;
static K_SEM_DEFINE(pvt_sem, 0, 1);

static void gnss_event_handler(int event)
{
	if (event == NRF_MODEM_GNSS_EVT_PVT) {
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			k_sem_give(&pvt_sem);
		}
	}
}

int main(void)
{
	int err;

	printk("\nnRF9151-DK GNSS demo starting\n");

	err = nrf_modem_lib_init();
	if (err) {
		printk("nrf_modem_lib_init failed: %d\n", err);
		return -1;
	}

	if (nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0") != 0) {
		printk("Failed to set system mode\n");
	}
	if (nrf_modem_at_printf("AT+CFUN=31") != 0) {
		printk("Failed to activate GNSS (CFUN=31)\n");
	}

	nrf_modem_gnss_event_handler_set(gnss_event_handler);
	nrf_modem_gnss_fix_interval_set(1);
	nrf_modem_gnss_fix_retry_set(0);

	err = nrf_modem_gnss_start();
	if (err) {
		printk("nrf_modem_gnss_start failed: %d\n", err);
		return -1;
	}

	printk("GNSS started. Needs clear sky view; cold fix can take minutes.\n");

	while (1) {
		k_sem_take(&pvt_sem, K_FOREVER);

		if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			printk("\n*** GNSS FIX ACQUIRED ***\n");
			printk("Latitude : %.06f\n", pvt.latitude);
			printk("Longitude: %.06f\n", pvt.longitude);
			printk("Altitude : %.01f m\n", (double)pvt.altitude);
			printk("Accuracy : %.01f m\n", (double)pvt.accuracy);
			printk("UTC time : %04u-%02u-%02u %02u:%02u:%02u\n",
			       pvt.datetime.year, pvt.datetime.month,
			       pvt.datetime.day, pvt.datetime.hour,
			       pvt.datetime.minute, pvt.datetime.seconds);
			printk("Google Maps: https://maps.google.com/?q=%.06f,%.06f\n",
			       pvt.latitude, pvt.longitude);
		} else {
			int tracked = 0, used = 0;

			for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
				if (pvt.sv[i].sv == 0) {
					continue;
				}
				tracked++;
				if (pvt.sv[i].flags &
				    NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
					used++;
				}
			}
			printk("Searching... satellites tracked: %d, used: %d\n",
			       tracked, used);
		}
	}
}
