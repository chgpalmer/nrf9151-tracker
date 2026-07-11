# Adaptive cadence / quiescent mode — design

Status: approved in discussion 2026-07-11 (this doc is the written form).
Prerequisite (done): docs/loc-fsm-decision-table.md + tests/fsm.

## 1. Problem and evidence

A tracker that never moves behaves exactly like one that moves constantly:

- Parked in CELL_LOOP it ships a cell fix every 30 s ≈ **7.4 MB/month** —
  most of the 10 MB plan — and the fixes are worthless: a tower centroid
  "updating" a known parked position is negative information (Charlie had to
  make the webapp draw cell and GPS points separately because of exactly
  these blips).
- Parked indoors with no fix it churns ATTACH → ACQUIRE → EXCLUSIVE →
  timeout forever (decision-table finding F-2), narrating ~1 KB of logs per
  ~6-min lap.
- GNSS in continuous tracking draws ~40–45 mA. LTE peaks higher but bursts
  for seconds and idles at a PSM floor of ~µA, so a parked tracker's energy
  budget IS the GNSS receiver: ~2 days on a 2000 mAh cell vs months with
  GNSS duty-cycled. Irrelevant on the USB-fed DK, mandatory for the product.

Key observations that shaped the design:

- Sampling is free; **shipping and cell fixes are the data cost.** GPS points
  are ~5–7 B deltas; the money goes on datagram overhead (47 B each) and
  30 s cell fixes.
- A position **check** is nearly free in power when ephemeris is held: a hot
  start is 1–3 s. The expensive thing is *continuous* tracking. The modem
  has native periodic-navigation mode (`fix_interval > 1`) that duty-cycles
  itself and schedules its own ephemeris upkeep — no hand-rolled stop/start.

## 2. Architecture

Three pieces, split so the FSM keeps owning exactly what it owns today
(radio arbitration) and nothing else:

```
PVT frames ─┬─> motion.c ──stationary──> loc_fsm ──gnss_mode/publish──> main.c
            │   (verdict)                (radio policy + REST/QUIESCENT)
            └────────────────────────────────────────> obs_queue (unchanged)
cell id ────┴─> motion.c                 main.c ──flush interval──> uplink
```

- **`motion.c` (new)**: consumes fixes and serving-cell IDs, owns the
  stationary verdict. Pure, clock-as-parameter, IMU-ready: the interface is
  `motion_update(...) -> bool stationary` and an IMU later replaces its
  internals without touching a consumer.
- **`loc_fsm`**: gains `stationary` as an input (alongside `lte_registered`),
  two new states (QUIESCENT, REST), and its `gnss_wanted` bool output becomes
  a three-valued **`gnss_mode`: CONTINUOUS / PERIODIC / OFF** plus a
  `gnss_interval_s` for PERIODIC. States are modes with distinct policy —
  parked-with-fix and parked-no-fix have distinct radio policy, so they are
  states, per the header's own philosophy.
- **`main.c` glue**: applies gnss_mode edges to the modem (stop → set
  fix_interval → restart), and sets the uplink flush interval from the
  stationary verdict (`uplink_set_flush_interval()`, new one-liner API).

No proto change. No server change. The parked heartbeat is an ordinary cell
fix at slow cadence; the webapp already renders cell points separately.

## 3. Stationary verdict (motion.c)

- **GPS source (primary)**: maintain a parked centroid; stationary when every
  fix over the entry window stays within `MOTION_RADIUS_M` (default 25 m —
  above GPS jitter) of it. Entry hysteresis `MOTION_ENTRY_S` (default 300 s):
  bike-faffing before a ride never triggers quiescence.
- **Exit**: the first out-of-radius fix flips the verdict immediately (waking
  is cheap; bias toward waking), but the **urgent flush** waits for a second
  consecutive out-of-radius fix so a single indoor multipath jump doesn't
  burn a radio wake.
- **Cell source (fallback, no fix available)**: stationary while the serving
  cell stays within the set of recently seen cells (small LRU, default 4) —
  plain equality would see reselection ping-pong at a cell boundary as
  motion. A cell outside the set = moving, and resets REST's backoff.
- Verdict is device-level (one bool), but each source judges only its own
  evidence — a cell blip never overrides a GPS-stationary verdict.

## 4. Modes and cadences

| | GNSS | queue | flush | cell fixes |
|---|---|---|---|---|
| MOVING (today's behaviour) | continuous 1 Hz | every fix | 120 s / segment-full | per current states |
| QUIESCENT (parked, fix held) | periodic, `QUIESCENT_CHECK_S` = 120 | every check fix | `QUIESCENT_FLUSH_S` = 1200 | none |
| REST (parked, no fix) | periodic, backoff 300→600→1200→1800 s cap | — | on heartbeat | heartbeat every `REST_HEARTBEAT_S` = 1200 |

Parked-month math: QUIESCENT ≈ 720 pts + 72 flushes/day ≈ 10 KB/day ≈
**0.3 MB/mo**; REST ≈ 72 heartbeats/day ≈ 5 KB/day ≈ **0.15 MB/mo**. Against
7.4 MB/mo today. Theft detection: QUIESCENT notices displacement on the next
check (≤ 2 min) and urgent-flushes on confirmation; REST notices a cell-set
change (~km granularity) — the honest limit until an IMU provides wake-on-motion.

All defaults are Kconfig symbols (`TRACKER_MOTION_*`, `TRACKER_QUIESCENT_*`,
`TRACKER_REST_*`).

## 5. FSM changes (table + ztest updated in the same commit, rule 4)

New inputs: `stationary` (from motion.c). New evidence: `acquire_failures` —
consecutive ACQUIRE/EXCLUSIVE attempts since the last fix that ended in
timeout or sky-empty; unlike the sky streaks it survives state changes and
resets on fix or cell-set change.

New transitions (numbering continues the table):

- REPORT_GNSS → QUIESCENT: `stationary` (fix held, parked).
- QUIESCENT → REPORT_GNSS: `!stationary` (motion) — resume continuous 1 Hz.
- QUIESCENT → GNSS_ACQUIRE: checks stop producing fixes (staleness measured
  in missed checks, not wall seconds — see below): one real acquisition
  attempt before giving up further.
- CELL_LOOP → REST: `stationary` (cell verdict) and `acquire_failures ≥ 2`.
- REST: the periodic GNSS wake IS the retry (no separate timer — the
  modem's periodic interval is set to the current backoff). A wake that
  fixes outright → REPORT_GNSS; one that sights satellites without fixing →
  GNSS_ACQUIRE (a real attempt, evidence rules active); one that observes
  nothing → stay, double the backoff. A cell-set change → GNSS_ACQUIRE and
  resets the backoff.
- REST obeys the global registration-loss override like everyone else.

Output changes: `gnss_wanted: bool` → `gnss_mode` + `gnss_interval_s`;
QUIESCENT and REST rows added to the outputs table (publish_allowed: yes for
both — QUIESCENT publishes GPS checks, REST publishes heartbeats;
prefer_cell: no / yes).

**Staleness becomes cadence-aware.** `gps_current` today means "fix < 30 s
old", which is nonsense under 120 s checks. Redefine: a fix is current if it
is newer than `N_missed × expected_interval` (N_missed = 3), where
expected_interval is 1 s in continuous mode and the check interval in
periodic mode. The existing GPS_STALE_S semantics are the continuous-mode
special case.

**Evidence counters are continuous-mode-only.** sky_empty / chopped /
hotstart streaks assume 1 Hz epochs; in QUIESCENT and REST the FSM does not
evaluate them (a periodic check either fixes or it doesn't). They resume on
(re)entering the continuous-mode states. The ztest pins this.

## 6. Failure modes considered

- **Cell flapping while parked** → LRU set, not last-ID equality (§3).
- **Multipath jump while parked** → two-sample confirm before urgent flush (§3).
- **Ephemeris lapse while parked** → modem-native periodic mode does its own
  ephemeris upkeep; we do not schedule refreshes (and the FSM no longer has a
  refresh transition — F-1).
- **Stolen from indoors (REST)** → cell-change detection only; accepted until
  IMU. Documented, not solved.
- **GNSS mode-change races** → main.c applies mode edges only (compare with
  last applied), stop/reconfigure/start as one sequence; a failed apply
  retries next tick rather than latching a wrong mode.
- **Flush interval change with queued data** → `uplink_set_flush_interval()`
  re-arms from last flush; shortening it can only flush sooner, never lose data.

## 7. Testing

- **tests/fsm**: new rows (QUIESCENT/REST transitions, acquire_failures
  bookkeeping, cadence-aware staleness, evidence-counters-suspended) — same
  fake-clock style, still milliseconds.
- **motion.c is pure** → second ztest suite in the same test app: feed fix
  sequences (park, jitter, multipath jump, creep, cell flap) and assert the
  verdict — the scenarios from §3/§6 written down.
- **Sim**: mocks stay happy-path; no scenario tests (rejected earlier, stands).
- **Field check**: park the DK ≥ 5 min → server shows flush cadence drop and
  cell fixes stop; carry it away → urgent flush within ~2 checks; overnight
  indoors → REST heartbeats only, no churn logs (compare the logs table
  against a pre-change overnight).

## 8. Rejected alternatives

- **GNSS fully OFF in REST** — saves negligible power beyond periodic mode
  and blinds sky-return detection; becomes a policy tweak once an IMU exists.
- **Hand-rolled GNSS stop/start duty-cycling** — the modem's periodic mode
  does it natively, including ephemeris upkeep.
- **Slow-cadence cell fixes instead of none in QUIESCENT** — a tower centroid
  next to a held GPS position is negative information.
- **IMU-first** — no hardware on the DK; the motion_source seam keeps the
  swap cheap.
- **Binary moving/parked (no REST/QUIESCENT split)** — parked-with-sky and
  parked-no-sky have different radio policies and different exits; merging
  them recreates the churn loop.
