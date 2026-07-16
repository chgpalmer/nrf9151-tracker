#include "lte_ctrl.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <nrf_modem_at.h>

LOG_MODULE_REGISTER(lte, CONFIG_TRACKER_LOG_LEVEL);

static enum lte_lc_nw_reg_status lte_status = LTE_LC_NW_REG_NOT_REGISTERED;
/* Set from the LTE callback thread, polled by the main loop. */
static volatile bool lte_connected;

/* Registration (lte_connected) is NOT the same as having an active default
 * PDN with an assigned IP; sockets and DNS only work once the PDN is up, so
 * socket setup gates on this. On native_sim CONFIG_LTE_LC_PDN_MODULE is off
 * (host sockets are always up), so default to true there. */
#if defined(CONFIG_LTE_LC_PDN_MODULE)
static volatile bool pdn_active;
#else
static volatile bool pdn_active = true;
#endif

/* GNSS only runs when LTE leaves the radio alone, so the two facts that explain
 * a starved GNSS are whether we are stuck in RRC connected and whether the
 * network actually granted PSM. Both are set from the LTE callback thread. */
static volatile bool rrc_connected;
static volatile int  psm_tau_s    = -1;
static volatile int  psm_active_s = -1; /* -1 = PSM not granted */

/* When the last publish went out. The LTE handler uses it to log how long RRC
 * stayed connected after we finished sending -- the tail RAI should shrink. */
static volatile int64_t last_pub_ms;

/* Registration was lost: lte_ctrl_poll asks the modem WHY (AT+CEER, the last
 * EMM/ESM failure cause) — one line per loss, the difference between "it
 * dropped" and "the network rejected TAU with cause 15". Set in the LTE
 * callback, consumed in the loop (AT commands don't belong in callbacks). */
static volatile bool want_ceer;

/* lte_ctrl_start() turns the stack on; GNSS_EXCLUSIVE is the only thing that
 * turns it off (via lte_ctrl_set_stack). */
static bool stack_on = true;

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS: {
		/* The searching EDGE splits the attach time: boot->searching is
		 * modem init, searching->registered is scan + PLMN walk +
		 * registration — the part band memory can shrink. One INF per
		 * episode. */
		static enum lte_lc_nw_reg_status prev_status =
			LTE_LC_NW_REG_NOT_REGISTERED;

		if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING &&
		    prev_status != LTE_LC_NW_REG_SEARCHING) {
			LOG_INF("LTE: searching");
		}
		prev_status = evt->nw_reg_status;
		lte_status = evt->nw_reg_status;
		if (lte_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    lte_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_INF("LTE registered (%s)",
				lte_status == LTE_LC_NW_REG_REGISTERED_ROAMING
					? "roaming" : "home");
			lte_connected = true;
		} else {
			if (lte_connected) {
				want_ceer = true; /* registered -> lost: ask why */
			}
			lte_connected = false;
		}
		break;
	}
	case LTE_LC_EVT_PSM_UPDATE:
		/* active_time == -1 means the network refused PSM. Roaming networks
		 * often do. Without PSM the modem keeps monitoring pagings and GNSS
		 * never gets a long enough window for a cold start. Log at INF: this
		 * was LOG_DBG and therefore invisible at CONFIG_TRACKER_LOG_LEVEL=3. */
		psm_tau_s    = evt->psm_cfg.tau;
		psm_active_s = evt->psm_cfg.active_time;
		if (psm_active_s < 0) {
			LOG_WRN("PSM: NOT granted by network (TAU %d)", psm_tau_s);
		} else {
			LOG_INF("PSM: granted — TAU %d s, active %d s",
				psm_tau_s, psm_active_s);
		}
		break;
#if defined(CONFIG_LTE_LC_PDN_MODULE)
	case LTE_LC_EVT_PDN:
		/* Default-context (CID 0) activation is when we actually have
		 * an IP and sockets can be created. */
		if (evt->pdn.cid == 0) {
			if (evt->pdn.type == LTE_LC_EVT_PDN_ACTIVATED) {
				pdn_active = true;
				LOG_INF("PDN: default context active (IP up)");
			} else if (evt->pdn.type == LTE_LC_EVT_PDN_DEACTIVATED) {
				pdn_active = false;
				LOG_WRN("PDN: default context down");
			}
		}
		break;
#endif
	case LTE_LC_EVT_MODEM_EVENT: {
		/* The modem narrating its own radio work — the observability
		 * for "what is searching actually doing". Search-phase events
		 * are sparse (one per search cycle); CE level shifts with
		 * coverage and stays at DBG. */
		switch (evt->modem_evt.type) {
		case LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE:
			LOG_INF("modem: light search done (no cell found yet)");
			break;
		case LTE_LC_MODEM_EVT_SEARCH_DONE:
			LOG_DBG("modem: full search done");
			break;
		case LTE_LC_MODEM_EVT_RESET_LOOP:
			LOG_WRN("modem: RESET LOOP restriction");
			break;
		case LTE_LC_MODEM_EVT_CE_LEVEL:
			LOG_DBG("modem: CE level %d", evt->modem_evt.ce_level);
			break;
		default:
			LOG_INF("modem: event %d", evt->modem_evt.type);
			break;
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		rrc_connected = (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED);
		if (rrc_connected) {
			LOG_DBG("RRC: connected (GNSS blocked)");
		} else {
			/* The tail between our last publish and RRC release is
			 * radio time GNSS cannot use. RAI exists to shrink it;
			 * this delta is the measurement of whether it works. */
			int64_t tail = k_uptime_get() - last_pub_ms;

			if (last_pub_ms > 0 && tail < 30000) {
				LOG_DBG("RRC: idle (%lld ms after publish)",
					(long long)tail);
			} else {
				LOG_DBG("RRC: idle");
			}
		}
		break;
	default:
		break;
	}
}

const char *lte_ctrl_status_str(void)
{
	switch (lte_status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:       return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:      return "registered (home)";
	case LTE_LC_NW_REG_SEARCHING:            return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:  return "denied";
	case LTE_LC_NW_REG_UNKNOWN:              return "no coverage";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:   return "registered (roaming)";
	default:                                  return "?";
	}
}

#if defined(CONFIG_NRF_MODEM_LIB)
#include <modem/at_monitor.h>

/* SIM readiness timing: "%XSIM: 1" marks the UICC initialized. Whether it
 * lands at +5 s or +60 s decides if the boot-attach gap is the SIM applet
 * (nothing we can do) or the modem's search scheduling (tunable below). */
static void xsim_handler(const char *notif)
{
	char buf[32];

	strncpy(buf, notif, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0';
	LOG_INF("SIM: %s", buf);
}
AT_MONITOR(xsim_monitor, "%XSIM", xsim_handler);

/* Front-load search attempts when unregistered: the default scheduling let
 * the modem idle ~60 s between a failed +9 s boot light search and the full
 * search that then succeeded in 6 s (measured 2026-07-12). Retry at 5 s,
 * then 10/20/40, then loop every 60 s — worst case a handful of extra scans
 * per outage, each seconds long. Written to modem NVM; `fastsearch off`
 * clears back to modem defaults. */
static void fast_search_configure(void)
{
	struct lte_lc_periodic_search_cfg cfg = {
		.loop = true,
		.return_to_pattern = 0,
		.band_optimization = 1,
		.pattern_count = 1,
		.patterns = { {
			.type = LTE_LC_PERIODIC_SEARCH_PATTERN_TABLE,
			.table = { .val_1 = 5, .val_2 = 10, .val_3 = 20,
				   .val_4 = 40, .val_5 = 60 },
		} },
	};
	int err = lte_lc_periodic_search_set(&cfg);

	if (err) {
		LOG_WRN("LTE: fast search pattern rejected (%d)", err);
	} else {
		LOG_INF("LTE: fast search pattern set");
	}
}

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_fastsearch(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "off") == 0) {
		shell_print(sh, "clear: %d (reboot re-applies unless rebuilt)",
			    lte_lc_periodic_search_clear());
	} else if (argc == 2 && strcmp(argv[1], "on") == 0) {
		fast_search_configure();
		shell_print(sh, "set");
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(fastsearch, NULL,
		       "Aggressive out-of-coverage search pattern (on|off)",
		       cmd_fastsearch, 1, 1);
#endif
#endif /* CONFIG_NRF_MODEM_LIB */

/* ── attach-speed experiment: network-state checkpoint ────────────────────
 * The modem persists its network history (band, channel, PLMN — and GNSS
 * data) to its own NVM only on CFUN=0, which an abrupt dev reset never
 * performs — so every cold boot pays a full scan (measured ~90 s vs 32-39 s
 * warm). Once per boot, at the first parked entry (radio idle, nobody
 * waiting), cycle power-off -> normal: one extra re-attach at the quietest
 * possible moment buys the NEXT reset a warm start. `checkpoint off` on the
 * shell disables it at runtime. */
static bool checkpoint_enabled = IS_ENABLED(CONFIG_TRACKER_LTE_CHECKPOINT);
static bool checkpoint_done;

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_checkpoint(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		checkpoint_enabled = (strcmp(argv[1], "on") == 0);
	}
	shell_print(sh, "checkpoint %s%s", checkpoint_enabled ? "on" : "off",
		    checkpoint_done ? " (already ran this boot)" : "");
	return 0;
}
SHELL_CMD_ARG_REGISTER(checkpoint, NULL,
		       "LTE network-state checkpoint (on|off)",
		       cmd_checkpoint, 1, 1);
#endif

bool lte_ctrl_checkpoint_due(void)
{
	return checkpoint_enabled && !checkpoint_done;
}

void lte_ctrl_checkpoint(void)
{
	checkpoint_done = true;
	LOG_INF("LTE: checkpoint — persisting network state (one re-attach)");
	(void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
	(void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);
}

int lte_ctrl_start(void)
{
#if defined(CONFIG_NRF_MODEM_LIB)
	/* Time the SIM (%XSIM notifications) and front-load search retries —
	 * both persist/apply before the attach we're about to start. */
	(void)nrf_modem_at_printf("AT%%XSIM=1");
	fast_search_configure();
#endif

	int err = lte_lc_connect_async(lte_handler);

	if (err) {
		LOG_ERR("lte_lc_connect_async: %d", err);
		return err;
	}

#if defined(CONFIG_LTE_LC_PDN_MODULE)
	/* Default-context PDN events tell us when the IP is actually up,
	 * not just when we've registered. */
	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_WRN("pdn events enable: %d", err);
	}
#endif
	return 0;
}

bool lte_ctrl_registered(void)
{
	return lte_connected;
}

bool lte_ctrl_pdn_active(void)
{
	return pdn_active;
}

bool lte_ctrl_stack_on(void)
{
	return stack_on;
}

int lte_ctrl_set_stack(bool on)
{
	int err = lte_lc_func_mode_set(on ? LTE_LC_FUNC_MODE_ACTIVATE_LTE
					  : LTE_LC_FUNC_MODE_DEACTIVATE_LTE);

	if (err == 0) {
		stack_on = on;
	}
	return err;
}

void lte_ctrl_poll(void)
{
	/* Registration loss post-mortem: one CEER read per loss edge. */
	if (want_ceer) {
		want_ceer = false;
		char ceer[64];

		if (nrf_modem_at_cmd(ceer, sizeof(ceer), "AT+CEER") == 0) {
			char *nl = strpbrk(ceer, "\r\n");

			if (nl) {
				*nl = '\0';
			}
			LOG_INF("LTE lost: %s", ceer);
		}
	}
}

void lte_ctrl_note_publish(int64_t now_ms)
{
	last_pub_ms = now_ms;
}

bool lte_ctrl_rrc_connected(void)
{
	return rrc_connected;
}

void lte_ctrl_psm(int *tau_s, int *active_s)
{
	*tau_s = psm_tau_s;
	*active_s = psm_active_s;
}
