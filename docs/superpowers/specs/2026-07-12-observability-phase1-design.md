# Observability Phase 1: uplink layer + logs over LTE — design

Approved 2026-07-12 (brainstormed with Claude; this is the validated design,
not a proposal). Motivation: a 22-minute silent field wedge with no logs to
diagnose it, and send-path architecture debt — flush policy braided into
`main.c`/FSM state, `obs_queue` owning both data and transport calls, log
data having exactly one escape route (UART, useless on a power bank).

Phase 2 (separate, later): epoch timestamps, 32 MB NOR partitions, FCB flash
flight recorder, flash obs spill. Parked deliberately: uplink liveness /
ping-escalation — build the flight recorder before the autopilot.

## 1. Module layout

```
src/net/uplink.{c,h}    NEW  — envelope owner, flush policy, drain loop
src/net/obs_queue.c     ROLE CHANGE — keeps ring/segmentation/encode; loses
                          its flush loop; registers cell (prio 0) and gps
                          segment (prio 1) sources
src/net/log_uplink.c    NEW  — Zephyr log backend + uplink source (prio 2)
src/net/coap_pub.c      UNCHANGED beneath uplink
src/loc/loc_fsm.c       SHRINKS — cell-sent glue becomes uplink_on_sent(mask)
src/main.c              SHRINKS — init + sampling + FSM tick only
```

Obs ring grows 64 → 2048 (~96 KB of ~170 KB headroom). LED3 pulse and the
RRC-tail measurement move behind `uplink_on_sent`.

## 2. Wire schema

```proto
message LogLine  { uint32 age_s = 1; uint32 level = 2; string module = 3; string text = 4; }
message LogBatch { repeated LogLine lines = 1; }
/* Entry.kind oneof += LogBatch log = 4; */
```

Text stays text. Levels are Zephyr's (1=ERR…4=DBG). Old decoders skip the
unknown arm.

## 3. Uplink

Pull model. Sources register a vtable:

```c
struct uplink_source {
    const char *name;
    bool (*has_pending)(void);
    int  (*encode_next)(pb_ostream_t *stream, size_t budget); /* one Entry */
    void (*commit)(void);    /* datagram containing your entries was sent */
    void (*rollback)(void);  /* it wasn't; rewind */
};
```

Flush reasons: timer (2 min) · fullness (source announces a full segment) ·
urgent request (WRN+ log, cell queued, FSM state change). Urgent log wakes
rate-limited ≥60 s. All flushing respects `uplink_set_allowed()` (the FSM's
radio verdict). Drain loop: per-datagram budget ~450 B, sources in fixed
priority order, multiple datagrams per radio wake, RAI on the last, commit
only after send success (transactional pull). Producers never block; every
source drops-oldest with a visible counter.

## 4. Log path + re-tiering

`log_uplink` backend: Kconfig threshold (default INF), compact lines into an
8 KB drop-oldest ring, `dropped N` line on overflow, WRN+ → urgent flush.
Re-entrancy trap: uplink's own logging is DBG-only and urgent requests are
deferred-by-flag — the backend must never re-enter uplink's send path.

Re-tiering: INF ≈ a handful of lines per 10 min (boot identity,
registration/PSM/PDN edges, FSM transitions, fix gained/lost, send errors);
per-second chatter (status blocks, sat tables, flush confirmations) → DBG.
UART still shows DBG; the shell (`CONFIG_SHELL`) provides runtime per-module
log levels with Zephyr's built-in `log` commands.

## 5. Server + webapp

`coap_server.py` decodes the new arm into `logs(device_id, received_ts REAL,
level INT, module TEXT, text TEXT)`, ts = received − age. `app.py` gains
`GET /api/logs?device&from_ts&to_ts&min_level&limit`. Webapp gains a Logs
sidebar page: level-colored rows, ERR/WRN/INF/DBG filter chips, date picker +
steppers (Map-page pattern), incremental auto-refresh on today.

## 6. Error handling

Send failure → rollback, retry next flush. LTE down → nothing pulls, rings
age under local drop policy. Radio-dark periods (GNSS_EXCLUSIVE) queue logs
and deliver after re-attach with correct ages — post-wedge forensics arrive
by themselves.

## 7. Verification

Sim e2e (`make demo`): logs flow sim → server → logs table; INF quiet; WRN
within ~60 s; map unchanged. `make webtest` gains Logs-page assertions.
`smoke.sh` PASS. Hardware desk session: UART unaffected, wire quiet at INF,
WRN wakes the radio promptly, GNSS_EXCLUSIVE stint delivers queued logs
after re-attach.
