/*
 * nRF9151 GPS tracker — active mode
 *
 * Loop:
 *   - sample GNSS every second (1 Hz PVT events)
 *   - MQTT publish latest fix every 10 seconds
 *
 * LTE is brought up once at boot and kept connected. GNSS runs
 * concurrently via the modem's LTE-M+GPS timeslotting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <net/mqtt_helper.h>
#include <hw_id.h>

#include "loc_fsm.h"

LOG_MODULE_REGISTER(tracker, CONFIG_TRACKER_LOG_LEVEL);

/* ── configuration ──────────────────────────────────────────────── */
#define BROKER_HOST       CONFIG_TRACKER_MQTT_BROKER_HOST
#define BROKER_PORT       CONFIG_TRACKER_MQTT_BROKER_PORT
#define TOPIC_PREFIX      CONFIG_TRACKER_MQTT_TOPIC_PREFIX

#define GNSS_INTERVAL_S   1   /* PVT event rate */
#define POST_INTERVAL_MS  (CONFIG_TRACKER_POST_INTERVAL_S * 1000)
#define STATUS_INTERVAL_MS 5000
/* GPS staleness now lives in loc_fsm, which owns the fix bookkeeping. */
#define MQTT_RETRY_MS     10000  /* between reconnect attempts */

/* RSRP index → dBm (same formula as modem_info.h RSRP_IDX_TO_DBM). */
#define RSRP_IDX_TO_DBM(idx) ((idx) < 0 ? (idx) - 140 : (idx) - 141)

/* Serving-cell identity, parsed from AT%XMONITOR (cached registration data —
 * no radio scan). Enough for an OpenCelliD single-cell lookup. */
struct serving_cell {
	int      mcc;
	int      mnc;
	uint32_t tac;
	uint32_t cid;
	int      rsrp_dbm;
	int      band;
	char     op[32];
	bool     valid;
};

/* ── state ───────────────────────────────────────────────────────── */
static struct nrf_modem_gnss_pvt_data_frame pvt;
static K_SEM_DEFINE(pvt_sem, 0, 1);

static enum lte_lc_nw_reg_status lte_status = LTE_LC_NW_REG_NOT_REGISTERED;
/* Both are set from callback threads (LTE handler / mqtt_helper) and polled by
 * the main loop, which reconnects off mqtt_connected. */
static volatile bool lte_connected;
static volatile bool mqtt_connected;

/* GNSS only runs when LTE leaves the radio alone, so the two facts that explain
 * a starved GNSS are whether we are stuck in RRC connected and whether the
 * network actually granted PSM. Both are set from the LTE callback thread. */
static volatile bool rrc_connected;
static volatile int  psm_tau_s    = -1;
static volatile int  psm_active_s = -1; /* -1 = PSM not granted */

static char device_id[HW_ID_LEN];
static char pos_topic[64];
static char cell_topic[64];

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

/* Set up GNSS without starting it: loc_fsm decides when it may run. */
static int gnss_configure(void)
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

	return 0;
}

static int gnss_start(void)
{
	int err = nrf_modem_gnss_start();

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
		/* active_time == -1 means the network refused PSM. Roaming networks
		 * often do. Without PSM the modem keeps monitoring pagings and GNSS
		 * never gets a long enough window for a cold start. Log at INF: this
		 * was LOG_DBG and therefore invisible at CONFIG_TRACKER_LOG_LEVEL=3. */
		psm_tau_s    = evt->psm_cfg.tau;
		psm_active_s = evt->psm_cfg.active_time;
		if (psm_active_s < 0) {
			LOG_WRN("PSM: NOT granted by network (TAU %d)", psm_tau_s);
		} else {
			LOG_INF("PSM: granted — TAU %d s, active %d s",
				psm_tau_s, psm_active_s);
		}
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		rrc_connected = (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED);
		LOG_INF("RRC: %s", rrc_connected ? "connected (GNSS blocked)" : "idle");
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

/* ── MQTT ─────────────────────────────────────────────────────────  */
static char mqtt_payload[256];

static void on_connack(enum mqtt_conn_return_code rc, bool session_present)
{
	if (rc == MQTT_CONNECTION_ACCEPTED) {
		LOG_INF("MQTT connected to %s:%d", BROKER_HOST, BROKER_PORT);
		mqtt_connected = true;
	} else {
		LOG_WRN("MQTT connack refused: %d", rc);
	}
}

static void on_disconnect(int result)
{
	LOG_INF("MQTT disconnected: %d", result);
	mqtt_connected = false;
}

static int tracker_mqtt_connect(void)
{
	static char broker_host[] = BROKER_HOST;

	struct mqtt_helper_cfg cfg = {
		.cb.on_connack    = on_connack,
		.cb.on_disconnect = on_disconnect,
	};

	int err = mqtt_helper_init(&cfg);
	if (err) {
		LOG_ERR("mqtt_helper_init: %d", err);
		return err;
	}

	struct mqtt_helper_conn_params params = {
		.hostname  = { .ptr = broker_host, .size = strlen(broker_host) },
		.device_id = { .ptr = device_id,   .size = strlen(device_id) },
	};

	err = mqtt_helper_connect(&params);
	if (err) {
		LOG_ERR("mqtt_helper_connect: %d", err);
	}
	return err;
}

static int mqtt_publish_fix(const struct nrf_modem_gnss_pvt_data_frame *p)
{
	uint8_t used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (p->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			used++;
		}
	}

	bool vel = (p->flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID);

	snprintf(mqtt_payload, sizeof(mqtt_payload),
		 "{\"lat\":%.6f,\"lon\":%.6f,"
		 "\"alt\":%.1f,\"acc\":%.1f,"
		 "\"spd\":%.1f,\"hdg\":%.1f,\"sats\":%u}",
		 p->latitude, p->longitude,
		 (double)p->altitude, (double)p->accuracy,
		 vel ? (double)p->speed : 0.0,
		 vel ? (double)p->heading : 0.0,
		 used);

	struct mqtt_publish_param msg = {
		.message.topic.qos            = MQTT_QOS_0_AT_MOST_ONCE,
		.message.topic.topic.utf8     = (uint8_t *)pos_topic,
		.message.topic.topic.size     = strlen(pos_topic),
		.message.payload.data         = (uint8_t *)mqtt_payload,
		.message.payload.len          = strlen(mqtt_payload),
		.message_id                   = mqtt_helper_msg_id_get(),
		.dup_flag                     = 0,
		.retain_flag                  = 0,
	};

	int err = mqtt_helper_publish(&msg);
	if (err) {
		LOG_ERR("mqtt_helper_publish: %d", err);
		return err;
	}

	LOG_INF("MQTT → %s (%zu bytes)", pos_topic, strlen(mqtt_payload));
	return 0;
}

/* ── cell fallback ────────────────────────────────────────────────  */
static int publish_json(const char *topic)
{
	struct mqtt_publish_param msg = {
		.message.topic.qos        = MQTT_QOS_0_AT_MOST_ONCE,
		.message.topic.topic.utf8 = (uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.payload.data     = (uint8_t *)mqtt_payload,
		.message.payload.len      = strlen(mqtt_payload),
		.message_id               = mqtt_helper_msg_id_get(),
		.dup_flag                 = 0,
		.retain_flag              = 0,
	};

	int err = mqtt_helper_publish(&msg);
	if (err) {
		LOG_ERR("mqtt_helper_publish: %d", err);
		return err;
	}
	LOG_INF("MQTT → %s (%zu bytes)", topic, strlen(mqtt_payload));
	return 0;
}

/* Helper: extract the next comma-delimited XMONITOR field, stripping the
 * surrounding quotes if present. Returns the token or NULL. Uses strtok state. */
static char *xmon_field(char *first)
{
	char *f = strtok(first, ",");

	if (f && f[0] == '"') {
		f++;
		char *e = strchr(f, '"');

		if (e) {
			*e = '\0';
		}
	}
	return f;
}

/* Read the serving cell from AT%XMONITOR (cached — no radio scan). Fills sc.
 * XMONITOR: <reg>,"<long>","<short>","<plmn>","<tac>",<AcT>,<band>,
 *           "<cell_id>",<phys_cell_id>,<EARFCN>,<rsrp>,<snr>,...
 * PLMN is "MCCMNC" (MCC=first 3 digits); TAC and cell-id are hex strings. */
static int read_serving_cell(struct serving_cell *sc)
{
	char at_resp[256];

	memset(sc, 0, sizeof(*sc));

	if (nrf_modem_at_cmd(at_resp, sizeof(at_resp), "AT%%XMONITOR") != 0) {
		return -EIO;
	}

	char *cp = strchr(at_resp, ':');

	if (!cp) {
		return -EINVAL;
	}
	cp++;

	xmon_field(cp);                    /* 1: reg status */
	char *longn = xmon_field(NULL);    /* 2: long name  */
	xmon_field(NULL);                  /* 3: short name */
	char *plmn = xmon_field(NULL);     /* 4: plmn "MCCMNC" */
	char *tac  = xmon_field(NULL);     /* 5: tac (hex)  */
	xmon_field(NULL);                  /* 6: AcT        */
	char *band = xmon_field(NULL);     /* 7: band       */
	char *cid  = xmon_field(NULL);     /* 8: cell id (hex) */
	xmon_field(NULL);                  /* 9: phys cell  */
	xmon_field(NULL);                  /* 10: EARFCN    */
	char *rsrp = strtok(NULL, ",\r\n");/* 11: rsrp idx  */

	if (!plmn || strlen(plmn) < 5 || !tac || !cid) {
		return -ENODATA;
	}

	/* Split PLMN: MCC = first 3 chars, MNC = the rest (2 or 3 digits). */
	char mcc_buf[4] = { plmn[0], plmn[1], plmn[2], '\0' };

	sc->mcc = atoi(mcc_buf);
	sc->mnc = atoi(plmn + 3);
	sc->tac = strtoul(tac, NULL, 16);
	sc->cid = strtoul(cid, NULL, 16);
	sc->band = band ? atoi(band) : 0;
	sc->rsrp_dbm = rsrp ? RSRP_IDX_TO_DBM(atoi(rsrp)) : 0;
	if (longn) {
		strncpy(sc->op, longn, sizeof(sc->op) - 1);
	}
	sc->valid = true;
	return 0;
}

static int mqtt_publish_cell(const struct serving_cell *sc)
{
	if (!sc->valid) {
		LOG_WRN("no serving cell — skipping cell publish");
		return -ENODATA;
	}

	snprintf(mqtt_payload, sizeof(mqtt_payload),
		"{\"mcc\":%d,\"mnc\":%d,\"tac\":%u,\"cid\":%u,\"rsrp\":%d}",
		sc->mcc, sc->mnc, sc->tac, sc->cid, sc->rsrp_dbm);

	return publish_json(cell_topic);
}

/* ── status block ─────────────────────────────────────────────────  */
static void print_status(const struct nrf_modem_gnss_pvt_data_frame *p,
			 const struct loc_status *loc, int64_t uptime_ms)
{
	LOG_INF("===== status @ %llds =====", (long long)(uptime_ms / 1000));

	/* LTE status: registration state + serving-cell details (same cached
	 * AT%XMONITOR read used for the cell-location fallback). */
	struct serving_cell sc;

	if (read_serving_cell(&sc) == 0 && sc.valid) {
		LOG_INF("LTE:  %s | op %s | band %d | cell %u | RSRP %d dBm",
			lte_status_str(), sc.op, sc.band, sc.cid, sc.rsrp_dbm);
	} else {
		LOG_INF("LTE:  %s", lte_status_str());
	}

	/* GNSS status */
	if (p->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		LOG_INF("GNSS: FIX  %.6f, %.6f  alt %.1fm  acc %.1fm  sats %u/%u",
			p->latitude, p->longitude,
			(double)p->altitude, (double)p->accuracy,
			loc->used, loc->tracked);
	} else {
		LOG_INF("GNSS: searching  flags 0x%02x", p->flags);
		/* These two mean different things and want different fixes:
		 *   DEADLINE_MISSED        no window at all since the last PVT — LTE
		 *                          is transmitting; only backing off LTE helps.
		 *   NOT_ENOUGH_WINDOW_TIME windows exist but are too short — the case
		 *                          nrf_modem_gnss_prio_mode_enable() is for.
		 * The old code tested only the first, so the two looked identical. */
		if (p->flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
			LOG_WRN("GNSS: no window since last PVT (LTE active)");
		}
		if (p->flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
			LOG_WRN("GNSS: windows too short (no PSM/eDRX?)");
		}
	}
	loc_fsm_log(loc, p);

	if (psm_active_s < 0) {
		LOG_INF("LTE:  RRC %s | PSM not granted",
			rrc_connected ? "connected" : "idle");
	} else {
		LOG_INF("LTE:  RRC %s | PSM TAU %ds active %ds",
			rrc_connected ? "connected" : "idle",
			psm_tau_s, psm_active_s);
	}

	LOG_INF("MQTT: %s", mqtt_connected ? "connected" : "disconnected");
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

	/* Device identity: IMEI on hardware, env/Kconfig on native_sim */
	err = hw_id_get(device_id, sizeof(device_id));
	if (err) {
		LOG_ERR("hw_id_get: %d", err);
		return err;
	}
	snprintf(pos_topic, sizeof(pos_topic), "%s/%s/position",
		 TOPIC_PREFIX, device_id);
	snprintf(cell_topic, sizeof(cell_topic), "%s/%s/cell",
		 TOPIC_PREFIX, device_id);
	LOG_INF("device id: %s", device_id);
	LOG_INF("position topic: %s", pos_topic);
	LOG_INF("cell topic: %s", cell_topic);

	/* Start LTE async — don't block, status loop shows progress */
	LOG_INF("starting LTE (async)...");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async: %d", err);
		return err;
	}

	/* Configure GNSS but do NOT start it yet. An unregistered modem runs a
	 * continuous cell search; GNSS running against that starves the search and
	 * gains nothing itself. loc_fsm starts it once LTE has registered. */
	err = gnss_configure();
	if (err) {
		LOG_ERR("gnss configure: %d", err);
		return err;
	}
	bool gnss_running = false;

	int64_t last_post_ms   = 0;
	int64_t last_status_ms = 0;
	int64_t next_mqtt_ms   = 0;
	struct loc_status loc;

	loc_fsm_init(k_uptime_get());

	for (;;) {
		/* Wait for next PVT event (1 Hz) with a 2s timeout so the
		 * status block still prints even if GNSS is blocked by LTE. */
		k_sem_take(&pvt_sem, K_SECONDS(2));

		int64_t now = k_uptime_get();

		/* Policy first: it owns the fix-staleness and ephemeris bookkeeping. */
		loc_fsm_update(&pvt, now, lte_connected, &loc);
		bool gps_current = loc.gps_current;

		/* Run GNSS only when the FSM says so. While the modem is searching
		 * for a network the search owns the radio, and GNSS alongside it just
		 * flip-flops between a few tracked satellites and none. */
		if (loc.gnss_wanted && !gnss_running) {
			LOG_INF("LTE registered — starting GNSS");
			if (gnss_start() == 0) {
				gnss_running = true;
			}
		} else if (!loc.gnss_wanted && gnss_running) {
			LOG_INF("LTE lost — stopping GNSS until re-registered");
			(void)nrf_modem_gnss_stop();
			gnss_running = false;
		}

		/* A cold start needs ~30 s of uninterrupted GNSS. Publishing, and the
		 * 60 s MQTT keepalive, both wake the radio and cut the window short.
		 * So while the FSM wants silence, take MQTT down entirely — PSM then
		 * puts LTE to sleep and GNSS gets the whole radio. */
		if (loc.radio_quiet_wanted) {
			if (mqtt_connected) {
				LOG_INF("loc: ACQUIRE — dropping MQTT to free the radio");
				(void)mqtt_helper_disconnect();
			}
			/* Don't let the reconnect below fire the moment we leave. */
			next_mqtt_ms = now + MQTT_RETRY_MS;
		}

		/* (Re)connect MQTT whenever LTE is up and the broker isn't. This
		 * covers the first connect at boot and every later drop — a broker
		 * restart, a lost PDN — without needing to power-cycle the board.
		 * mqtt_helper_init() accepts UNINIT and DISCONNECTED alike, so
		 * tracker_mqtt_connect() is safe to call again after a disconnect.
		 * -EOPNOTSUPP just means an attempt is still in flight (no connack
		 * yet); back off and let it settle rather than logging a failure. */
		if (!loc.radio_quiet_wanted &&
		    lte_connected && !mqtt_connected && now >= next_mqtt_ms) {
			next_mqtt_ms = now + MQTT_RETRY_MS;
			LOG_INF("connecting MQTT to %s:%d...", BROKER_HOST, BROKER_PORT);
			err = tracker_mqtt_connect();
			if (err && err != -EOPNOTSUPP) {
				LOG_WRN("MQTT connect failed: %d (retry in %d s)",
					err, MQTT_RETRY_MS / 1000);
			}
		}

		/* Status block every STATUS_INTERVAL_MS */
		if (now - last_status_ms >= STATUS_INTERVAL_MS) {
			last_status_ms = now;
			print_status(&pvt, &loc, now);
		}

		/* Publish once per interval when MQTT is up. Prefer a current
		 * GPS fix; otherwise report the serving cell (cheap XMONITOR
		 * read — no radio scan). Covers boot-before-GPS and GPS loss. */
		if (now - last_post_ms >= POST_INTERVAL_MS && mqtt_connected) {
			if (gps_current) {
				last_post_ms = now;
				err = mqtt_publish_fix(&pvt);
				if (err) {
					LOG_WRN("publish failed: %d (will retry)", err);
				}
			} else if (lte_connected) {
				last_post_ms = now;
				struct serving_cell sc;

				if (read_serving_cell(&sc) == 0) {
					err = mqtt_publish_cell(&sc);
					if (err) {
						LOG_WRN("cell publish failed: %d", err);
					} else {
						/* Coarse position is on the map; the FSM
						 * may now take the radio for GNSS. */
						loc_fsm_note_cell_sent();
					}
				} else {
					LOG_WRN("serving cell unavailable");
				}
			}
		} else if (!mqtt_connected) {
			last_post_ms = now - POST_INTERVAL_MS + 3000;
		}
	}

	return 0;
}
