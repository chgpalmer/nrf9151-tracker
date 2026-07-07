/*
 * nRF9151 GPS tracker — active mode
 *
 * Loop:
 *   - sample GNSS every second (1 Hz PVT events)
 *   - POST latest fix to HTTP server every 10 seconds
 *
 * LTE is brought up once at boot and kept connected. GNSS runs
 * concurrently via the modem's LTE-M+GPS timeslotting.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(tracker, CONFIG_TRACKER_LOG_LEVEL);

/* ── configuration ──────────────────────────────────────────────── */
#define POST_SERVER_HOST  CONFIG_TRACKER_SERVER_HOST
#define POST_SERVER_PORT  CONFIG_TRACKER_SERVER_PORT
#define POST_PATH         CONFIG_TRACKER_SERVER_PATH

#define GNSS_INTERVAL_S   1   /* PVT event rate */
#define POST_INTERVAL_MS  (CONFIG_TRACKER_POST_INTERVAL_S * 1000)
#define STATUS_INTERVAL_MS 5000

/* ── state ───────────────────────────────────────────────────────── */
static struct nrf_modem_gnss_pvt_data_frame pvt;
static K_SEM_DEFINE(pvt_sem, 0, 1);

static enum lte_lc_nw_reg_status lte_status = LTE_LC_NW_REG_NOT_REGISTERED;
static bool lte_connected;


/* ── GNSS ─────────────────────────────────────────────────────────  */
static void gnss_handler(int event)
{
	if (event == NRF_MODEM_GNSS_EVT_PVT) {
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			k_sem_give(&pvt_sem);
		}
	}
}

static int gnss_start(void)
{
	int err;

	err = nrf_modem_gnss_event_handler_set(gnss_handler);
	if (err) {
		LOG_ERR("gnss event handler: %d", err);
		return err;
	}

	uint16_t nmea_mask = 0; /* no NMEA — we use PVT only */
	nrf_modem_gnss_nmea_mask_set(nmea_mask);

	nrf_modem_gnss_fix_interval_set(GNSS_INTERVAL_S);
	nrf_modem_gnss_fix_retry_set(0); /* run continuously */

	uint8_t use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;
	nrf_modem_gnss_use_case_set(use_case);

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("gnss start: %d", err);
	}
	return err;
}

/* ── LTE ──────────────────────────────────────────────────────────  */
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		lte_status = evt->nw_reg_status;
		if (lte_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    lte_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_INF("LTE registered (%s)",
				lte_status == LTE_LC_NW_REG_REGISTERED_ROAMING
					? "roaming" : "home");
			lte_connected = true;
		} else {
			lte_connected = false;
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_DBG("PSM: TAU %d s, active %d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	default:
		break;
	}
}

static const char *lte_status_str(void)
{
	switch (lte_status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:       return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:      return "registered (home)";
	case LTE_LC_NW_REG_SEARCHING:            return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:  return "denied";
	case LTE_LC_NW_REG_UNKNOWN:              return "no coverage";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:   return "registered (roaming)";
	default:                                  return "?";
	}
}

/* ── HTTP POST ────────────────────────────────────────────────────  */
static uint8_t http_recv_buf[512];
static char    http_payload[256];
static char    http_host[64];

static int http_response_cb(struct http_response *rsp,
			    enum http_final_call final_data,
			    void *user_data)
{
	if (final_data == HTTP_DATA_FINAL) {
		LOG_INF("HTTP %d %s", rsp->http_status_code,
			rsp->http_status);
	}
	return 0;
}

/* Pass NULL for p to send a dummy payload (LTE data-path test only). */
static int http_post_fix(const struct nrf_modem_gnss_pvt_data_frame *p)
{
	struct sockaddr_in server = {0};
	int sock, err;

	if (p == NULL) {
		snprintf(http_payload, sizeof(http_payload),
			 "{\"dummy\":true,\"msg\":\"lte-datapath-test\"}");
	} else {
		uint8_t used = 0;

		for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
			if (p->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				used++;
			}
		}

		snprintf(http_payload, sizeof(http_payload),
			 "{\"lat\":%.6f,\"lon\":%.6f,"
			 "\"alt\":%.1f,\"acc\":%.1f,"
			 "\"sats_used\":%u,"
			 "\"ts\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\"}",
			 p->latitude, p->longitude,
			 (double)p->altitude, (double)p->accuracy,
			 used,
			 p->datetime.year, p->datetime.month, p->datetime.day,
			 p->datetime.hour, p->datetime.minute, p->datetime.seconds);
	}

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket: %d", errno);
		return -errno;
	}

	server.sin_family = AF_INET;
	server.sin_port = htons(POST_SERVER_PORT);
	inet_pton(AF_INET, POST_SERVER_HOST, &server.sin_addr);

	err = connect(sock, (struct sockaddr *)&server, sizeof(server));
	if (err) {
		LOG_ERR("connect to %s:%d failed: %d", POST_SERVER_HOST,
			POST_SERVER_PORT, errno);
		close(sock);
		return -errno;
	}

	snprintf(http_host, sizeof(http_host), "%s:%d",
		 POST_SERVER_HOST, POST_SERVER_PORT);

	struct http_request req = {
		.method             = HTTP_POST,
		.url                = POST_PATH,
		.host               = http_host,
		.protocol           = "HTTP/1.1",
		.payload            = http_payload,
		.payload_len        = strlen(http_payload),
		.content_type_value = "application/json",
		.response           = http_response_cb,
		.recv_buf           = http_recv_buf,
		.recv_buf_len       = sizeof(http_recv_buf),
	};

	err = http_client_req(sock, &req, 5000, NULL);
	close(sock);

	if (err < 0) {
		LOG_ERR("http_client_req: %d", err);
		return err;
	}

	LOG_INF("POST %s:%d%s -> %d bytes",
		POST_SERVER_HOST, POST_SERVER_PORT, POST_PATH, err);
	return 0;
}

/* ── status block ─────────────────────────────────────────────────  */
static void print_status(const struct nrf_modem_gnss_pvt_data_frame *p,
			 int64_t uptime_ms)
{
	char at_resp[256];
	uint8_t tracked = 0, used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (p->sv[i].sv > 0) {
			tracked++;
			if (p->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				used++;
			}
		}
	}

	LOG_INF("===== status @ %llds =====", (long long)(uptime_ms / 1000));

	/* LTE status: combine lte_lc registration state with AT%XMONITOR
	 * radio details (operator, band, cell, RSRP, SNR).
	 * XMONITOR response (space after colon, fields comma-separated):
	 *   %XMONITOR: <reg>,"<long>","<short>","<plmn>","<tac>",<AcT>,
	 *              <band>,"<cell_id>",<phys_cell_id>,<EARFCN>,<rsrp>,<snr>,...
	 */
	if (nrf_modem_at_cmd(at_resp, sizeof(at_resp), "AT%%XMONITOR") == 0) {
		int  band = 0, rsrp = 255, snr = 127;
		char op[32] = "", cell[16] = "";

		/* Skip prefix, then parse field by field using strtok on a copy. */
		char buf[256];
		strncpy(buf, at_resp, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';

		char *p = strchr(buf, ':');
		if (p) {
			p++; /* skip ':' */
			/* field 1: reg (skip) */
			strtok(p, ",");
			/* field 2: long name (quoted) */
			char *f = strtok(NULL, ",");
			if (f && f[0] == '"') {
				/* strip quotes */
				f++;
				char *e = strchr(f, '"');
				if (e) {
					*e = '\0';
				}
				strncpy(op, f, sizeof(op) - 1);
			}
			/* fields 3,4,5: short name, plmn, tac */
			strtok(NULL, ",");
			strtok(NULL, ",");
			strtok(NULL, ",");
			/* field 6: AcT (skip) */
			strtok(NULL, ",");
			/* field 7: band */
			f = strtok(NULL, ",");
			if (f) {
				band = atoi(f);
			}
			/* field 8: cell id (quoted) */
			f = strtok(NULL, ",");
			if (f && f[0] == '"') {
				f++;
				char *e = strchr(f, '"');
				if (e) {
					*e = '\0';
				}
				strncpy(cell, f, sizeof(cell) - 1);
			}
			/* fields 9,10: phys cell, EARFCN */
			strtok(NULL, ",");
			strtok(NULL, ",");
			/* field 11: rsrp */
			f = strtok(NULL, ",");
			if (f) {
				rsrp = atoi(f);
			}
			/* field 12: snr */
			f = strtok(NULL, ",\r\n");
			if (f) {
				snr = atoi(f);
			}
		}

		if (band) {
			LOG_INF("LTE:  %s | op %s | band %d | cell %s | RSRP %d dBm | SNR %d dB",
				lte_status_str(), op, band, cell,
				rsrp - 140, snr - 24);
		} else {
			LOG_INF("LTE:  %s%s%s",
				lte_status_str(),
				op[0] ? " | op " : "", op);
		}
	} else {
		LOG_INF("LTE:  %s", lte_status_str());
	}

	/* GNSS status */
	if (p->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		LOG_INF("GNSS: FIX  %.6f, %.6f  alt %.1fm  acc %.1fm  sats %u/%u",
			p->latitude, p->longitude,
			(double)p->altitude, (double)p->accuracy,
			used, tracked);
	} else {
		LOG_INF("GNSS: searching  tracked %u  used %u", tracked, used);
		if (p->flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
			LOG_WRN("GNSS: blocked by LTE");
		}
	}
}

/* ── main ─────────────────────────────────────────────────────────  */
int main(void)
{
	int err;

	LOG_INF("tracker starting");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("modem init: %d", err);
		return err;
	}

	/* Start LTE async — don't block, status loop shows progress */
	LOG_INF("starting LTE (async)...");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async: %d", err);
		return err;
	}

	/* Start GNSS immediately — timeslots with LTE automatically */
	LOG_INF("starting GNSS...");
	err = gnss_start();
	if (err) {
		LOG_ERR("gnss start: %d", err);
		return err;
	}

	int64_t last_post_ms   = 0;
	int64_t last_status_ms = 0;
	bool    has_fix        = false;

	for (;;) {
		/* Wait for next PVT event (1 Hz) with a 2s timeout so the
		 * status block still prints even if GNSS is blocked by LTE. */
		if (k_sem_take(&pvt_sem, K_SECONDS(2)) == 0) {
			has_fix = (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID);
		}

		int64_t now = k_uptime_get();

		/* Status block every STATUS_INTERVAL_MS */
		if (now - last_status_ms >= STATUS_INTERVAL_MS) {
			last_status_ms = now;
			print_status(&pvt, now);
		}

		/* POST every POST_INTERVAL_MS as soon as LTE is up.
		 * Send real fix payload when available, dummy when not --
		 * this tests the data path independently of GNSS. */
		if (now - last_post_ms >= POST_INTERVAL_MS && lte_connected) {
			last_post_ms = now;
			LOG_INF("sending %s payload to %s:%d%s",
				has_fix ? "fix" : "dummy",
				POST_SERVER_HOST, POST_SERVER_PORT, POST_PATH);
			err = http_post_fix(has_fix ? &pvt : NULL);
			if (err) {
				LOG_WRN("post failed: %d (will retry)", err);
			}
		} else if (!lte_connected) {
			LOG_DBG("post waiting for LTE");
			last_post_ms = now - POST_INTERVAL_MS + 3000;
		}
	}

	return 0;
}
