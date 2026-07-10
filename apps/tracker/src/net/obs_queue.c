#include "obs_queue.h"
#include "coap_pub.h"
#include "tracker_encode.h"

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(obs_queue, CONFIG_TRACKER_LOG_LEVEL);

/* Must match the array bound in proto/tracker.cddl (15: [1*24 entry]). */
#define QUEUE_CAP 24

/* A GPS sample this close to the previous one becomes a repeat entry.
 * Thresholds in degrees*1e7: 900 ≈ 10 m of latitude; 1400 ≈ 10 m of
 * longitude at 52° N (scaled by cos(lat) -- close enough for a dedup gate). */
#define REPEAT_LAT_E7 900
#define REPEAT_LON_E7 1400

enum obs_kind { OBS_GPS, OBS_CELL, OBS_REPEAT };

struct obs {
	enum obs_kind kind;
	int64_t t_ms; /* uptime when observed */
	union {
		struct {
			int32_t lat_e7, lon_e7, alt_dm;
			uint32_t acc_dm, spd_dms, hdg_ddeg, sats;
		} gps;
		struct {
			uint32_t mcc, mnc, tac, cid;
			int32_t rsrp;
		} cell;
	};
};

static struct obs queue[QUEUE_CAP];
static size_t q_len;
static const char *dev_id;
/* Base position the next repeat would be judged against. */
static int32_t base_lat_e7, base_lon_e7;
static bool base_valid;

void obs_queue_init(const char *device_id)
{
	dev_id = device_id;
	q_len = 0;
	base_valid = false;
}

static void push(const struct obs *o)
{
	if (q_len == QUEUE_CAP) {
		/* Keep the newest data: the oldest entry is the least useful
		 * and its age field would be the least precise anyway. */
		memmove(&queue[0], &queue[1], (QUEUE_CAP - 1) * sizeof(queue[0]));
		q_len--;
	}
	queue[q_len++] = *o;
}

void obs_queue_add_gps(const struct nrf_modem_gnss_pvt_data_frame *p,
		       int64_t fix_uptime_ms)
{
	int32_t lat = (int32_t)llround(p->latitude * 1e7);
	int32_t lon = (int32_t)llround(p->longitude * 1e7);

	/* Unmoved (and not the first sample): 4 B instead of ~33 B. */
	if (base_valid &&
	    (lat - base_lat_e7 < REPEAT_LAT_E7) && (base_lat_e7 - lat < REPEAT_LAT_E7) &&
	    (lon - base_lon_e7 < REPEAT_LON_E7) && (base_lon_e7 - lon < REPEAT_LON_E7)) {
		struct obs o = { .kind = OBS_REPEAT, .t_ms = fix_uptime_ms };

		push(&o);
		return;
	}

	bool vel = (p->flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID);
	uint8_t used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (p->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			used++;
		}
	}

	struct obs o = {
		.kind = OBS_GPS,
		.t_ms = fix_uptime_ms,
		.gps = {
			.lat_e7 = lat,
			.lon_e7 = lon,
			.alt_dm = (int32_t)lroundf(p->altitude * 10.0f),
			.acc_dm = (uint32_t)lroundf(p->accuracy * 10.0f),
			.spd_dms = vel ? (uint32_t)lroundf(p->speed * 10.0f) : 0,
			.hdg_ddeg = vel ? (uint32_t)lroundf(p->heading * 10.0f) : 0,
			.sats = used,
		},
	};

	push(&o);
	base_lat_e7 = lat;
	base_lon_e7 = lon;
	base_valid = true;
}

void obs_queue_add_cell(int mcc, int mnc, uint32_t tac, uint32_t cid,
			int rsrp_dbm, int64_t obs_uptime_ms)
{
	struct obs o = {
		.kind = OBS_CELL,
		.t_ms = obs_uptime_ms,
		.cell = { .mcc = mcc, .mnc = mnc, .tac = tac, .cid = cid,
			  .rsrp = rsrp_dbm },
	};

	push(&o);
	/* A cell interlude breaks the repeat chain: the next GPS sample must be
	 * a full entry so the server never resolves a repeat against a cell. */
	base_valid = false;
}

size_t obs_queue_len(void)
{
	return q_len;
}

bool obs_queue_has_cell(void)
{
	for (size_t i = 0; i < q_len; i++) {
		if (queue[i].kind == OBS_CELL) {
			return true;
		}
	}
	return false;
}

int obs_queue_flush(int64_t now_ms)
{
	/* 24 entries * ~33 B + envelope stays well inside this. Static: the
	 * batch struct alone is ~1.5 KB and the main stack is 4 KB. */
	static uint8_t buf[1024];
	static struct batch b;

	if (q_len == 0) {
		return -ENODATA;
	}

	memset(&b, 0, sizeof(b));
	b.batch_device_id_m.value = (const uint8_t *)dev_id;
	b.batch_device_id_m.len = strlen(dev_id);
	b.batch_entry_m_l_entry_m_count = q_len;

	for (size_t i = 0; i < q_len; i++) {
		struct entry_r *e = &b.batch_entry_m_l_entry_m[i];
		uint32_t age = (uint32_t)((now_ms - queue[i].t_ms) / 1000);

		/* Schema caps age at 7200; a stuck queue must not fail encode. */
		age = MIN(age, 7200);

		switch (queue[i].kind) {
		case OBS_GPS:
			e->entry_choice = gps_entry_m_c;
			e->gps_entry_m = (struct gps_entry){
				.gps_entry_age_s_m = age,
				.gps_entry_lat_e7_m = queue[i].gps.lat_e7,
				.gps_entry_lon_e7_m = queue[i].gps.lon_e7,
				.gps_entry_alt_dm_m = queue[i].gps.alt_dm,
				.gps_entry_acc_dm_m = queue[i].gps.acc_dm,
				.gps_entry_spd_dms_m = queue[i].gps.spd_dms,
				.gps_entry_hdg_ddeg_m = queue[i].gps.hdg_ddeg,
				.gps_entry_sats_m = queue[i].gps.sats,
			};
			break;
		case OBS_CELL:
			e->entry_choice = cell_entry_m_c;
			e->cell_entry_m = (struct cell_entry){
				.cell_entry_age_s_m = age,
				.cell_entry_mcc_m = queue[i].cell.mcc,
				.cell_entry_mnc_m = queue[i].cell.mnc,
				.cell_entry_tac_m = queue[i].cell.tac,
				.cell_entry_cell_id_m = queue[i].cell.cid,
				.cell_entry_rsrp_dbm_m = queue[i].cell.rsrp,
			};
			break;
		case OBS_REPEAT:
			e->entry_choice = repeat_entry_m_c;
			e->repeat_entry_m.repeat_entry_age_s_m = age;
			break;
		}
	}

	size_t len;
	int err = cbor_encode_batch(buf, sizeof(buf), &b, &len);

	if (err) {
		LOG_ERR("cbor_encode_batch: %d (dropping %u entries)", err,
			(unsigned)q_len);
		/* An unencodable queue would wedge forever; dropping is the
		 * lesser evil and the next samples start clean. */
		q_len = 0;
		return -EINVAL;
	}

	err = coap_pub_send(buf, len);
	if (err) {
		return err; /* keep the queue; retry on the next flush */
	}

	LOG_INF("flushed %u obs (%zu B payload)", (unsigned)q_len, len);
	q_len = 0;
	return 0;
}
