#include "coap_pub.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>

LOG_MODULE_REGISTER(coap_pub, CONFIG_TRACKER_LOG_LEVEL);

/* Header + token + options + payload. A full 24-entry batch is ~850 B. */
#define PKT_BUF_LEN 1152 /* batches: 24 entries + envelope + CoAP framing */

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
		/* getaddrinfo returns its own codes (EAI_*), but the offloaded
		 * resolver may also set errno — log both. */
		LOG_WRN("getaddrinfo(%s): ret %d, errno %d (%s)",
			host, err, errno, strerror(errno));
		return -EHOSTUNREACH;
	}

	/* Explicit type/protocol, NOT res->ai_socktype/ai_protocol: the modem's
	 * offloaded getaddrinfo ignores the hints' protocol side and returns
	 * SOCK_DGRAM paired with IPPROTO_TCP (measured on mfw + NCS 3.4), which
	 * nrf_socket() rightly rejects with EPROTOTYPE. The canonical Nordic
	 * samples all hardcode (family, SOCK_DGRAM, IPPROTO_UDP). */
	sock = zsock_socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
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

/* RAI hint per send: LAST releases RRC right after the datagram (no reply is
 * coming: messages are NON); ONGOING holds the connection for the rest of the
 * burst — telling the network "done" three times per flush invited an RRC
 * release mid-burst. Compiled out on native_sim, where there is no modem. */
static void rai_hint_before_send(bool last)
{
#if defined(CONFIG_LTE_LC_RAI_MODULE)
	int rai = last ? RAI_LAST : RAI_ONGOING;

	if (zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai)) < 0) {
		LOG_DBG("SO_RAI: %d", -errno);
	}
#else
	(void)last;
#endif
}

int coap_pub_send(const uint8_t *payload, size_t len, bool last)
{
	static uint8_t buf[PKT_BUF_LEN]; /* 1 KB+: keep off the 4 KB main stack */
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
				     COAP_CONTENT_FORMAT_APP_OCTET_STREAM);
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

	rai_hint_before_send(last);
	if (zsock_send(sock, pkt.data, pkt.offset, 0) < 0) {
		LOG_WRN("send: errno %d (%s)", errno, strerror(errno));
		return -errno;
	}

	LOG_DBG("CoAP → /obs (%u B, %zu B payload)", pkt.offset, len);
	return 0;
}

/* This send expects exactly one downlink: tell the modem so it can release
 * RRC right after that response instead of idling in connected mode. */
static void rai_one_resp_before_send(void)
{
#if defined(CONFIG_LTE_LC_RAI_MODULE)
	int rai = RAI_ONE_RESP;

	if (zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &rai, sizeof(rai)) < 0) {
		LOG_DBG("SO_RAI(one_resp): %d", -errno);
	}
#endif
}

int coap_pub_exchange(const char *path, const uint8_t *req, size_t req_len,
		      uint8_t *resp, size_t resp_cap, int timeout_ms)
{
	static uint8_t buf[PKT_BUF_LEN]; /* shared tx/rx staging, off-stack */
	struct coap_packet pkt;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	int err;

	if (sock < 0) {
		return -ENOTCONN;
	}

	memcpy(token, coap_next_token(), sizeof(token));
	err = coap_packet_init(&pkt, buf, sizeof(buf), COAP_VERSION_1,
			       COAP_TYPE_NON_CON, sizeof(token), token,
			       COAP_METHOD_POST, coap_next_id());
	if (err) {
		return err;
	}
	err = coap_packet_append_option(&pkt, COAP_OPTION_URI_PATH,
					path, strlen(path));
	if (err) {
		return err;
	}
	err = coap_append_option_int(&pkt, COAP_OPTION_CONTENT_FORMAT,
				     COAP_CONTENT_FORMAT_APP_OCTET_STREAM);
	if (err) {
		return err;
	}
	err = coap_packet_append_payload_marker(&pkt);
	if (err) {
		return err;
	}
	err = coap_packet_append_payload(&pkt, req, req_len);
	if (err) {
		return err;
	}

	/* Drain anything stale (a late response from a previous exchange). */
	while (zsock_recv(sock, buf, sizeof(buf), ZSOCK_MSG_DONTWAIT) > 0) {
	}

	rai_one_resp_before_send();
	if (zsock_send(sock, pkt.data, pkt.offset, 0) < 0) {
		LOG_WRN("exchange send: errno %d (%s)", errno, strerror(errno));
		return -errno;
	}
	LOG_DBG("CoAP → /%s (%zu B payload), awaiting response", path, req_len);

	int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return -EAGAIN;
		}
		struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };

		err = zsock_poll(&pfd, 1, (int)left);
		if (err < 0) {
			return -errno;
		}
		if (err == 0) {
			return -EAGAIN;
		}
		int n = zsock_recv(sock, buf, sizeof(buf), 0);

		if (n < 0) {
			return -errno;
		}

		struct coap_packet rsp;

		if (coap_packet_parse(&rsp, buf, n, NULL, 0) < 0) {
			continue; /* not CoAP; keep waiting */
		}
		uint8_t rtok[COAP_TOKEN_MAX_LEN];
		uint8_t tkl = coap_header_get_token(&rsp, rtok);

		if (tkl != sizeof(token) || memcmp(rtok, token, tkl) != 0) {
			continue; /* someone else's response */
		}
		if (coap_header_get_code(&rsp) != COAP_RESPONSE_CODE_CONTENT) {
			LOG_WRN("exchange: response code 0x%02x",
				coap_header_get_code(&rsp));
			return -EBADMSG;
		}
		uint16_t plen;
		const uint8_t *payload = coap_packet_get_payload(&rsp, &plen);

		if (payload == NULL || plen == 0) {
			return -EBADMSG;
		}
		if (plen > resp_cap) {
			return -EMSGSIZE;
		}
		memcpy(resp, payload, plen);
		return plen;
	}
}

void coap_pub_close(void)
{
	if (sock >= 0) {
		zsock_close(sock);
		sock = -1;
	}
}
