/*
 * Observation store: sample locally, ship via uplink.
 *
 * Sampling is free (no radio); transmitting is not. GPS samples land in a
 * ~34-minute RAM ring (drop-oldest beyond that, until the phase-2 flash
 * spill); cells in a small ring of their own. Both register as uplink
 * sources: uplink pulls delta-coded track segments / cell entries out at
 * flush time, and cursors advance only when a datagram is confirmed sent.
 * This module never touches the transport.
 */
#ifndef OBS_QUEUE_H__
#define OBS_QUEUE_H__

#include <stddef.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

/* dt_ms is the nominal spacing between GPS samples; a track segment's time
 * model (anchor + i*dt) is built on it. Registers the uplink sources. */
void obs_queue_init(uint32_t dt_ms);

/* Queue one GPS observation from a fix-valid PVT snapshot captured at
 * fix_uptime_ms. Never blocks; ring full = drop oldest (logged). */
void obs_queue_add_gps(const struct nrf_modem_gnss_pvt_data_frame *p,
		       int64_t fix_uptime_ms);

/* Queue a serving-cell observation; requests an urgent uplink flush (the
 * FSM's GNSS handover waits on the cell report reaching the server). */
void obs_queue_add_cell(int mcc, int mnc, uint32_t tac, uint32_t cid,
			int rsrp_dbm, int act, int64_t obs_uptime_ms);

size_t obs_queue_len(void);

#endif /* OBS_QUEUE_H__ */
