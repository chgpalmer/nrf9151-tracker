#include "guard.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_TASK_WDT)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/task_wdt/task_wdt.h>
#endif

LOG_MODULE_REGISTER(guard, CONFIG_TRACKER_LOG_LEVEL);

/* Survives sys_reboot (SRAM retained through NVIC reset), dies with power —
 * exactly the lifetime a "why did I just reboot" note wants. */
#define NOTE_MAGIC 0x67756172 /* "guar" */
enum guard_death {
	GUARD_WDT = 1,
	GUARD_MODEM_FAULT = 2,
};
static struct guard_note {
	uint32_t magic;
	uint32_t kind;
	uint32_t pc;
} note __noinit;

static void die(enum guard_death kind, uint32_t pc)
{
	note.magic = NOTE_MAGIC;
	note.kind = kind;
	note.pc = pc;
	sys_reboot(SYS_REBOOT_COLD);
}

#if defined(CONFIG_TASK_WDT)
static int wdt_channel = -ENODEV;
static volatile bool hang_requested;

/* Runs from the task_wdt timer ISR: the loop missed its deadline. */
static void loop_stalled(int channel_id, void *user_data)
{
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);
	die(GUARD_WDT, 0);
}
#endif

#if defined(CONFIG_NRF_MODEM_LIB_ON_FAULT_APPLICATION_SPECIFIC)
#include <modem/nrf_modem_lib.h>
#include <nrf_modem.h>

/* The modem core crashed. Reboot the whole SoC rather than hot-resetting
 * the modem: deterministic (~40 s back to tracking via the normal boot
 * path, which the flight recorder and server narrate) instead of trying
 * to rebuild lte_lc/socket/GNSS state on a corpse mid-flight. */
void nrf_modem_fault_handler(struct nrf_modem_fault_info *info)
{
	die(GUARD_MODEM_FAULT, info ? (uint32_t)info->program_counter : 0);
}
#endif

void guard_init(void)
{
	if (note.magic == NOTE_MAGIC) {
		/* ERR: ships on the uplink's first flush AND lands in flash —
		 * a death certificate nobody has to go looking for. */
		LOG_ERR("previous session died: %s (pc 0x%08x)",
			note.kind == GUARD_WDT ? "main loop stalled (watchdog)" :
			note.kind == GUARD_MODEM_FAULT ? "MODEM FAULT" : "?",
			note.pc);
		note.magic = 0;
	}

#if defined(CONFIG_TASK_WDT)
	/* Hardware fallback if the SoC has a WDT instance: catches even a
	 * dead kernel timer. Without it, task_wdt still catches a stalled
	 * loop as long as timers tick (both field freezes kept timers alive —
	 * the LED blip pattern never stopped). */
	const struct device *hw =
		COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(wdt0), okay),
			    (DEVICE_DT_GET(DT_NODELABEL(wdt0))), (NULL));
	int err = task_wdt_init(hw);

	if (err) {
		LOG_ERR("task_wdt init: %d (running unguarded)", err);
		return;
	}
	wdt_channel = task_wdt_add(CONFIG_TRACKER_GUARD_WDT_S * 1000U,
				   loop_stalled, NULL);
	if (wdt_channel < 0) {
		LOG_ERR("task_wdt add: %d (running unguarded)", wdt_channel);
	} else {
		LOG_INF("watchdog armed: loop must feed every %d s",
			CONFIG_TRACKER_GUARD_WDT_S);
	}
#endif
}

void guard_feed(void)
{
#if defined(CONFIG_TASK_WDT)
	if (hang_requested) {
		/* Bench self-test (`guard hang`): stall the loop right here,
		 * in loop context, and let the bite prove the whole
		 * note-reboot-report path. WRN so the intent ships first. */
		LOG_WRN("guard: deliberate hang (test) — watchdog bites in %d s",
			CONFIG_TRACKER_GUARD_WDT_S);
		k_sleep(K_FOREVER);
	}
	if (wdt_channel >= 0) {
		task_wdt_feed(wdt_channel);
	}
#endif
}

#if defined(CONFIG_TASK_WDT) && defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	hang_requested = true;
	shell_print(sh, "main loop will stall on its next pass; reboot in ~%d s",
		    CONFIG_TRACKER_GUARD_WDT_S);
	return 0;
}
SHELL_CMD_REGISTER(hang, NULL, "stall the main loop (watchdog self-test)",
		   cmd_hang);
#endif
