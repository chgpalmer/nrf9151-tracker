#include "coap_pub.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>

LOG_MODULE_REGISTER(coap_pub, CONFIG_TRACKER_LOG_LEVEL);

/* Header + token + options + payload. gps_obs encodes to ~50 B; leave slack. */
#define PKT_BUF_LEN 192

static int sock = -1;

int coap_pub_init(const char *host, uint16_t port)
{
	struct zsock_addrinfo *res = NULL;
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	char portstr[8];
	int err;

	if (sock >= 0) {
		return 0;
	}

	snprintf(portstr, sizeof(portstr), "%u", port);
	err = zsock_getaddrinfo(host, portstr, &hints, &res);
	if (err) {
		LOG_WRN("getaddrinfo(%s): %d", host, err);
		return -EHOSTUNREACH;
	}

	sock = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock < 0) {
		LOG_ERR("socket: %d", -errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* connect() a UDP socket: fixes the peer so plain send() works and the
	 * RAI option applies to a known flow. */
	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (err < 0) {
		LOG_ERR("connect: %d", -errno);
		coap_pub_close();
		return -errno;
	}

	LOG_INF("CoAP target %s:%u (UDP, NON)", host, port);
	return 0;
}

bool coap_pub_ready(void)
{
	return sock >= 0;
}

/* Tell the modem this send is the last of the burst so it can ask the network
 * to release RRC immediately (no reply is coming: messages are NON). Compiled
 * out on native_sim, where there is no modem. */
static void rai_last_before_send(void)
{
#if defined(CONFIG_LTE_LC_RAI_MODULE)
	int rai = RAI_LAST;

	if (zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai)) < 0) {
		LOG_DBG("SO_RAI: %d", -errno);
	}
#endif
}

int coap_pub_send(const uint8_t *payload, size_t len)
{
	uint8_t buf[PKT_BUF_LEN];
	struct coap_packet pkt;
	int err;

	if (sock < 0) {
		return -ENOTCONN;
	}

	err = coap_packet_init(&pkt, buf, sizeof(buf), COAP_VERSION_1,
			       COAP_TYPE_NON_CON, COAP_TOKEN_MAX_LEN,
			       coap_next_token(), COAP_METHOD_POST,
			       coap_next_id());
	if (err) {
		return err;
	}

	err = coap_packet_append_option(&pkt, COAP_OPTION_URI_PATH, "obs", 3);
	if (err) {
		return err;
	}
	err = coap_append_option_int(&pkt, COAP_OPTION_CONTENT_FORMAT,
				     COAP_CONTENT_FORMAT_APP_CBOR);
	if (err) {
		return err;
	}
	/* RFC 7967: suppress ALL responses. Without this the server's NON reply
	 * arrives after RAI has released RRC, so the network pages the device and
	 * drags it straight back into connected mode -- undoing the RAI saving on
	 * every single report. */
	err = coap_append_option_int(&pkt, COAP_OPTION_NO_RESPONSE,
				     COAP_NO_RESPONSE_SUPPRESS_ALL);
	if (err) {
		return err;
	}
	err = coap_packet_append_payload_marker(&pkt);
	if (err) {
		return err;
	}
	err = coap_packet_append_payload(&pkt, payload, len);
	if (err) {
		return err;
	}

	rai_last_before_send();
	if (zsock_send(sock, pkt.data, pkt.offset, 0) < 0) {
		LOG_WRN("send: %d", -errno);
		return -errno;
	}

	LOG_INF("CoAP → /obs (%u B, %zu B payload)", pkt.offset, len);
	return 0;
}

void coap_pub_close(void)
{
	if (sock >= 0) {
		zsock_close(sock);
		sock = -1;
	}
}
