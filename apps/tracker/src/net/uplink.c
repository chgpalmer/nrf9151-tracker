#include "uplink.h"
#include "coap_pub.h"
#include "tracker.pb.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Everything in here logs at DBG: this module sits under the log-uplink
 * backend, and its own lines re-entering the send path is the one loop this
 * architecture must never close. DBG stays below the backend's threshold. */
LOG_MODULE_REGISTER(uplink, CONFIG_TRACKER_LOG_LEVEL);

#define FLUSH_INTERVAL_MS (CONFIG_TRACKER_FLUSH_INTERVAL_S * 1000)
#define LOG_WAKE_MIN_MS   (CONFIG_TRACKER_LOG_WAKE_MIN_S * 1000)

/* Per-datagram payload budget: stays inside the ~512 B safe cellular MTU
 * (IPv6-safe) with CoAP overhead on top. */
#define DATAGRAM_BUDGET 450

static const struct uplink_source *sources[UPLINK_MAX_SOURCES];
static const char *dev_id;
static bool allowed;
static void (*sent_cb)(uint32_t kinds_mask);

static volatile bool want_flush;      /* deferred flush requests land here */
static volatile bool want_flush_log;  /* ...log-urgent ones here (rate-limited) */
static int64_t last_flush_ms;
static int64_t last_log_wake_ms = -LOG_WAKE_MIN_MS;

void uplink_init(const char *device_id)
{
	dev_id = device_id;
	/* Backdated so the FIRST queued observation flushes immediately: a
	 * fresh boot should appear on the map within seconds. */
	last_flush_ms = -FLUSH_INTERVAL_MS;
}

int uplink_register(const struct uplink_source *src, uint8_t prio)
{
	if (prio >= UPLINK_MAX_SOURCES || sources[prio] != NULL) {
		return -EINVAL;
	}
	sources[prio] = src;
	return 0;
}

void uplink_set_allowed(bool ok)
{
	allowed = ok;
}

void uplink_request_flush(enum uplink_flush_why why)
{
	if (why == UPLINK_FLUSH_LOG) {
		want_flush_log = true;
	} else {
		want_flush = true;
	}
}

void uplink_on_sent(void (*cb)(uint32_t kinds_mask))
{
	sent_cb = cb;
}

static bool any_pending(void)
{
	for (int i = 0; i < UPLINK_MAX_SOURCES; i++) {
		if (sources[i] && sources[i]->has_pending()) {
			return true;
		}
	}
	return false;
}

/* nanopb callback: emit the device id string. */
static bool encode_device_id(pb_ostream_t *stream, const pb_field_t *field,
			     void *const *arg)
{
	const char *id = *arg;

	return pb_encode_tag(stream, PB_WT_STRING, field->tag) &&
	       pb_encode_string(stream, (const pb_byte_t *)id, strlen(id));
}

/* Which sources contributed to the datagram being assembled, so commit and
 * rollback go only to the ones whose cursors actually moved. */
struct asm_state {
	uint32_t kinds;
	bool touched[UPLINK_MAX_SOURCES];
};

static struct asm_state cur; /* datagram being assembled (single-threaded) */

/* nanopb callback for Obs.entries: the drain of one datagram. Ask sources in
 * priority order to contribute entries while budget remains. */
static bool encode_entries(pb_ostream_t *stream, const pb_field_t *field,
			   void *const *arg)
{
	ARG_UNUSED(arg);

	for (int i = 0; i < UPLINK_MAX_SOURCES; i++) {
		if (!sources[i]) {
			continue;
		}
		for (;;) {
			size_t left = DATAGRAM_BUDGET > stream->bytes_written
				? DATAGRAM_BUDGET - stream->bytes_written : 0;
			if (left < 8) {
				return true; /* full enough; next datagram */
			}

			/* The source encodes a complete Entry submessage; the
			 * field tag comes first so it can size against budget
			 * honestly. */
			uint32_t kind = 0;
			pb_ostream_t before = *stream;
			int n;

			if (!pb_encode_tag(stream, PB_WT_STRING, field->tag)) {
				return false;
			}
			n = sources[i]->encode_next(stream, left, &kind);
			if (n <= 0) {
				*stream = before; /* un-write the tag */
				if (n < 0) {
					LOG_DBG("source %s encode error %d",
						sources[i]->name, n);
				}
				break; /* next source */
			}
			cur.kinds |= kind;
			cur.touched[i] = true;
		}
	}
	return true;
}

static int send_one_datagram(void)
{
	/* Static: off the main stack; > budget so overruns hit the MTU, not
	 * silent memory corruption. */
	static uint8_t buf[640];
	Obs obs = Obs_init_zero;
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	memset(&cur, 0, sizeof(cur));

	obs.version = 3;
	obs.device_id.funcs.encode = encode_device_id;
	obs.device_id.arg = (void *)dev_id;
	obs.entries.funcs.encode = encode_entries;

	if (!pb_encode(&stream, Obs_fields, &obs)) {
		LOG_ERR("pb_encode: %s", PB_GET_ERROR(&stream));
		/* Roll back cursors; a poisoned source will fail again, but a
		 * transient (concurrent ring wrap) recovers. */
		for (int i = 0; i < UPLINK_MAX_SOURCES; i++) {
			if (cur.touched[i]) {
				sources[i]->rollback();
			}
		}
		return -EINVAL;
	}

	if (cur.kinds == 0) {
		return 0; /* nothing was pending after all */
	}

	int err = coap_pub_send(buf, stream.bytes_written);

	for (int i = 0; i < UPLINK_MAX_SOURCES; i++) {
		if (!cur.touched[i]) {
			continue;
		}
		if (err) {
			sources[i]->rollback();
		} else {
			sources[i]->commit();
		}
	}
	if (err) {
		return err;
	}

	LOG_DBG("sent %zu B (kinds 0x%x)", stream.bytes_written, cur.kinds);
	if (sent_cb) {
		sent_cb(cur.kinds);
	}
	return 0;
}

int uplink_poll(int64_t now_ms)
{
	bool due = false;

	if (want_flush) {
		due = true;
	}
	if (want_flush_log && now_ms - last_log_wake_ms >= LOG_WAKE_MIN_MS) {
		due = true;
	}
	if (now_ms - last_flush_ms >= FLUSH_INTERVAL_MS) {
		due = true;
	}

	if (!due || !allowed || !coap_pub_ready() || !any_pending()) {
		return 0;
	}

	/* Consume the request flags now: anything requested after this point
	 * belongs to the next wake. */
	want_flush = false;
	if (want_flush_log) {
		want_flush_log = false;
		last_log_wake_ms = now_ms;
	}

	/* One radio wake, as many datagrams as it takes. RAI's LAST hint goes
	 * on every send (coap_pub does it); consecutive sends land in the same
	 * RRC window, so only the final one's hint matters in practice. */
	int sent = 0;

	while (any_pending()) {
		int err = send_one_datagram();

		if (err) {
			LOG_DBG("send failed (%d) after %d datagrams", err, sent);
			return err; /* sources rolled back; retry next flush */
		}
		sent++;
		if (sent > 64) {
			break; /* safety valve; the rest goes next wake */
		}
	}
	last_flush_ms = now_ms;
	return 0;
}
