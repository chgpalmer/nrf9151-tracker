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
- `make fsmtest` — loc_fsm unit test vs `docs/loc-fsm-decision-table.md`
- `make flogtest` — flash flight recorder e2e on native_sim (init,
  persistence across reboot, ring rotation, erase); mandatory for any
  `src/flog/` change
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
     the commit message (see README "Data budget" for the method), then
     DOUBLE it: measured SIM billing runs ~2x the app-layer arithmetic
     (IP/UDP headers + per-RRC-session carrier rounding; calibrated
     2026-07-12: 105 KB estimated, 0.21 MB billed). Radio wakes cost bytes
     even when the payload is tiny.
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
   `docs/loc-fsm-decision-table.md` is normative and `tests/fsm/` encodes it
   row by row: change the FSM, the table, and the test together — `make
   fsmtest` is mandatory for any loc_fsm change.
5. **Verify before claiming done.** Firmware: build BOTH targets (sim +
   hardware). Webapp: `make webtest` is mandatory — parse checks and curl are
   not tests (a page can serve fine and die at runtime). End-to-end: smoke.sh.
6. **Deploy order for wire-format changes: server first** (VM: `git pull &&
   make setup-host`, restart), then flash. Static assets are served
   `Cache-Control: no-cache`; after structural JS renames, hard-refresh once.
7. **Design before feature code.** Brainstorm with the user first (they want
   discussion, not option menus). Design specs are working documents: write
   them to `docs/superpowers/specs/` (git-ignored) and follow them, but
   NEVER commit them — durable knowledge belongs in code comments, module
   docs, or the decision table. Grade ideas against measured evidence;
   record rejected ideas with reasons.

## State of play (update when it changes)
- Wire: protobuf v3 — anchor+delta track segments (~5-7 B/pt @1 Hz), cell
  fixes, log batches. Mixed-month ≈ 1.7 MB + logs.
- Parked cost SOLVED in firmware (QUIESCENT + REST + motion.c, adaptive
  cadence): parked ≈ 0.15–0.75 MB/mo vs 7.4 MB before. PENDING FIELD
  VERIFICATION: park ≥5 min → flush cadence drops & cell fixes stop; carry
  away → track resumes within ~2 min; overnight indoors → heartbeats only,
  no churn logs. Board reflashed 2026-07-16 (main @ 6b6a84c: arch-review
  F1/F4/F5 refactors + single-owner ephemeris inventory, C2 gate on
  `healthy`; moved log lines now ship module "lte"/"gnss", not "tracker").
- FIELD INCIDENT (open, 2026-07-17 11:36): device went silent mid-ride for
  45+ h — LTE registered, GNSS fixing, main loop provably alive (LED2
  blink→solid needs leds_update), yet zero packets on the wire and the
  carrier saw no data. Server fully exonerated (ingest healthy, tcpdump
  empty). Wedge is in the ship path: publish_allowed stuck, socket setup
  failing forever, or a zombie socket. Board still fielded @ 6b6a84c;
  UART capture on next physical access is the diagnosis path.
- Flight recorder SHIPPED (src/flog: log backend → RAM ring → FCB circular
  log; 8 MB of the DK's 32 MB NOR, 1 MB flash_sim partition on native_sim,
  `flog dump/stats/mark/erase` shell). ~5 days of full-DBG at a realistic
  mix, ~85 days at INF. Sim-verified (`make flogtest`); NOT yet run on
  hardware — the board is in the field; flash + verify on next access.
  Remaining Phase 2: obs spill (24 MB reserved after the 8 MB ring), epoch
  timestamps, uplink extraction of flash logs, spi_nor DPD power tuning,
  uplink liveness watchdog (the incident's real fix).
- A-GNSS LIVE: server pulls IGS broadcast ephemeris (BKG BRDC00WRD_S, free
  anonymous HTTPS, 30 min refresh) and serves POST /agnss; device fetches
  ~1 KB when its inventory is thin and injects via agnss_write. All scaling
  is server-side (server/agnss.py, `make servertest`). Best-effort: server
  down = old behaviour. VM is a clean tracking checkout of origin/main
  (the login-gating commit is upstream since 510d3de); web UI creds live
  in the VM's git-ignored .env.
