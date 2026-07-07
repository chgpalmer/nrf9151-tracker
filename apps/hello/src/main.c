/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("Hello from nRF9151-DK! %s\n", CONFIG_BOARD_TARGET);
	while (1) {
		k_sleep(K_SECONDS(5));
		printk("still alive\n");
	}
	return 0;
}
