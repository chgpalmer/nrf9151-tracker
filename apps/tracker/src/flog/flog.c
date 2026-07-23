/*
 * flog: flash flight recorder — a log backend that lands every compiled
 * log line in a circular flash log surviving reboot and power loss.
 *
 * Born from the 2026-07-17 field wedge: LTE registered, GNSS fixing, main
 * loop alive, zero uplink for 45 h — and the DBG stream that held the
 * diagnosis existed nowhere durable. This is the durable tier the
 * log-uplink ring's Kconfig always promised.
 *
 * Stack: log core → this backend → RAM staging ring → drain thread →
 * FCB (in-tree circular buffer: CRC8/element, boot head/tail recovery,
 * sector rotate) → flash_map "flight_log" partition → spi_nor (hw) /
 * flash_simulator (sim). Partition layout is hardcoded in the DT
 * overlays; FCB sectors are LOGICAL (partition/CONFIG_TRACKER_FLOG_SECTORS,
 * uniform array built here) because flash_area_get_sectors() on 8 MB of
 * 4 KB-erase NOR would burn 16 KB of RAM on descriptors.
 *
 * Concurrency: LOG_MODE_IMMEDIATE means process() runs in WHATEVER thread
 * logs — so it only formats into a spinlock-guarded drop-oldest ring
 * (same discipline as log_uplink.c) and pokes a semaphore. All flash I/O
 * (append, page program, the ~1-2 s sector-rotate erase) happens in the
 * lowest-priority drain thread.
 *
 * Re-entrancy: flash/SPI drivers log. Lines logged BY the drain thread
 * are dropped here (they still reach UART/uplink) so a failing driver
 * cannot feed the recorder its own funeral. Repeated append failures put
 * the recorder OFFLINE — one edge WRN, never a loop (hard rule 1).
 */
#include "flog.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(flog, CONFIG_TRACKER_LOG_LEVEL);

#define RING_CAP CONFIG_TRACKER_FLOG_RING_LINES
#define TEXT_MAX 96
#define MOD_MAX  16

/* On-flash record: header + two NUL-terminated strings. Bump FCB_VERSION
 * if this ever changes — old sectors then fail CRC-of-version and rot out
 * naturally on rotate. */
#define FCB_MAGIC   0x666c6f67 /* "flog" */
#define FCB_VERSION 1

struct __packed rec_hdr {
	uint32_t t_s;   /* uptime seconds */
	uint16_t t_ms;  /* millisecond remainder */
	uint8_t  level; /* 1=ERR 2=WRN 3=INF 4=DBG */
};

#define REC_MAX (sizeof(struct rec_hdr) + MOD_MAX + TEXT_MAX)

struct line {
	int64_t t_ms;
	uint8_t level;
	char mod[MOD_MAX];
	char text[TEXT_MAX];
};

static struct line ring[RING_CAP];
static size_t r_start, r_count;
static uint32_t r_dropped;      /* staging overflow, reported in stats */
static struct k_spinlock ring_lock;
static K_SEM_DEFINE(drain_sem, 0, 1);

static struct fcb fcb;
static struct flash_sector sectors[CONFIG_TRACKER_FLOG_SECTORS];
static bool flog_up;            /* init succeeded and not offline */
static int last_err;
static uint32_t consec_fails;
static uint32_t written_lines;  /* since boot */
#define MAX_CONSEC_FAILS 8

static k_tid_t drain_tid;

/* ---- backend side: log core -> staging ring (any thread) ---------------- */

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
	/* The text formatter prefixes "module: " even with the level flag
	 * off; the module travels as its own field, so strip the duplicate
	 * (same quirk log_uplink handles). */
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
LOG_OUTPUT_DEFINE(flog_output, out_func, out_buf, sizeof(out_buf));

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	uint8_t level = log_msg_get_level(&msg->log);

	/* 0 = raw printk-ish; treat as INF. Higher number = chattier. */
	if (level > CONFIG_TRACKER_FLOG_LEVEL && level != 0) {
		return;
	}
	/* Never record the drain thread's own consequences (SPI driver
	 * errors mid-write): the feedback loop ends here, the lines still
	 * reach the other backends. */
	if (!flog_up || k_current_get() == drain_tid) {
		return;
	}

	int16_t sid = log_msg_get_source_id(&msg->log);
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	cur_level = level ? level : LOG_LEVEL_INF;
	cur_mod = (sid >= 0) ? log_source_name_get(0, sid) : NULL;
	log_output_msg_process(&flog_output, &msg->log,
			       LOG_OUTPUT_FLAG_CRLF_NONE);
	push_line(); /* in case the message didn't end in a newline */
	k_spin_unlock(&ring_lock, key);

	k_sem_give(&drain_sem);
}

static void panic(const struct log_backend *const backend)
{
	/* Flash I/O from panic context is how you corrupt the evidence. */
	log_backend_deactivate(backend);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	r_dropped += cnt;
	k_spin_unlock(&ring_lock, key);
}

static const struct log_backend_api flog_backend_api = {
	.process = process,
	.panic = panic,
	.dropped = dropped,
};

/* autostart=false: nothing may reach the ring before fcb recovery ran. */
LOG_BACKEND_DEFINE(log_backend_flog, flog_backend_api, false);

/* ---- flash side: ring -> FCB (drain thread only) ------------------------ */

static size_t rec_assemble(uint8_t *buf, const struct line *l)
{
	struct rec_hdr h = {
		.t_s = (uint32_t)(l->t_ms / 1000),
		.t_ms = (uint16_t)(l->t_ms % 1000),
		.level = l->level,
	};
	size_t ml = strlen(l->mod) + 1;
	size_t tl = strlen(l->text) + 1;

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), l->mod, ml);
	memcpy(buf + sizeof(h) + ml, l->text, tl);
	return sizeof(h) + ml + tl;
}

static int rec_write(const uint8_t *buf, size_t len)
{
	struct fcb_entry loc;
	int err = fcb_append(&fcb, len, &loc);

	if (err == -ENOSPC) {
		/* Ring full: reclaim the oldest sector. Seconds of erase,
		 * but we are the lowest-priority thread in the system. */
		err = fcb_rotate(&fcb);
		if (err == 0) {
			err = fcb_append(&fcb, len, &loc);
		}
	}
	if (err) {
		return err;
	}
	err = flash_area_write(fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), buf, len);
	if (err) {
		return err;
	}
	return fcb_append_finish(&fcb, &loc);
}

static void drain_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	static uint8_t buf[REC_MAX]; /* drain thread only: static is safe */
	struct line l;

	for (;;) {
		k_sem_take(&drain_sem, K_FOREVER);

		for (;;) {
			if (!flog_up) {
				break; /* offline: stop touching flash */
			}

			k_spinlock_key_t key = k_spin_lock(&ring_lock);

			if (r_count == 0) {
				k_spin_unlock(&ring_lock, key);
				break;
			}
			l = ring[r_start];
			r_start = (r_start + 1) % RING_CAP;
			r_count--;
			k_spin_unlock(&ring_lock, key);

			size_t len = rec_assemble(buf, &l);
			int err = rec_write(buf, len);

			if (err == 0) {
				written_lines++;
				consec_fails = 0;
			} else {
				last_err = err;
				if (++consec_fails >= MAX_CONSEC_FAILS) {
					/* Edge, not loop: one WRN then dark.
					 * `flog erase` or reboot re-arms. */
					flog_up = false;
					LOG_WRN("recorder OFFLINE after %u "
						"failures (last err %d)",
						consec_fails, last_err);
					break;
				}
			}
		}
	}
}

K_THREAD_DEFINE(flog_drain, 2048, drain_fn, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* Marker records (boot boundary, operator note) bypass the log core so
 * they exist even when nothing else flows. Drain-thread-free path: called
 * rarely, small, and the ring/sem machinery handles it like any line. */
static void flog_note(const char *text)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	if (r_count == RING_CAP) {
		r_start = (r_start + 1) % RING_CAP;
		r_count--;
		r_dropped++;
	}

	struct line *l = &ring[(r_start + r_count) % RING_CAP];

	l->t_ms = k_uptime_get();
	l->level = LOG_LEVEL_INF;
	strncpy(l->mod, "flog", MOD_MAX - 1);
	l->mod[MOD_MAX - 1] = '\0';
	strncpy(l->text, text, TEXT_MAX - 1);
	l->text[TEXT_MAX - 1] = '\0';
	r_count++;
	k_spin_unlock(&ring_lock, key);
	k_sem_give(&drain_sem);
}

int flog_init(void)
{
	const struct flash_area *fap;
	int err;

	err = flash_area_open(FIXED_PARTITION_ID(flight_log), &fap);
	if (err) {
		LOG_WRN("no flight_log partition (%d) — recorder off", err);
		return err;
	}

	/* Uniform logical sectors over the whole partition (see top). */
	uint32_t sec_size = fap->fa_size / CONFIG_TRACKER_FLOG_SECTORS;

	flash_area_close(fap);
	for (int i = 0; i < CONFIG_TRACKER_FLOG_SECTORS; i++) {
		sectors[i].fs_off = (off_t)i * sec_size;
		sectors[i].fs_size = sec_size;
	}

	fcb = (struct fcb){
		.f_magic = FCB_MAGIC,
		.f_version = FCB_VERSION,
		.f_sector_cnt = CONFIG_TRACKER_FLOG_SECTORS,
		.f_scratch_cnt = 0,
		.f_sectors = sectors,
	};

	err = fcb_init(FIXED_PARTITION_ID(flight_log), &fcb);
	if (err) {
		/* Unformatted or torn flash: start fresh once, not a loop. */
		LOG_WRN("fcb_init %d — erasing ring and retrying", err);
		if (flash_area_open(FIXED_PARTITION_ID(flight_log), &fap) == 0) {
			(void)flash_area_erase(fap, 0, fap->fa_size);
			flash_area_close(fap);
		}
		err = fcb_init(FIXED_PARTITION_ID(flight_log), &fcb);
		if (err) {
			LOG_WRN("flight recorder unavailable (%d)", err);
			return err;
		}
	}

	drain_tid = flog_drain;
	flog_up = true;
	log_backend_enable(&log_backend_flog, NULL, CONFIG_TRACKER_FLOG_LEVEL);
	flog_note("=== boot ===");
	LOG_INF("flight recorder up: %u KB ring, %d sectors",
		(unsigned)(fcb.f_sector_cnt * sec_size / 1024),
		CONFIG_TRACKER_FLOG_SECTORS);
	return 0;
}

/* ---- shell: the extraction surface -------------------------------------- */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <stdlib.h>

struct dump_ctx {
	const struct shell *sh;
	uint32_t skip;  /* entries to skip before printing */
	uint32_t shown;
};

static int dump_cb(struct fcb_entry_ctx *ctx, void *arg)
{
	struct dump_ctx *d = arg;
	uint8_t buf[REC_MAX];
	uint16_t len = MIN(ctx->loc.fe_data_len, sizeof(buf));

	if (d->skip > 0) {
		d->skip--;
		return 0;
	}
	if (flash_area_read(ctx->fap, FCB_ENTRY_FA_DATA_OFF(ctx->loc),
			    buf, len)) {
		return 0; /* skip unreadable, keep walking */
	}
	if (len < sizeof(struct rec_hdr) + 2) {
		return 0;
	}

	struct rec_hdr h;

	memcpy(&h, buf, sizeof(h));
	/* Defensive NUL: a torn record must not run the print off the end. */
	buf[len - 1] = '\0';

	const char *mod = (const char *)buf + sizeof(h);
	size_t ml = strnlen(mod, len - sizeof(h));
	const char *text = (ml + sizeof(h) + 1 < len) ? mod + ml + 1 : "";
	static const char lvl_ch[] = { '?', 'E', 'W', 'I', 'D' };

	shell_print(d->sh, "[+%02u:%02u:%02u.%03u] %c %s: %s",
		    h.t_s / 3600, (h.t_s / 60) % 60, h.t_s % 60, h.t_ms,
		    lvl_ch[MIN(h.level, 4)], mod, text);
	d->shown++;
	return 0;
}

static int count_cb(struct fcb_entry_ctx *ctx, void *arg)
{
	ARG_UNUSED(ctx);
	(*(uint32_t *)arg)++;
	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t want = (argc > 1) ? strtoul(argv[1], NULL, 10) : 50;
	uint32_t total = 0;

	if (fcb.fap == NULL) {
		shell_warn(sh, "recorder never initialised");
		return -ENODEV;
	}
	if (!flog_up) {
		shell_warn(sh, "recorder offline (last err %d)", last_err);
	}
	/* Two walks: count, then print the tail. A full 8 MB ring is ~130 k
	 * headers — seconds on SPI, fine for a forensic shell command.
	 * (fcb_offset_last_n caps n at 255, too small for "last 1000".) */
	fcb_walk(&fcb, NULL, count_cb, &total);
	struct dump_ctx d = {
		.sh = sh, .skip = (total > want) ? total - want : 0,
	};

	fcb_walk(&fcb, NULL, dump_cb, &d);
	shell_print(sh, "-- %u of %u lines --", d.shown, total);
	return 0;
}

/* ---- bench: split the dump path's ~7.5 ms/record three ways -------------
 * (flash walk vs shell printing vs the host-side transport). Field question
 * 2026-07-23: dumps ran 9.6 KB/s at BOTH 115200 and 1 Mbaud. */

struct bench_ctx {
	uint32_t limit;
	uint32_t count;
	uint32_t bytes;
};

static int bench_cb(struct fcb_entry_ctx *ctx, void *arg)
{
	struct bench_ctx *b = arg;
	uint8_t buf[REC_MAX];
	uint16_t len = MIN(ctx->loc.fe_data_len, sizeof(buf));

	/* Same read the dump does, minus the printing. */
	if (flash_area_read(ctx->fap, FCB_ENTRY_FA_DATA_OFF(ctx->loc),
			    buf, len) == 0) {
		b->bytes += len;
	}
	b->count++;
	return (b->count >= b->limit) ? 1 : 0; /* nonzero stops the walk */
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = (argc > 2) ? strtoul(argv[2], NULL, 10) : 2000;

	if (argc > 1 && strcmp(argv[1], "walk") == 0) {
		if (fcb.fap == NULL) {
			shell_warn(sh, "recorder never initialised");
			return -ENODEV;
		}

		struct bench_ctx b = { .limit = n };
		int64_t t0 = k_uptime_get();

		fcb_walk(&fcb, NULL, bench_cb, &b);

		int64_t dt = k_uptime_get() - t0;

		shell_print(sh, "walk: %u records, %u B, %lld ms (%lld rec/s)",
			    b.count, b.bytes, (long long)dt,
			    dt ? (long long)(b.count * 1000 / dt) : 0);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "print") == 0) {
		/* ~72 chars/line like a real dump line; no flash involved.
		 * Device-side elapsed includes shell TX backpressure, so
		 * pair it with host-side arrival timing to see the wire. */
		int64_t t0 = k_uptime_get();

		for (uint32_t i = 0; i < n; i++) {
			shell_print(sh, "[bench %6u] ................"
				    "........................................",
				    i);
		}

		int64_t dt = k_uptime_get() - t0;

		shell_print(sh, "print: %u lines, %lld ms (%lld lines/s)",
			    n, (long long)dt,
			    dt ? (long long)(n * 1000 / dt) : 0);
		return 0;
	}
	shell_print(sh, "usage: flog bench walk|print [n]");
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (fcb.fap == NULL) {
		shell_warn(sh, "recorder never initialised");
		return -ENODEV;
	}
	shell_print(sh, "state: %s  last_err: %d",
		    flog_up ? "recording" : "OFFLINE", last_err);
	shell_print(sh, "since boot: %u written, %u dropped (staging)",
		    written_lines, r_dropped);
	shell_print(sh, "ring: %d sectors x %u KB, %d free",
		    fcb.f_sector_cnt,
		    (unsigned)(fcb.f_sectors[0].fs_size / 1024),
		    fcb_free_sector_cnt(&fcb));
	return 0;
}

static int cmd_mark(const struct shell *sh, size_t argc, char **argv)
{
	char text[TEXT_MAX] = "mark: ";

	for (size_t i = 1; i < argc; i++) {
		strncat(text, argv[i], sizeof(text) - strlen(text) - 2);
		strncat(text, " ", sizeof(text) - strlen(text) - 1);
	}
	flog_note(text);
	shell_print(sh, "marked");
	return 0;
}

static int cmd_erase(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (fcb.fap == NULL) {
		shell_warn(sh, "recorder never initialised");
		return -ENODEV;
	}

	int err = fcb_clear(&fcb);

	if (err == 0) {
		/* Also the offline-recovery path: fresh flash, fresh luck. */
		consec_fails = 0;
		flog_up = true;
	}
	shell_print(sh, "erase: %d", err);
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(flog_cmds,
	SHELL_CMD_ARG(dump, NULL, "dump [n] — last n lines (default 50)",
		      cmd_dump, 1, 1),
	SHELL_CMD(stats, NULL, "recorder state", cmd_stats),
	SHELL_CMD_ARG(bench, NULL, "bench walk|print [n] — dump-path timing",
		      cmd_bench, 2, 1),
	SHELL_CMD_ARG(mark, NULL, "mark <text> — operator marker",
		      cmd_mark, 2, 8),
	SHELL_CMD(erase, NULL, "erase the ring (recovery path)", cmd_erase),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(flog, &flog_cmds, "flash flight recorder", NULL);
#endif /* CONFIG_SHELL */
