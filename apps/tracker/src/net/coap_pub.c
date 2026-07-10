#include "coap_pub.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>

LOG_MODULE_REGISTER(coap_pub, CONFIG_TRACKER_LOG_LEVEL);

/* DEBUG-COMMIT: log the resolved peer IP so we can see what host/port actually
 * got baked into the build (rules out stale-CMake-cache shipping a wrong
 * server). Formats the first getaddrinfo result. */
static void debug_log_peer(const struct zsock_addrinfo *res, uint16_t port)
{
	char ip[INET6_ADDRSTRLEN] = "?";

	if (res && res->ai_addr && res->ai_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;

		zsock_inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
	}
	LOG_INF("DEBUG resolved peer: %s:%u (ai_family %d)",
		ip, port, res ? res->ai_family : -1);
}

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
	LOG_INF("DEBUG coap_pub_init: host='%s' port=%u", host, port);
	err = zsock_getaddrinfo(host, portstr, &hints, &res);
	if (err) {
		/* DEBUG-COMMIT: errno too — getaddrinfo returns its own codes
		 * (EAI_*) but the offloaded resolver may also set errno. */
		LOG_WRN("getaddrinfo(%s): ret %d, errno %d (%s)",
			host, err, errno, strerror(errno));
		return -EHOSTUNREACH;
	}

	debug_log_peer(res, port);

	sock = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock < 0) {
		LOG_ERR("socket: errno %d (%s)", errno, strerror(errno));
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* connect() a UDP socket: fixes the peer so plain send() works and the
	 * RAI option applies to a known flow. */
	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (err < 0) {
		LOG_ERR("connect: errno %d (%s)", errno, strerror(errno));
		coap_pub_close();
		return -errno;
	}

	LOG_INF("CoAP target %s:%u (UDP, NON) — socket %d ready", host, port, sock);
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
		LOG_WRN("send: errno %d (%s)", errno, strerror(errno));
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
