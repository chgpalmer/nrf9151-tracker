# nrf9151-tracker — agent working rules

GPS tracker: nRF9151-DK firmware (Zephyr / NCS v3.4) + Python CoAP server +
web map. This repo is the manifest repo of a west workspace: it lives at
`<ws>/nrf9151-tracker`; the workspace root holds `.venv`, `zephyr/`, `nrf/`,
`modules/`. Supported host: **Ubuntu 24.04 LTS / Python 3.12 only** (NCS
module pins have no wheels for newer Pythons; 26.04 breaks `make setup-zephyr`).

## Commands
- `make build` / `make flash` / `make uart` — hardware (`nrf9151dk/nrf9151/ns`)
- `make sim` → `make demo` — native_sim + local CoAP server + web map on :8080
- `./smoke.sh` — build + demo e2e; `make webtest` — headless-Chromium UI test
- `make proto` — regen nanopb C + server `tracker_pb2.py` after editing
  `proto/tracker.proto` (generated code is committed; never hand-edit it)

## Hard rules
1. **Data plan is 10 MB/month; every uplink byte is budgeted.**
   - The wire ships log levels ≤ INF (`TRACKER_LOG_UPLINK_LEVEL`). INF must
     stay QUIET (~a handful of lines per 10 min): discrete events only —
     boot identity, registration/PSM/PDN edges, FSM transitions, send errors.
     Anything periodic or per-sample is DBG (UART/shell only, compiled in via
     `TRACKER_LOG_LEVEL=4`).
   - **Never log a persisting condition repeatedly — log its edges** (onset
     WRN, clear INF). Precedent: the GNSS window warnings in `main.c`.
   - A WRN+ line triggers a (rate-limited) radio wake. No WRN/ERR in loops.
   - Before adding any recurring transmission, do the bytes-per-month math in
     the commit message (see README "Data budget" for the method).
2. **`uplink` is the only sender.** Producers register as `uplink_source`s
   (pull model; `encode_next/commit/rollback` is transactional — cursors
   advance only after a confirmed send). Never call `coap_pub_send` directly.
3. **`proto/tracker.proto` is the single wire contract.** nanopb uses
   FT_CALLBACK everywhere (no fixed arrays on the stack); a segment's point
   count is decided BEFORE `pb_encode`, never trimmed mid-encode (packed
   arrays would desync). Field numbers are forever; never reuse retired ones.
4. **`loc_fsm` owns the GNSS/LTE radio arbitration.** Single `next_state()`
   writer; every transition logs `A -> B (reason)`. Timers only where no
   event exists, and each one needs a written justification.
5. **Verify before claiming done.** Firmware: build BOTH targets (sim +
   hardware). Webapp: `make webtest` is mandatory — parse checks and curl are
   not tests (a page can serve fine and die at runtime). End-to-end: smoke.sh.
6. **Deploy order for wire-format changes: server first** (VM: `git pull &&
   make setup-host`, restart), then flash. Static assets are served
   `Cache-Control: no-cache`; after structural JS renames, hard-refresh once.
7. **Design before feature code.** Brainstorm with the user first (they want
   discussion, not option menus); approved designs live in
   `docs/superpowers/specs/`. Grade ideas against measured evidence; record
   rejected ideas with reasons.

## State of play (update when it changes)
- Wire: protobuf v3 — anchor+delta track segments (~5-7 B/pt @1 Hz), cell
  fixes, log batches. Mixed-month ≈ 1.7 MB + logs.
- Known unsolved data cost: **parked devices** — CELL_LOOP samples a cell
  every 30 s (~7.4 MB/mo if parked in it all month) and failed-acquire churn
  narrates ~1 KB/cycle. Fix = adaptive cadence / quiescent mode (queued;
  prerequisite: the FSM decision-table review + ztest).
- Phase 2 (specced, queued): flash flight recorder + obs spill on the DK's
  32 MB NOR, epoch timestamps, log byte-budget backstop.
- GNSS acquisition-while-moving is weak (ephemeris demodulation needs steady
  signal); A-GNSS is the eventual fix. LED2 solid = fix held = safe to move.
