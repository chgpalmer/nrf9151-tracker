/*
 * Observation queue: sample locally, transmit in batches.
 *
 * Sampling is free (no radio); transmitting is not. Entries accumulate here
 * with their capture uptime and go out as one CBOR batch (proto/tracker.cddl),
 * each entry carrying its age at send time -- the device never needs a wall
 * clock. A GPS sample within ~10 m of the previous one is stored as a repeat
 * entry (~4 B): alive and unmoved, at a tenth of the cost.
 */
#ifndef OBS_QUEUE_H__
#define OBS_QUEUE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

void obs_queue_init(const char *device_id);

/* Queue one GPS observation from a fix-valid PVT snapshot captured at
 * fix_uptime_ms. Becomes a repeat entry if within ~10 m of the last one. */
void obs_queue_add_gps(const struct nrf_modem_gnss_pvt_data_frame *p,
		       int64_t fix_uptime_ms);

void obs_queue_add_cell(int mcc, int mnc, uint32_t tac, uint32_t cid,
			int rsrp_dbm, int64_t obs_uptime_ms);

size_t obs_queue_len(void);
bool obs_queue_has_cell(void);

/* Encode everything queued and send it as one CoAP POST. On success the queue
 * empties; on failure entries are kept for the next attempt. -ENODATA when
 * there is nothing to send. */
int obs_queue_flush(int64_t now_ms);

#endif /* OBS_QUEUE_H__ */
