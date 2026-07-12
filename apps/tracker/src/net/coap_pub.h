/*
 * CoAP/UDP publisher: fire-and-forget observation reports.
 *
 * One connected UDP socket, non-confirmable POSTs to /obs with a protobuf
 * payload (see proto/tracker.proto). Non-confirmable matches the old MQTT QoS 0
 * semantics: a lost report is superseded by the next one, and no reply means
 * RAI can release RRC immediately after the send -- this is where UDP beats
 * the old TCP path, whose broker ACK kept the radio up seconds longer.
 */
#ifndef COAP_PUB_H__
#define COAP_PUB_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resolve the server and connect the UDP socket. Call once LTE is up (DNS
 * needs the PDN). Safe to call again after coap_pub_close(). */
int coap_pub_init(const char *host, uint16_t port);

bool coap_pub_ready(void);

/* Send one encoded observation as NON POST /obs. The RAI hint tells the
 * modem whether more datagrams follow in this burst: last=false sets
 * RAI_ONGOING (hold the RRC connection open), last=true sets RAI_LAST
 * (release right after this send). One burst = one RRC session. */
int coap_pub_send(const uint8_t *payload, size_t len, bool last);

/* The one request/RESPONSE exchange (A-GNSS): CON POST `path` with req as
 * payload, then block up to timeout_ms for the matching response (token
 * checked; stale datagrams drained). Confirmable, unlike /obs: RFC 7252
 * retransmission inside the window covers BOTH loss directions — a lost
 * request gets resent, a lost response gets re-provoked (the server dedups
 * by message ID and repeats its answer). Sets RAI_ONE_RESP before each
 * send — "one downlink is coming, then release RRC". Returns the response
 * payload length (copied into resp), or -EAGAIN on timeout / negative
 * errno. */
int coap_pub_exchange(const char *path, const uint8_t *req, size_t req_len,
		      uint8_t *resp, size_t resp_cap, int timeout_ms);

void coap_pub_close(void);

#endif /* COAP_PUB_H__ */
