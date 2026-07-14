#include "event_queue.h"
#include "uplink.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <pb_encode.h>
#include "tracker.pb.h"

LOG_MODULE_REGISTER(event_queue, CONFIG_TRACKER_LOG_LEVEL);

/* Events are rare (one per wake). Four is plenty of slack for a device that
 * cannot reach the server; beyond that the oldest alert is the least useful. */
#define EVENT_RING_CAP 4

struct event_rec {
	uint32_t reason;
	int64_t  t_ms;
};

static struct event_rec ring[EVENT_RING_CAP];
static uint8_t start, count, taken;

static uint32_t age_of(int64_t t_ms)
{
	int64_t age = (k_uptime_get() - t_ms) / 1000;

	return (age < 0) ? 0 : (uint32_t)age;
}

void event_queue_add(uint32_t reason, int64_t now_ms)
{
	if (count == EVENT_RING_CAP) {
		start = (start + 1) % EVENT_RING_CAP;
		count--;
		LOG_WRN("event ring full — dropped the oldest");
	}
	ring[(start + count) % EVENT_RING_CAP] = (struct event_rec){
		.reason = reason,
		.t_ms = now_ms,
	};
	count++;
	/* Jump the queue: an alert is the one thing worth a radio wake of its
	 * own. (The FSM state change requests an urgent flush too; this makes
	 * the intent explicit and covers events raised without a transition.) */
	uplink_request_flush(UPLINK_FLUSH_URGENT);
}

static bool ev_has_pending(void)
{
	return count > taken;
}

static int ev_encode_next(pb_ostream_t *stream, size_t budget, uint32_t *kind)
{
	if (!ev_has_pending()) {
		return 0;
	}

	const struct event_rec *r = &ring[(start + taken) % EVENT_RING_CAP];
	Entry e = Entry_init_zero;

	e.age_s = age_of(r->t_ms);
	e.which_kind = Entry_motion_tag;
	e.kind.motion.age_s = e.age_s;
	e.kind.motion.reason = r->reason;

	size_t sz;

	if (!pb_get_encoded_size(&sz, Entry_fields, &e) || sz + 2 > budget) {
		return sz + 2 > budget ? 0 : -EINVAL;
	}

	size_t before = stream->bytes_written;

	if (!pb_encode_submessage(stream, Entry_fields, &e)) {
		return -EIO;
	}
	taken++;
	*kind = UPLINK_KIND_EVENT;
	return (int)(stream->bytes_written - before);
}

static void ev_commit(void)
{
	start = (start + taken) % EVENT_RING_CAP;
	count -= taken;
	taken = 0;
}

static void ev_rollback(void)
{
	taken = 0;
}

static const struct uplink_source event_source = {
	.name = "event",
	.has_pending = ev_has_pending,
	.encode_next = ev_encode_next,
	.commit = ev_commit,
	.rollback = ev_rollback,
};

void event_queue_init(void)
{
	start = count = taken = 0;
	uplink_register(&event_source, 0);
}
