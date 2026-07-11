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

/* Send one encoded observation as NON POST /obs (application/cbor). Sets
 * RAI_LAST on the socket right before the send so the modem can ask for RRC
 * release as soon as the datagram is out. */
int coap_pub_send(const uint8_t *payload, size_t len);

void coap_pub_close(void);

#endif /* COAP_PUB_H__ */
