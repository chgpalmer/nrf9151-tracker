/*
 * log_uplink: a Zephyr log backend that ships log lines over LTE.
 *
 * The logging core hands every enabled message to process(); lines at or
 * above the uplink threshold (Kconfig, default INF) land in a small
 * drop-oldest RAM ring. The module doubles as an uplink source (priority
 * behind cell + gps): at flush time it encodes queued lines as one LogBatch
 * Entry, exact-sized to the datagram budget, cursor advancing only on
 * confirmed send. A WRN+ arrival requests an urgent (rate-limited) flush.
 *
 * Re-entrancy: this backend feeds uplink, and uplink logs. That loop is cut
 * twice — uplink logs at DBG (below the default threshold), and
 * uplink_request_flush() only sets a flag; nothing here ever calls into the
 * send path.
 */
#include "uplink.h"
#include "tracker.pb.h"

#include <pb_encode.h>

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>

#define RING_CAP CONFIG_TRACKER_LOG_UPLINK_RING_LINES
#define TEXT_MAX 96
#define MOD_MAX  16

struct line {
	int64_t t_ms;
	uint8_t level;
	char mod[MOD_MAX];
	char text[TEXT_MAX];
};

static struct line ring[RING_CAP];
static size_t r_start, r_count, r_taken;
static uint32_t r_dropped; /* lines lost to overflow; reported in-band */

/* ---- backend side: log core -> ring -------------------------------------- */

/* Line assembly buffer: log_output emits formatted bytes in chunks. */
static char asm_buf[TEXT_MAX];
static size_t asm_len;
static uint8_t cur_level;
static const char *cur_mod;

static void push_line(void)
{
	size_t skip = 0;

	if (asm_len == 0) {
		return;
	}

	/* The text formatter prefixes "module: " even with the level flag off;
	 * the module already travels as its own field, so strip the duplicate. */
	if (cur_mod) {
		size_t ml = strlen(cur_mod);

		if (asm_len > ml + 2 && memcmp(asm_buf, cur_mod, ml) == 0 &&
		    asm_buf[ml] == ':' && asm_buf[ml + 1] == ' ') {
			skip = ml + 2;
		}
	}
	if (r_count == RING_CAP) {
		r_start = (r_start + 1) % RING_CAP;
		r_count--;
		if (r_taken > 0) {
			r_taken--;
		}
		r_dropped++;
	}

	struct line *l = &ring[(r_start + r_count) % RING_CAP];

	l->t_ms = k_uptime_get();
	l->level = cur_level;
	strncpy(l->mod, cur_mod ? cur_mod : "?", MOD_MAX - 1);
	l->mod[MOD_MAX - 1] = '\0';
	memcpy(l->text, asm_buf + skip, asm_len - skip);
	l->text[asm_len - skip] = '\0';
	r_count++;
	asm_len = 0;
}

static int out_func(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < length; i++) {
		char c = (char)data[i];

		if (c == '\n' || c == '\r') {
			push_line();
		} else if (asm_len < TEXT_MAX - 1) {
			asm_buf[asm_len++] = c;
		}
	}
	return (int)length;
}

static uint8_t out_buf[64];
LOG_OUTPUT_DEFINE(uplink_log_output, out_func, out_buf, sizeof(out_buf));

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	uint8_t level = log_msg_get_level(&msg->log);

	/* 0 = raw printk-ish; treat as INF. Higher number = chattier. */
	if (level > CONFIG_TRACKER_LOG_UPLINK_LEVEL && level != 0) {
		return;
	}

	cur_level = level ? level : LOG_LEVEL_INF;
	int16_t sid = log_msg_get_source_id(&msg->log);

	cur_mod = (sid >= 0) ? log_source_name_get(0, sid) : NULL;

	/* Body only: no colors, no timestamp (ages travel per line), no
	 * level/module prefix (they go as fields). */
	log_output_msg_process(&uplink_log_output, &msg->log,
			       LOG_OUTPUT_FLAG_CRLF_NONE);
	push_line(); /* in case the message didn't end in a newline */

	if (cur_level <= LOG_LEVEL_WRN) {
		uplink_request_flush(UPLINK_FLUSH_LOG);
	}
}

static void panic(const struct log_backend *const backend)
{
	log_backend_deactivate(backend);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	r_dropped += cnt;
}

static const struct log_backend_api log_uplink_api = {
	.process = process,
	.panic = panic,
	.dropped = dropped,
};

LOG_BACKEND_DEFINE(log_backend_uplink, log_uplink_api, true);

/* ---- source side: ring -> uplink ------------------------------------------ */

static bool str_cb(pb_ostream_t *stream, const pb_field_t *field,
		   void *const *arg)
{
	const char *s = *arg;

	return pb_encode_tag(stream, PB_WT_STRING, field->tag) &&
	       pb_encode_string(stream, (const pb_byte_t *)s, strlen(s));
}

/* How many lines the in-progress LogBatch encodes: [r_start+r_taken, +batch_n) */
static size_t batch_n;

static bool encode_lines(pb_ostream_t *stream, const pb_field_t *field,
			 void *const *arg)
{
	ARG_UNUSED(arg);
	int64_t now = k_uptime_get();

	for (size_t i = 0; i < batch_n; i++) {
		struct line *l = &ring[(r_start + r_taken + i) % RING_CAP];
		LogLine ll = LogLine_init_zero;

		ll.age_s = (uint32_t)CLAMP((now - l->t_ms) / 1000, 0, 7200);
		ll.level = l->level;
		ll.module.funcs.encode = str_cb;
		ll.module.arg = l->mod;
		ll.text.funcs.encode = str_cb;
		ll.text.arg = l->text;

		if (!pb_encode_tag(stream, PB_WT_STRING, field->tag) ||
		    !pb_encode_submessage(stream, LogLine_fields, &ll)) {
			return false;
		}
	}
	return true;
}

static bool log_has_pending(void)
{
	return r_count > r_taken || r_dropped > 0;
}

static int log_encode_next(pb_ostream_t *stream, size_t budget, uint32_t *kind)
{
	/* Report overflow in-band before anything else: it is itself a log
	 * line, synthesized here so it can never be dropped. */
	if (r_dropped > 0 && r_count < RING_CAP) {
		struct line *l = &ring[(r_start + r_count) % RING_CAP];

		l->t_ms = k_uptime_get();
		l->level = LOG_LEVEL_WRN;
		strncpy(l->mod, "log_uplink", MOD_MAX - 1);
		snprintf(l->text, TEXT_MAX, "dropped %u lines", r_dropped);
		r_count++;
		r_dropped = 0;
	}
	if (r_count <= r_taken) {
		return 0;
	}

	Entry e;
	size_t n = MIN(r_count - r_taken, 16);

	for (;;) {
		batch_n = n;
		e = (Entry)Entry_init_zero;
		e.age_s = 0; /* lines carry their own ages */
		e.which_kind = Entry_log_tag;
		e.kind.log.lines.funcs.encode = encode_lines;

		size_t sz;

		if (!pb_get_encoded_size(&sz, Entry_fields, &e)) {
			return -EINVAL;
		}
		if (sz + 2 <= budget) {
			break;
		}
		if (n <= 1) {
			return 0; /* not even one line fits this datagram */
		}
		n /= 2;
	}

	size_t before = stream->bytes_written;

	if (!pb_encode_submessage(stream, Entry_fields, &e)) {
		return -EIO;
	}
	r_taken += n;
	*kind = UPLINK_KIND_LOG;
	return (int)(stream->bytes_written - before);
}

static void log_commit(void)
{
	r_start = (r_start + r_taken) % RING_CAP;
	r_count -= r_taken;
	r_taken = 0;
}

static void log_rollback(void)
{
	r_taken = 0;
}

static const struct uplink_source log_source = {
	.name = "log",
	.has_pending = log_has_pending,
	.encode_next = log_encode_next,
	.commit = log_commit,
	.rollback = log_rollback,
};

void log_uplink_init(void)
{
	uplink_register(&log_source, 2);
}
