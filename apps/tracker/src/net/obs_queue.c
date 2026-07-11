#include "obs_queue.h"
#include "coap_pub.h"
#include "tracker.pb.h"

#include <pb_encode.h>

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(obs_queue, CONFIG_TRACKER_LOG_LEVEL);

/* One flush's worth of samples, plus headroom for cell interludes. */
#define QUEUE_CAP 64

/* Points in one track segment. Sized so the worst case (~7.7 B/point at highway
 * speed) stays inside the ~512 B safe cellular MTU even on IPv6, so UDP never
 * fragments. The count is fixed BEFORE pb_encode runs and never trimmed during
 * it: packed arrays are length-prefixed and written field-by-field, so stopping
 * a callback on a byte budget would truncate dlat and desync it from dlon/spd.
 * A longer flush simply emits several datagrams, each with its own anchor. */
#define MAX_SEG_POINTS 50

/* A sample this far off the nominal cadence breaks the run: a segment's time
 * model is anchor + i*dt, so a gap (GNSS dropout, state change) must start a new
 * anchor rather than silently smear every timestamp after it. */
#define DT_SLACK(dt) ((int64_t)(dt) / 2)

enum obs_kind { OBS_GPS, OBS_CELL };

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
static uint32_t sample_dt_ms = 1000;

void obs_queue_init(const char *device_id, uint32_t dt_ms)
{
	dev_id = device_id;
	q_len = 0;
	sample_dt_ms = dt_ms ? dt_ms : 1000;
}

static void push(const struct obs *o)
{
	if (q_len == QUEUE_CAP) {
		/* Keep the newest data: the oldest sample is the least useful and
		 * its age field would be the least precise anyway. */
		memmove(&queue[0], &queue[1], (QUEUE_CAP - 1) * sizeof(queue[0]));
		q_len--;
	}
	queue[q_len++] = *o;
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

	struct obs o = {
		.kind = OBS_GPS,
		.t_ms = fix_uptime_ms,
		.gps = {
			.lat_e7 = (int32_t)llround(p->latitude * 1e7),
			.lon_e7 = (int32_t)llround(p->longitude * 1e7),
			.alt_dm = (int32_t)lroundf(p->altitude * 10.0f),
			.acc_dm = (uint32_t)lroundf(p->accuracy * 10.0f),
			.spd_dms = vel ? (uint32_t)lroundf(p->speed * 10.0f) : 0,
			.hdg_ddeg = vel ? (uint32_t)lroundf(p->heading * 10.0f) : 0,
			.sats = used,
		},
	};

	push(&o);
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

/* ---- segment building --------------------------------------------------- */

/* How many consecutive GNSS samples from `start` form one segment: GPS kind,
 * cadence within tolerance, capped at MAX_SEG_POINTS. */
static size_t seg_span(size_t start)
{
	size_t n = 1;

	while (start + n < q_len && n < MAX_SEG_POINTS &&
	       queue[start + n].kind == OBS_GPS) {
		int64_t gap = queue[start + n].t_ms - queue[start + n - 1].t_ms;

		if (gap < (int64_t)sample_dt_ms - DT_SLACK(sample_dt_ms) ||
		    gap > (int64_t)sample_dt_ms + DT_SLACK(sample_dt_ms)) {
			break; /* cadence broke -- the next point needs a new anchor */
		}
		n++;
	}
	return n;
}

/* Which run of the ring a packed-array callback should emit. */
struct seg_ctx {
	size_t start; /* index of the anchor (point 0) */
	size_t count; /* points including the anchor */
};

/* A packed repeated field encoded from a callback must write its own tag and
 * length prefix, and the length is only known once the payload has been sized --
 * hence the two passes (nanopb's PB_OSTREAM_SIZING just counts bytes).
 *
 * Deltas are sint32 varints, so a large GNSS jump merely costs an extra byte or
 * two: unlike a fixed-width packed layout there is no overflow to guard against,
 * and a segment only ever breaks for a real reason (a cadence gap). */
#define ENCODE_PACKED(fn_name, emit)                                            \
	static bool fn_name(pb_ostream_t *stream, const pb_field_t *field,      \
			    void *const *arg)                                   \
	{                                                                       \
		const struct seg_ctx *s = *arg;                                 \
		pb_ostream_t sizer = PB_OSTREAM_SIZING;                         \
                                                                                \
		for (size_t i = 1; i < s->count; i++) {                         \
			if (!emit(&sizer, s, i)) {                              \
				return false;                                   \
			}                                                       \
		}                                                               \
		if (!pb_encode_tag(stream, PB_WT_STRING, field->tag) ||         \
		    !pb_encode_varint(stream, sizer.bytes_written)) {           \
			return false;                                           \
		}                                                               \
		for (size_t i = 1; i < s->count; i++) {                         \
			if (!emit(stream, s, i)) {                              \
				return false;                                   \
			}                                                       \
		}                                                               \
		return true;                                                    \
	}

static bool emit_dlat(pb_ostream_t *st, const struct seg_ctx *s, size_t i)
{
	return pb_encode_svarint(st, queue[s->start + i].gps.lat_e7 -
					     queue[s->start + i - 1].gps.lat_e7);
}

static bool emit_dlon(pb_ostream_t *st, const struct seg_ctx *s, size_t i)
{
	return pb_encode_svarint(st, queue[s->start + i].gps.lon_e7 -
					     queue[s->start + i - 1].gps.lon_e7);
}

static bool emit_spd(pb_ostream_t *st, const struct seg_ctx *s, size_t i)
{
	return pb_encode_varint(st, queue[s->start + i].gps.spd_dms);
}

ENCODE_PACKED(encode_dlat, emit_dlat)
ENCODE_PACKED(encode_dlon, emit_dlon)
ENCODE_PACKED(encode_spd, emit_spd)

static bool encode_device_id(pb_ostream_t *stream, const pb_field_t *field,
			     void *const *arg)
{
	const char *id = *arg;

	return pb_encode_tag(stream, PB_WT_STRING, field->tag) &&
	       pb_encode_string(stream, (const pb_byte_t *)id, strlen(id));
}

/* ---- flush -------------------------------------------------------------- */

/* Which slice of the ring goes into the datagram currently being encoded. */
struct flush_ctx {
	int64_t now_ms;
	size_t to; /* encode queue[0 .. to) */
};

static uint32_t age_of(size_t i, int64_t now_ms)
{
	int64_t age = (now_ms - queue[i].t_ms) / 1000;

	return (uint32_t)CLAMP(age, 0, 7200);
}

static bool encode_entries(pb_ostream_t *stream, const pb_field_t *field,
			   void *const *arg)
{
	const struct flush_ctx *f = *arg;
	size_t i = 0;

	while (i < f->to) {
		/* Must outlive this iteration: the delta callbacks run inside
		 * pb_encode_submessage() below and dereference it. */
		static struct seg_ctx seg;
		Entry e = Entry_init_zero;

		e.age_s = age_of(i, f->now_ms);

		if (queue[i].kind == OBS_CELL) {
			e.which_kind = Entry_cell_tag;
			e.kind.cell.mcc = queue[i].cell.mcc;
			e.kind.cell.mnc = queue[i].cell.mnc;
			e.kind.cell.tac = queue[i].cell.tac;
			e.kind.cell.cell_id = queue[i].cell.cid;
			e.kind.cell.rsrp_dbm = queue[i].cell.rsrp;
			i++;
		} else {
			seg.start = i;
			seg.count = seg_span(i);

			e.which_kind = Entry_track_tag;
			e.kind.track.dt_ms = sample_dt_ms;
			e.kind.track.has_anchor = true;
			e.kind.track.anchor.lat_e7 = queue[i].gps.lat_e7;
			e.kind.track.anchor.lon_e7 = queue[i].gps.lon_e7;
			e.kind.track.anchor.alt_dm = queue[i].gps.alt_dm;
			e.kind.track.anchor.acc_dm = queue[i].gps.acc_dm;
			e.kind.track.anchor.spd_dms = queue[i].gps.spd_dms;
			e.kind.track.anchor.hdg_ddeg = queue[i].gps.hdg_ddeg;
			e.kind.track.anchor.sats = queue[i].gps.sats;
			e.kind.track.dlat.funcs.encode = encode_dlat;
			e.kind.track.dlat.arg = &seg;
			e.kind.track.dlon.funcs.encode = encode_dlon;
			e.kind.track.dlon.arg = &seg;
			e.kind.track.spd_dms.funcs.encode = encode_spd;
			e.kind.track.spd_dms.arg = &seg;
			i += seg.count;
		}

		if (!pb_encode_tag(stream, PB_WT_STRING, field->tag) ||
		    !pb_encode_submessage(stream, Entry_fields, &e)) {
			return false;
		}
	}
	return true;
}

/* How many queue slots the next datagram takes: the cell entries at the head,
 * plus at most one track segment. Decided up front so the encode never has to
 * stop early (see MAX_SEG_POINTS). */
static size_t next_chunk(void)
{
	size_t i = 0;

	while (i < q_len && queue[i].kind == OBS_CELL) {
		i++;
	}
	if (i < q_len) {
		i += seg_span(i); /* one segment per datagram keeps it under the MTU */
	}
	return i;
}

int obs_queue_flush(int64_t now_ms)
{
	/* Static: keeps it off the 4 KB main stack. Larger than the 512 B MTU
	 * target so an undersized chunk is caught by the network, not by a silent
	 * buffer overrun here. */
	static uint8_t buf[640];

	if (q_len == 0) {
		return -ENODATA;
	}

	while (q_len > 0) {
		struct flush_ctx f = { .now_ms = now_ms, .to = next_chunk() };
		Obs obs = Obs_init_zero;
		pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

		obs.version = 3;
		obs.device_id.funcs.encode = encode_device_id;
		obs.device_id.arg = (void *)dev_id;
		obs.entries.funcs.encode = encode_entries;
		obs.entries.arg = &f;

		if (!pb_encode(&stream, Obs_fields, &obs)) {
			LOG_ERR("pb_encode: %s (dropping %u obs)",
				PB_GET_ERROR(&stream), (unsigned)q_len);
			/* An unencodable queue would wedge forever; dropping is
			 * the lesser evil and the next samples start clean. */
			q_len = 0;
			return -EINVAL;
		}

		int err = coap_pub_send(buf, stream.bytes_written);

		if (err) {
			return err; /* keep the queue; retry on the next flush */
		}

		LOG_INF("flushed %u obs (%zu B payload)", (unsigned)f.to,
			stream.bytes_written);

		q_len -= f.to;
		if (q_len > 0) {
			/* Whatever is left goes out in the next datagram. */
			memmove(&queue[0], &queue[f.to], q_len * sizeof(queue[0]));
		}
	}
	return 0;
}
