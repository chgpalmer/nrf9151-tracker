#include "obs_queue.h"
#include "uplink.h"
#include "tracker.pb.h"

#include <pb_encode.h>

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(obs_queue, CONFIG_TRACKER_LOG_LEVEL);

/* ~34 minutes of 1 Hz sampling (~96 KB): rides out LTE dropouts in RAM.
 * Beyond that the ring drops oldest — the flash spill tier is phase 2. */
#define GPS_RING_CAP 2048
#define CELL_RING_CAP 8

/* Points in one track segment (one datagram): worst case ~7.7 B/point keeps
 * the datagram inside uplink's budget; the exact-size check below enforces
 * it precisely, this is just the search ceiling. */
#define MAX_SEG_POINTS 50

/* A sample this far off the nominal cadence breaks the run: a segment's time
 * model is anchor + i*dt, so a gap must start a new anchor rather than smear
 * every timestamp after it. */
#define DT_SLACK(dt) ((int64_t)(dt) / 2)

struct gps_obs {
	int64_t t_ms;
	int32_t lat_e7, lon_e7, alt_dm;
	uint32_t acc_dm, spd_dms, hdg_ddeg, sats;
};

struct cell_obs {
	int64_t t_ms;
	uint32_t mcc, mnc, tac, cid;
	int32_t rsrp;
	uint32_t act; /* 3GPP AcT: 7 = LTE-M, 9 = NB-IoT */
};

/* Circular rings with a transactional read cursor: `taken` marks entries
 * handed to uplink for the in-flight datagram; commit() pops them, rollback()
 * re-offers them. push() never blocks — full means drop-oldest. */
static struct gps_obs gps_ring[GPS_RING_CAP];
static size_t gps_start, gps_count, gps_taken;
static uint32_t gps_dropped;

static struct cell_obs cell_ring[CELL_RING_CAP];
static size_t cell_start, cell_count, cell_taken;

static uint32_t sample_dt_ms = 1000;

static inline struct gps_obs *gps_at(size_t i)
{
	return &gps_ring[(gps_start + i) % GPS_RING_CAP];
}

void obs_queue_add_gps(const struct nrf_modem_gnss_pvt_data_frame *p,
		       int64_t fix_uptime_ms)
{
	bool vel = (p->flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID);
	uint8_t used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (p->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			used++;
		}
	}

	if (gps_count == GPS_RING_CAP) {
		/* Keep the newest data; the oldest is the least useful. Say so
		 * once per burst — the line itself travels by uplink later. */
		if (gps_dropped++ == 0) {
			LOG_WRN("gps ring full — dropping oldest");
		}
		gps_start = (gps_start + 1) % GPS_RING_CAP;
		gps_count--;
		if (gps_taken > 0) {
			gps_taken--; /* the in-flight cursor moved with it */
		}
	}

	*gps_at(gps_count) = (struct gps_obs){
		.t_ms = fix_uptime_ms,
		.lat_e7 = (int32_t)llround(p->latitude * 1e7),
		.lon_e7 = (int32_t)llround(p->longitude * 1e7),
		.alt_dm = (int32_t)lroundf(p->altitude * 10.0f),
		.acc_dm = (uint32_t)lroundf(p->accuracy * 10.0f),
		.spd_dms = vel ? (uint32_t)lroundf(p->speed * 10.0f) : 0,
		.hdg_ddeg = vel ? (uint32_t)lroundf(p->heading * 10.0f) : 0,
		.sats = used,
	};
	gps_count++;

	/* Pressure valve, not a cadence: flushing every full segment made one
	 * RRC session per ~50 s of riding, and the session's fixed radio hold
	 * (~5-10 s) chopped GNSS each time. Let the flush timer batch several
	 * segments per session; announce fullness only under real pressure. */
	if (gps_count - gps_taken >= CONFIG_TRACKER_OBS_FLUSH_PENDING) {
		uplink_request_flush(UPLINK_FLUSH_FULL);
	}
}

void obs_queue_add_cell(int mcc, int mnc, uint32_t tac, uint32_t cid,
			int rsrp_dbm, int act, int64_t obs_uptime_ms)
{
	if (cell_count == CELL_RING_CAP) {
		cell_start = (cell_start + 1) % CELL_RING_CAP;
		cell_count--;
		if (cell_taken > 0) {
			cell_taken--;
		}
	}
	cell_ring[(cell_start + cell_count) % CELL_RING_CAP] = (struct cell_obs){
		.t_ms = obs_uptime_ms,
		.mcc = mcc, .mnc = mnc, .tac = tac, .cid = cid,
		.rsrp = rsrp_dbm, .act = act,
	};
	cell_count++;

	/* A cell fix gates the FSM's move to GNSS acquisition: urgent. */
	uplink_request_flush(UPLINK_FLUSH_URGENT);
}

static uint32_t age_of(int64_t t_ms)
{
	int64_t age = (k_uptime_get() - t_ms) / 1000;

	return (uint32_t)CLAMP(age, 0, 7200);
}

/* ---- gps segment source (prio 1) ----------------------------------------
 * Encodes one TrackSegment Entry per encode_next() call, sized EXACTLY via a
 * nanopb sizing pass and shrunk until it fits the offered budget. */

/* Which run of the ring the packed-array callbacks emit: points
 * [seg_base, seg_base + seg_count) in unconsumed-ring coordinates. */
static size_t seg_base, seg_count;

/* How many consecutive samples from `from` form one segment: cadence within
 * tolerance, capped at MAX_SEG_POINTS. */
static size_t seg_span(size_t from)
{
	size_t n = 1;

	while (from + n < gps_count && n < MAX_SEG_POINTS) {
		int64_t gap = gps_at(from + n)->t_ms - gps_at(from + n - 1)->t_ms;

		if (gap < (int64_t)sample_dt_ms - DT_SLACK(sample_dt_ms) ||
		    gap > (int64_t)sample_dt_ms + DT_SLACK(sample_dt_ms)) {
			break;
		}
		n++;
	}
	return n;
}

#define ENCODE_PACKED(fn_name, emit)                                            \
	static bool fn_name(pb_ostream_t *stream, const pb_field_t *field,      \
			    void *const *arg)                                    \
	{                                                                        \
		pb_ostream_t sizer = PB_OSTREAM_SIZING;                          \
                                                                                 \
		ARG_UNUSED(arg);                                                 \
		for (size_t i = 1; i < seg_count; i++) {                         \
			if (!emit(&sizer, i)) {                                  \
				return false;                                    \
			}                                                        \
		}                                                                \
		if (!pb_encode_tag(stream, PB_WT_STRING, field->tag) ||         \
		    !pb_encode_varint(stream, sizer.bytes_written)) {           \
			return false;                                            \
		}                                                                \
		for (size_t i = 1; i < seg_count; i++) {                         \
			if (!emit(stream, i)) {                                  \
				return false;                                    \
			}                                                        \
		}                                                                \
		return true;                                                     \
	}

static bool emit_dlat(pb_ostream_t *st, size_t i)
{
	return pb_encode_svarint(st, gps_at(seg_base + i)->lat_e7 -
					     gps_at(seg_base + i - 1)->lat_e7);
}

static bool emit_dlon(pb_ostream_t *st, size_t i)
{
	return pb_encode_svarint(st, gps_at(seg_base + i)->lon_e7 -
					     gps_at(seg_base + i - 1)->lon_e7);
}

static bool emit_spd(pb_ostream_t *st, size_t i)
{
	return pb_encode_varint(st, gps_at(seg_base + i)->spd_dms);
}

ENCODE_PACKED(encode_dlat, emit_dlat)
ENCODE_PACKED(encode_dlon, emit_dlon)
ENCODE_PACKED(encode_spd, emit_spd)

static void fill_track_entry(Entry *e, size_t base)
{
	const struct gps_obs *a = gps_at(base);

	*e = (Entry)Entry_init_zero;
	e->age_s = age_of(a->t_ms);
	e->which_kind = Entry_track_tag;
	e->kind.track.dt_ms = sample_dt_ms;
	e->kind.track.has_anchor = true;
	e->kind.track.anchor.lat_e7 = a->lat_e7;
	e->kind.track.anchor.lon_e7 = a->lon_e7;
	e->kind.track.anchor.alt_dm = a->alt_dm;
	e->kind.track.anchor.acc_dm = a->acc_dm;
	e->kind.track.anchor.spd_dms = a->spd_dms;
	e->kind.track.anchor.hdg_ddeg = a->hdg_ddeg;
	e->kind.track.anchor.sats = a->sats;
	e->kind.track.dlat.funcs.encode = encode_dlat;
	e->kind.track.dlon.funcs.encode = encode_dlon;
	e->kind.track.spd_dms.funcs.encode = encode_spd;
}

static bool gps_has_pending(void)
{
	return gps_count > gps_taken;
}

static int gps_encode_next(pb_ostream_t *stream, size_t budget, uint32_t *kind)
{
	if (!gps_has_pending()) {
		return 0;
	}

	Entry e;
	size_t n = seg_span(gps_taken);

	/* Exact fit: size the submessage (cheap — no output), shrink the
	 * segment until it fits the budget. Uplink already wrote the tag. */
	for (;;) {
		seg_base = gps_taken;
		seg_count = n;
		fill_track_entry(&e, gps_taken);

		size_t sz;

		if (!pb_get_encoded_size(&sz, Entry_fields, &e)) {
			return -EINVAL;
		}
		size_t total = sz + 2; /* + length prefix (sz < 16384 always) */

		if (total <= budget) {
			break;
		}
		if (n <= 1) {
			return 0; /* not even one point fits this datagram */
		}
		/* Points are homogeneous: scale down proportionally, retry. */
		size_t fixed = 40; /* anchor + headers, roughly */
		size_t per = (sz > fixed && n > 1) ? (sz - fixed) / n : 8;
		size_t want = per ? (budget > fixed ? (budget - fixed) / per : 1) : 1;

		n = MAX(1, MIN(n - 1, want));
	}

	size_t before = stream->bytes_written;

	if (!pb_encode_submessage(stream, Entry_fields, &e)) {
		return -EIO;
	}
	gps_taken += n;
	*kind = UPLINK_KIND_TRACK;
	return (int)(stream->bytes_written - before);
}

static void gps_commit(void)
{
	gps_start = (gps_start + gps_taken) % GPS_RING_CAP;
	gps_count -= gps_taken;
	gps_taken = 0;
}

static void gps_rollback(void)
{
	gps_taken = 0;
}

static const struct uplink_source gps_source = {
	.name = "gps",
	.has_pending = gps_has_pending,
	.encode_next = gps_encode_next,
	.commit = gps_commit,
	.rollback = gps_rollback,
};

/* ---- cell source (prio 0) ------------------------------------------------ */

static bool cell_has_pending(void)
{
	return cell_count > cell_taken;
}

static int cell_encode_next(pb_ostream_t *stream, size_t budget, uint32_t *kind)
{
	if (!cell_has_pending()) {
		return 0;
	}

	const struct cell_obs *c =
		&cell_ring[(cell_start + cell_taken) % CELL_RING_CAP];
	Entry e = Entry_init_zero;

	e.age_s = age_of(c->t_ms);
	e.which_kind = Entry_cell_tag;
	e.kind.cell.mcc = c->mcc;
	e.kind.cell.mnc = c->mnc;
	e.kind.cell.tac = c->tac;
	e.kind.cell.cell_id = c->cid;
	e.kind.cell.rsrp_dbm = c->rsrp;
	e.kind.cell.act = c->act;

	size_t sz;

	if (!pb_get_encoded_size(&sz, Entry_fields, &e) || sz + 2 > budget) {
		return sz + 2 > budget ? 0 : -EINVAL;
	}

	size_t before = stream->bytes_written;

	if (!pb_encode_submessage(stream, Entry_fields, &e)) {
		return -EIO;
	}
	cell_taken++;
	*kind = UPLINK_KIND_CELL;
	return (int)(stream->bytes_written - before);
}

static void cell_commit(void)
{
	cell_start = (cell_start + cell_taken) % CELL_RING_CAP;
	cell_count -= cell_taken;
	cell_taken = 0;
}

static void cell_rollback(void)
{
	cell_taken = 0;
}

static const struct uplink_source cell_source = {
	.name = "cell",
	.has_pending = cell_has_pending,
	.encode_next = cell_encode_next,
	.commit = cell_commit,
	.rollback = cell_rollback,
};

/* ---- init ----------------------------------------------------------------- */

void obs_queue_init(uint32_t dt_ms)
{
	sample_dt_ms = dt_ms ? dt_ms : 1000;
	gps_start = gps_count = gps_taken = 0;
	cell_start = cell_count = cell_taken = 0;
	uplink_register(&cell_source, 0);
	uplink_register(&gps_source, 1);
}

size_t obs_queue_len(void)
{
	return gps_count + cell_count;
}
