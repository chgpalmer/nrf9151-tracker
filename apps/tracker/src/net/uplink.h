/*
 * uplink: the one place data leaves over LTE.
 *
 * Pull model. Producers (obs store, log ring, later a flash backlog) register
 * as SOURCES; uplink owns the protobuf Obs envelope, the flush policy, and
 * the radio etiquette (drain everything in one wake, RAI on the last
 * datagram). Sources own their storage and their drop policy — uplink never
 * pushes back, producers never block.
 *
 * The pull is transactional: a source's read cursor may only advance in
 * commit(), which uplink calls after coap_pub_send() succeeded for the
 * datagram holding that source's entries. On failure rollback() is called
 * instead and the same entries are offered again on the next flush.
 *
 * Flush reasons: a 2-minute timer, a fullness announcement from a source
 * (uplink_request_flush), or an urgent request (WRN+ log line, cell fix
 * queued, FSM state change). Urgent requests are deferred to the next
 * uplink_poll() — never sent inline — so a log emitted from inside the send
 * path cannot re-enter it. Log-urgent wakes are rate-limited.
 *
 * All of it is gated on uplink_set_allowed(): the FSM's radio verdict.
 */
#ifndef UPLINK_H__
#define UPLINK_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pb_encode.h>

/* Entry kinds present in a sent datagram, reported via the on-sent hook. */
#define UPLINK_KIND_TRACK BIT(0)
#define UPLINK_KIND_CELL  BIT(1)
#define UPLINK_KIND_LOG   BIT(2)

enum uplink_flush_why {
	UPLINK_FLUSH_TIMER,   /* periodic */
	UPLINK_FLUSH_FULL,    /* a source has a full segment/batch ready */
	UPLINK_FLUSH_URGENT,  /* cell queued / FSM state change */
	UPLINK_FLUSH_LOG,     /* WRN+ log line (rate-limited) */
};

struct uplink_source {
	const char *name;
	bool (*has_pending)(void);
	/* Encode ONE Entry (tag + submessage) into the stream if it fits within
	 * `budget` bytes; return bytes written, 0 = nothing pending or nothing
	 * that fits, <0 = encode error. Called repeatedly until 0. Must set
	 * *kind to the UPLINK_KIND_* of what it wrote. */
	int (*encode_next)(pb_ostream_t *stream, size_t budget, uint32_t *kind);
	void (*commit)(void);
	void (*rollback)(void);
};

#define UPLINK_MAX_SOURCES 4

void uplink_init(const char *device_id);

/* prio 0 is drained first. Fixed order: cell(0), live gps(1), logs(2). */
int uplink_register(const struct uplink_source *src, uint8_t prio);

/* The FSM's radio verdict (and socket readiness), refreshed every loop. */
void uplink_set_allowed(bool allowed);

/* Ask for a flush on the next poll. Safe from any context (sets a flag). */
void uplink_request_flush(enum uplink_flush_why why);

/* Run the flush policy; call once per main-loop pass. Returns 0 if idle or
 * everything drained, negative errno if a send failed (data retained). */
int uplink_poll(int64_t now_ms);

/* One event out: a datagram was sent; mask holds the UPLINK_KIND_* bits of
 * what it contained. Serves the FSM's cell-reported transition, the TX LED,
 * and the RRC-tail measurement. */
void uplink_on_sent(void (*cb)(uint32_t kinds_mask));

#endif /* UPLINK_H__ */
