/*
 * Observation queue: sample locally, transmit in batches.
 *
 * Sampling is free (no radio); transmitting is not. Samples accumulate here with
 * their capture uptime and go out as protobuf (proto/tracker.proto), each entry
 * carrying its age at send time -- the device never needs a wall clock.
 *
 * A run of consecutive GNSS samples at a steady cadence is encoded as ONE track
 * segment: an absolute anchor fix plus packed delta arrays. That is ~5-7 B per
 * point instead of ~33 B, which is what makes high-rate sampling affordable.
 * A stationary run is simply zero deltas -- there is no separate "repeat" entry.
 */
#ifndef OBS_QUEUE_H__
#define OBS_QUEUE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

/* dt_ms is the nominal spacing between GNSS samples. A track segment's time
 * model is anchor + i*dt, so this must match how often obs_queue_add_gps() is
 * actually called; a sample landing well off that cadence starts a new segment
 * rather than silently smearing the timestamps. */
void obs_queue_init(const char *device_id, uint32_t dt_ms);

/* Queue one GPS observation from a fix-valid PVT snapshot captured at
 * fix_uptime_ms. */
void obs_queue_add_gps(const struct nrf_modem_gnss_pvt_data_frame *p,
		       int64_t fix_uptime_ms);

void obs_queue_add_cell(int mcc, int mnc, uint32_t tac, uint32_t cid,
			int rsrp_dbm, int64_t obs_uptime_ms);

size_t obs_queue_len(void);
bool obs_queue_has_cell(void);

/* Encode everything queued and send it -- as several CoAP POSTs if one datagram
 * cannot hold it all. On success the queue empties; on failure the unsent
 * remainder is kept for the next attempt. -ENODATA when there is nothing to
 * send. */
int obs_queue_flush(int64_t now_ms);

#endif /* OBS_QUEUE_H__ */
