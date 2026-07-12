#include "agnss.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "coap_pub.h"
#include "tracker.pb.h"

LOG_MODULE_REGISTER(agnss, CONFIG_TRACKER_LOG_LEVEL);

/* One exchange: ~25 B up, ~1 KB down. See Kconfig for the trigger knobs.
 * The timeout must survive an RRC release + paging cycle on a roaming
 * network: if RAI released the connection right after our request went out,
 * the ~1 KB response has to page us back — measured multi-second. */
#define EXCHANGE_TIMEOUT_MS 8000
#define NEED_CHECK_MS       30000
#define RETRY_S             60
#define LOCKOUT_S           900
#define MAX_CONSEC_FAILS    5

static int64_t next_attempt_ms;
static int64_t last_need_check_ms = -NEED_CHECK_MS;
static bool    need;
static uint32_t need_flags;
static uint64_t need_mask;
static uint8_t consec_fails;
static int     last_exchange_err;
/* Edge-logged failure episode: one WRN when exchanges start failing, one INF
 * when assistance flows again. Field lesson (2026-07-12 ride): two boot-time
 * responses died on the air invisibly (failures were DBG-only) and the
 * diagnosis needed the server's ledger. */
static bool    exchange_failing;

static const char *device_id_str;

/* Runtime kill-switch: the fallback (plain over-the-air demodulation) must
 * stay testable on hardware without a rebuild. `agnss off` on the shell. */
static bool agnss_enabled = true;

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>

static int cmd_agnss(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		agnss_enabled = (strcmp(argv[1], "on") == 0);
	}
	shell_print(sh, "agnss %s", agnss_enabled ? "on" : "off");
	return 0;
}
SHELL_CMD_ARG_REGISTER(agnss, NULL, "A-GNSS assistance fetch (on|off)",
		       cmd_agnss, 1, 1);
#endif

void agnss_set_device_id(const char *id)
{
	device_id_str = id;
}

/* Ask the modem what it is missing. "Thin" = fewer than MIN_HEALTHY GPS
 * satellites with at least MIN_LEFT_MIN of ephemeris left — the drive-gap
 * failure mode was a 4-5 SV inventory losing sight of one or two. */
static void refresh_need(int64_t now_ms)
{
	struct nrf_modem_gnss_agnss_expiry exp;

	if (now_ms - last_need_check_ms < NEED_CHECK_MS) {
		return;
	}
	last_need_check_ms = now_ms;

	if (nrf_modem_gnss_agnss_expiry_get(&exp) != 0) {
		return; /* keep the previous verdict */
	}

	int healthy = 0;

	need_mask = 0;
	for (int i = 0; i < exp.sv_count; i++) {
		if (exp.sv[i].sv_id < 1 || exp.sv[i].sv_id > 32) {
			continue; /* GPS only */
		}
		if (exp.sv[i].ephe_expiry >= CONFIG_TRACKER_AGNSS_MIN_LEFT_MIN) {
			healthy++;
		} else {
			need_mask |= 1ULL << (exp.sv[i].sv_id - 1);
		}
	}
	need_flags = exp.data_flags;
	need = (healthy < CONFIG_TRACKER_AGNSS_MIN_HEALTHY) ||
	       (exp.data_flags &
		(NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST |
		 NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST));
	LOG_DBG("%d healthy SVs, flags 0x%02x -> %s", healthy,
		exp.data_flags, need ? "want assistance" : "ok");
}

/* Decode one AgnssEphemeris (streamed by nanopb) and hand it straight to the
 * modem — one element at a time, never an array of them on the stack. */
static bool eph_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
	struct { int written; int failed; } *count = *arg;
	AgnssEphemeris p = AgnssEphemeris_init_zero;
	struct nrf_modem_gnss_agnss_gps_data_ephemeris e;

	ARG_UNUSED(field);
	if (!pb_decode(stream, AgnssEphemeris_fields, &p)) {
		return false;
	}

	e = (struct nrf_modem_gnss_agnss_gps_data_ephemeris){
		.sv_id = p.sv_id,     .health = p.health,
		.iodc = p.iodc,       .toc = p.toc,
		.af2 = p.af2,         .af1 = p.af1,
		.af0 = p.af0,         .tgd = p.tgd,
		.ura = p.ura,         .fit_int = p.fit_int,
		.toe = p.toe,         .w = p.w,
		.delta_n = p.delta_n, .m0 = p.m0,
		.omega_dot = p.omega_dot, .e = p.e,
		.idot = p.idot,       .sqrt_a = p.sqrt_a,
		.i0 = p.i0,           .omega0 = p.omega0,
		.crs = p.crs,         .cis = p.cis,
		.cus = p.cus,         .crc = p.crc,
		.cic = p.cic,         .cuc = p.cuc,
	};

	int err = nrf_modem_gnss_agnss_write(&e, sizeof(e),
					     NRF_MODEM_GNSS_AGNSS_GPS_EPHEMERIDES);

	if (err == 0) {
		count->written++;
		LOG_DBG("ephemeris sv %u injected", p.sv_id);
	} else {
		count->failed++;
		LOG_DBG("ephemeris sv %u REJECTED (%d)", p.sv_id, err);
	}
	return true; /* a rejected element must not abort the decode */
}

static int inject(const uint8_t *payload, size_t len)
{
	AgnssData data = AgnssData_init_zero;
	struct { int written; int failed; } count = { 0, 0 };
	bool time_ok = false, loc_ok = false;

	data.ephemeris.funcs.decode = eph_cb;
	data.ephemeris.arg = &count;

	pb_istream_t is = pb_istream_from_buffer(payload, len);

	if (!pb_decode(&is, AgnssData_fields, &data)) {
		LOG_WRN("decode failed: %s", PB_GET_ERROR(&is));
		return -EBADMSG;
	}

	if (data.has_time) {
		/* sv_mask 0: no per-satellite TOW, day+time alone still turns
		 * a cold start warm. (~200 B struct: static, not stack.) */
		static struct nrf_modem_gnss_agnss_gps_data_system_time_and_sv_tow t;

		memset(&t, 0, sizeof(t));
		t.date_day = data.time.date_day;
		t.time_full_s = data.time.time_full_s;
		t.time_frac_ms = data.time.time_frac_ms;
		time_ok = nrf_modem_gnss_agnss_write(&t, sizeof(t),
			NRF_MODEM_GNSS_AGNSS_GPS_SYSTEM_CLOCK_AND_TOWS) == 0;
	}
	if (data.has_location) {
		struct nrf_modem_gnss_agnss_data_location l = {
			.latitude = data.location.latitude,
			.longitude = data.location.longitude,
			.altitude = data.location.altitude,
			.unc_semimajor = data.location.unc_semimajor,
			.unc_semiminor = data.location.unc_semiminor,
			.orientation_major = 0,
			.unc_altitude = data.location.unc_altitude,
			.confidence = data.location.confidence,
		};

		loc_ok = nrf_modem_gnss_agnss_write(&l, sizeof(l),
			NRF_MODEM_GNSS_AGNSS_LOCATION) == 0;
	}

	LOG_INF("injected%s%s %d ephemerides (%d rejected)",
		time_ok ? " time" : "", loc_ok ? " location" : "",
		count.written, count.failed);
	return (count.written > 0 || time_ok) ? 0 : -ENODATA;
}

static int fetch_and_inject(void)
{
	static uint8_t resp[1200]; /* ~1 KB response: static, not stack */
	uint8_t req[48];
	AgnssRequest r = AgnssRequest_init_zero;

	strncpy(r.device_id, device_id_str ? device_id_str : "?",
		sizeof(r.device_id) - 1);
	r.data_flags = need_flags;
	r.sv_mask_ephe = need_mask;

	pb_ostream_t os = pb_ostream_from_buffer(req, sizeof(req));

	if (!pb_encode(&os, AgnssRequest_fields, &r)) {
		return -EINVAL;
	}

	int n = coap_pub_exchange("agnss", req, os.bytes_written,
				  resp, sizeof(resp), EXCHANGE_TIMEOUT_MS);

	if (n < 0) {
		last_exchange_err = n;
		LOG_DBG("exchange failed (%d)", n);
		return n;
	}
	return inject(resp, n);
}

void agnss_poll(int64_t now_ms, const struct loc_status *loc)
{
	/* Only when the radio is already ours to use: publishing states with
	 * the socket up. ACQUIRE/EXCLUSIVE need silence; QUIESCENT/REST need
	 * sleep — both re-enter a publishing state before they could benefit. */
	bool allowed = (loc->state == LOC_REPORT_CELL ||
			loc->state == LOC_REPORT_GNSS ||
			loc->state == LOC_CELL_LOOP) &&
		       loc->publish_allowed && coap_pub_ready();

	if (!agnss_enabled || !allowed || now_ms < next_attempt_ms) {
		return;
	}
	refresh_need(now_ms);
	if (!need) {
		return;
	}

	int err = fetch_and_inject();

	if (err == 0) {
		if (exchange_failing) {
			LOG_INF("exchange recovered");
			exchange_failing = false;
		}
		consec_fails = 0;
		need = false;
		last_need_check_ms = now_ms; /* re-evaluate in NEED_CHECK_MS */
		next_attempt_ms = now_ms +
			(int64_t)CONFIG_TRACKER_AGNSS_COOLDOWN_S * 1000;
	} else if (!exchange_failing) {
		/* Episode onset. -EAGAIN = response lost on the air (NON both
		 * ways, nobody retransmits); -EBADMSG/-ENODATA = it arrived
		 * broken. */
		LOG_WRN("exchange failed (%d) — will retry", err);
		exchange_failing = true;
		consec_fails = 1;
		next_attempt_ms = now_ms + RETRY_S * 1000;
	} else if (++consec_fails >= MAX_CONSEC_FAILS) {
		/* Edge-logged: one WRN per lockout, not one per failure. */
		LOG_WRN("%u fetches failed (last err %d), backing off %d min",
			consec_fails, last_exchange_err, LOCKOUT_S / 60);
		consec_fails = 0;
		next_attempt_ms = now_ms + LOCKOUT_S * 1000;
	} else {
		next_attempt_ms = now_ms + RETRY_S * 1000;
	}
}
