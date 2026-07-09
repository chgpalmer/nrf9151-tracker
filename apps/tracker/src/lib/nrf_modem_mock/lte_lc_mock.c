/*
 * Stub implementation of lte_lc API for native_sim builds.
 * Immediately "registers" on a roaming network via a short k_timer delay,
 * triggering the same lte_handler path as the real modem.
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <modem/lte_lc.h>

static lte_lc_evt_handler_t stored_handler;

static void lte_connect_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	if (!stored_handler) {
		return;
	}

	struct lte_lc_evt evt = {
		.type          = LTE_LC_EVT_NW_REG_STATUS,
		.nw_reg_status = LTE_LC_NW_REG_REGISTERED_ROAMING,
	};
	stored_handler(&evt);
}

K_TIMER_DEFINE(lte_connect_timer, lte_connect_timer_fn, NULL);

int lte_lc_connect_async(lte_lc_evt_handler_t handler)
{
	stored_handler = handler;
	/* Fire after 100ms so main() gets past lte_lc_connect_async() first */
	k_timer_start(&lte_connect_timer, K_MSEC(100), K_NO_WAIT);
	return 0;
}

/* Canned neighbor-cell result: a real central-London cell (MCC 234, MNC 10,
 * TAC 4320, CID 136523890 → ~51.498,-0.1436) so the local OpenCelliD lookup
 * actually resolves it in `make demo`. */
static struct lte_lc_ncell mock_neighbors[] = {
	{ .phys_cell_id = 88,  .rsrp = 45, .rsrq = 20, .earfcn = 1617 },
	{ .phys_cell_id = 120, .rsrp = 38, .rsrq = 16, .earfcn = 1617 },
};

int lte_lc_neighbor_cell_measurement(struct lte_lc_ncellmeas_params *params)
{
	ARG_UNUSED(params);
	if (!stored_handler) {
		return -EFAULT;
	}

	struct lte_lc_evt evt = {
		.type = LTE_LC_EVT_NEIGHBOR_CELL_MEAS,
		.cells_info = {
			.current_cell = {
				.mcc          = 234,
				.mnc          = 10,
				.id           = 136523890,
				.tac          = 4320,
				.phys_cell_id = 402,
				.rsrp         = 45,
				.rsrq         = 20,
				.earfcn       = 1617,
			},
			.ncells_count  = 2,
			.neighbor_cells = mock_neighbors,
		},
	};
	/* Deliver synchronously — main() waits on cell_sem right after. */
	stored_handler(&evt);
	return 0;
}

int lte_lc_offline(void)   { return 0; }
int lte_lc_power_off(void) { return 0; }
int lte_lc_normal(void)    { return 0; }
