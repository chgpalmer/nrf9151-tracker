# loc_fsm decision table (review of apps/tracker/src/loc/loc_fsm.c)

Reviewed 2026-07-12 against the code as of `defcb04`. This table is the
normative description of the GNSS/LTE arbitration; the ztest suite encodes it
row by row. If code and table disagree, one of them is wrong — fix whichever.

2026-07-16: the ephemeris inventory became an explicit input — `struct
ephe_inventory` (loc_fsm.h), digested once per pass by gnss_ctrl from the
modem's expiry report and passed into both `loc_fsm_update` and `agnss_poll`,
so the C2 gate and the fetch trigger provably read the same snapshot. The FSM
makes no modem calls.

## Evidence (inputs computed every 1 Hz tick)

| Signal | Definition | Notes |
|---|---|---|
| `fix` | PVT frame has FIX_VALID | instantaneous, per-epoch |
| `gps_current` | last fix < 30 s ago (`GPS_STALE_S`) | the "we know where we are" bit |
| `lte_registered` | from LTE handler | |
| `cell_sent` | latched when uplink confirms a datagram containing a cell entry | cleared on entering REPORT_CELL |
| `observed_window` | PVT flags have neither DEADLINE_MISSED nor NOT_ENOUGH_WINDOW_TIME | a blanked epoch is evidence of *nothing* |
| `sats_in_view` | tracked > 0 this epoch | one satellite is enough to justify trying |
| `sky_empty` | ≥ 5 consecutive **observed** epochs with tracked == 0 (`SKY_EMPTY_EPOCHS`) | blanked epochs pause the count |
| `lte_chops_gnss` | ≥ 10 consecutive epochs with NOT_ENOUGH_WINDOW_TIME (`CHOPPED_EPOCHS`) | modem's own testimony that idle-mode LTE is slicing GNSS |
| `hotstart_dead` | > 10 s (`VISIBLE_LOST_MS`) since an observed epoch had ≥ 4 ephemeris-bearing satellites in view | visible ≠ held: held ephemerides may have set |
| `stationary` | motion.c verdict (GPS centroid / cell-set LRU / IMU interrupt) | an input, like `lte_registered`; consumed by PARKED. `motion_note_imu()` (any-motion interrupt) flips it false instantly through both dwell verdicts |
| `acquire_failures` | consecutive ACQUIRE/EXCLUSIVE give-ups (timeout or sky-empty) since the last fix | SURVIVES state changes; reset by any fix, and by leaving PARKED on motion |
| `agnss_supply` | main.c reports `agnss_supply_ok()` each pass: assistance compiled in, enabled, and not fetch-locked-out | an input, like `stationary`; defaults false (no supply). Goes false after 5 consecutive fetch failures (agnss lockout) and true again on the next success — the C2 gate inherits that hysteresis for free |
| `held` | inventory SVs with ANY ephemeris validity left (`ephe_inventory.held`) | enough for a hot start, even 10 min; GPS only |
| `healthy` | inventory SVs with ≥ 30 min validity left (`ephe_inventory.healthy`, `TRACKER_AGNSS_MIN_LEFT_MIN`) | worth counting when deciding whether to fetch; drives the fetch trigger (< 8) and the ACQUIRE fetch unlock (< 4) |
| `cold` | `healthy < 4` (`SATS_FOR_FIX`) | the inventory cannot support a fix worth going dark for. Judged on `healthy`, not `held` (changed 2026-07-16): held ephemerides may belong to satellites that set hours ago or expire mid-hunt, and the gate must not bless a radio-dark hunt on such stock while a live supply could replace it — same verdict the fetch trigger uses, same snapshot. Distinct from `hotstart_dead` (visibility) |
| `imu_wake` | boot verdict: `imu_init()` succeeded (LIS3DH present and armed) | a MODE, not a per-tick signal (`loc_fsm_set_imu_wake`). Selects PARKED's policy; trusted absolutely — no runtime health checks (decided 2026-07-14) |
| `parked_had_fix` | did PARKED get entered holding a fix? | internal flag selecting the legacy flavor (checks vs backoff); flips true if a check fixes. Meaningless in IMU mode |

Streaks and `last_visible_ok` reset on every state change: each state judges
the sky on its own evidence.

## Global override

| Condition | Action | Why |
|---|---|---|
| `!lte_registered` and state ∉ {LTE_ATTACH, GNSS_EXCLUSIVE} | → LTE_ATTACH ("registration lost") | an unregistered modem cell-searches continuously, which starves GNSS and is starved by it; EXCLUSIVE is exempt because losing registration there is deliberate |

## Transitions (first matching row wins, top to bottom)

### LTE_ATTACH — get registered; GNSS off
| # | Condition | Next | Reason string |
|---|---|---|---|
| A1 | `lte_registered` | REPORT_CELL (clears `cell_sent`) | "registered" |

### REPORT_CELL — coarse position out first
| # | Condition | Next | |
|---|---|---|---|
| B1 | `fix` | REPORT_GNSS | "fix acquired" |
| B2 | `cell_sent` | GNSS_ACQUIRE | "cell reported" |

### GNSS_ACQUIRE — quiet windows, let it try
| # | Condition | Next | |
|---|---|---|---|
| C1 | `fix` | REPORT_GNSS | "fix acquired" |
| C2 | `lte_chops_gnss` and `!stationary` and not (`agnss_supply` and `cold`) | GNSS_EXCLUSIVE | "LTE chops GNSS" — parked trackers never escalate: no urgent fix need, and each escalation costs an LTE deactivate + ~35 s re-attach (7 pointless cycles measured in one parked night); parked no-fix resolves via CELL_LOOP → REST. Cold-with-supply never escalates either: LTE *is* the ephemeris supply line, and going dark trades a seconds-scale fetch for minutes of 50 bps demodulation (measured twice: 2026-07-12 ride and 2026-07-13 commute, 10–11 min blind each — one boot-time response lost to a −109 dBm cell, then EXCLUSIVE cut the supply while driving toward towers where the same fetch took 2 s). While held here, agnss_poll retries on its cold cadence (15 s) and may fetch from ACQUIRE; C3's timeout still bounds the stay, and a supply lockout flips `agnss_supply` false, restoring the escalation as the genuine no-server fallback |
| C3 | in-state > 300 s (`ACQUIRE_TIMEOUT`) | CELL_LOOP | "acquire timeout" |
| C4 | in-state > 30 s (`SKY_GRACE`) and `sky_empty` | CELL_LOOP | "sky empty" |

### GNSS_EXCLUSIVE — LTE deactivated outright
| # | Condition | Next | |
|---|---|---|---|
| D1 | `fix` | REPORT_GNSS | "fix acquired" |
| D2 | in-state > 300 s | CELL_LOOP | "acquire timeout" |
| D3 | in-state > 30 s and `sky_empty` | CELL_LOOP | "sky empty" |

Every exit re-enables LTE; a re-attach (~30–90 s) follows, during which the
override typically bounces CELL_LOOP → LTE_ATTACH within seconds (see F-2).

### REPORT_GNSS — fix held; publish GPS
| # | Condition | Next | |
|---|---|---|---|
| E1 | `!gps_current` and `sky_empty` | CELL_LOOP | "sky lost" |
| E2 | `!gps_current` and `hotstart_dead` | GNSS_ACQUIRE | "fix stale, ephemeris not visible" |
| E3 | `!gps_current` (hot start still viable) | stay | wait for re-fix |
| E4 | *(removed — see finding F-1)* | | the old ephemeris-refresh exit |
| E4' | `gps_current` and `stationary` | QUIESCENT (had_fix=true) | "stationary" |

### QUIESCENT — stationary; the old QUIESCENT+REST merged 2026-07-14 (IMU integration)

**IMU mode** (`imu_wake`): GNSS is OFF outright — no periodic checks, no
scheduled downloads, no parked fixes, which kills the parked-phantom
fake-trip class structurally. The accelerometer owns departure (its
interrupt flips `stationary` via motion.c). The only traffic is the
heartbeat cell fix every REST_HEARTBEAT_S. GNSS-evidence rows below are
skipped entirely: with the receiver stopped, PVT inputs are a frozen frame
(main.c additionally feeds the FSM a blanked frame while GNSS is off).
State named QUIESCENT (not PARKED): the tracker may ride a bike, a cat,
or a suitcase — Charlie's call 2026-07-14.

| # | Condition (IMU mode) | Next | |
|---|---|---|---|
| PI1 | `!stationary` | REPORT_CELL (resets failures, clears cell_sent) | "IMU wake" — LTE FIRST: the server must learn of the movement (alert event + free coarse cell fix) and the A-GNSS fetch must run before the radio-silent acquisition. The wake path IS the boot path (REPORT_CELL → ACQUIRE on cell_sent). main.c raises a MotionEvent on this edge (uplink prio 0, urgent flush) |
| PI2 | otherwise | stay | armed and quiet |

**Legacy mode** (no IMU): the old split behavior, flavored by
`parked_had_fix`:

| # | Condition (legacy) | Next | |
|---|---|---|---|
| P1 | `!stationary` and `gps_current` | REPORT_GNSS | "motion" |
| P1' | `!stationary` | GNSS_ACQUIRE (resets failures) | "motion" |
| P2 | `fix` | stay, `had_fix := true` | flavor upgrades IN PLACE (the old REST bounced through REPORT_GNSS and back — two transitions narrating nothing) |
| P3 | `had_fix` and fix older than 3 check intervals | GNSS_ACQUIRE | "checks not fixing" |
| P4 | `!had_fix` and `sats_in_view` | GNSS_ACQUIRE | "satellites sighted" |
| P5 | `!had_fix` and empty observed check, backoff elapsed | stay | doubles the backoff (once per wake) |

Sky-evidence streaks are 1 Hz bookkeeping and are NOT consulted here.
Staleness is cadence-aware in the had_fix flavor: `gps_current` means "fix
newer than 3 × QUIESCENT_CHECK_S", not 30 s.

### CELL_LOOP — cell cadence; sight-triggered retry
| # | Condition | Next | |
|---|---|---|---|
| G1 | `fix` | REPORT_GNSS | "fix returned" |
| G2 | `stationary` and `acquire_failures ≥ 2` | QUIESCENT (had_fix=false, backoff reset) | "stationary, repeated failures" |
| G3 | `sats_in_view` (one epoch, any strength) | GNSS_ACQUIRE | "satellites sighted" |

G2 before G3 on purpose: once parked-and-failing, a teasing satellite must
not restart the churn — QUIESCENT's own sighting row still reacts, but at
the backoff cadence.

## Outputs by state

| State | gnss_mode | lte_wanted | publish_allowed | prefer_cell | interval |
|---|---|---|---|---|---|
| LTE_ATTACH | OFF* | yes | no | — | — |
| REPORT_CELL | CONT | yes | yes | yes | 1 s |
| GNSS_ACQUIRE | CONT | yes | **no** | — | — |
| GNSS_EXCLUSIVE | CONT | **no** | **no** | — | — |
| REPORT_GNSS | CONT | yes | yes | only if fix stale | 1 s |
| CELL_LOOP | CONT | yes | yes | yes | 30 s |
| QUIESCENT (IMU mode) | **OFF** | yes | yes | yes (after 30 s staleness) | heartbeat interval |
| QUIESCENT (legacy, had_fix) | PERIODIC(check) | yes | yes | no (yes if stale) | check interval |
| QUIESCENT (legacy, no fix) | PERIODIC(backoff) | yes | yes | yes | heartbeat interval |

\* gnss_mode is OFF whenever `!lte_registered`, except in EXCLUSIVE where
being unregistered is deliberate — so GNSS is off while attaching.

`agnss_fetch_allowed` (output, 2026-07-16 — moved here from agnss.c's
private state-allowlist): true in REPORT_CELL / REPORT_GNSS / CELL_LOOP
(publishing states — LTE is already awake), and in GNSS_ACQUIRE while
`healthy < 4` (the fetch IS the fast path to having anything to acquire
with, pairing with the C2 gate). Never in EXCLUSIVE (radio silence is the
point) or QUIESCENT (radio sleep is the point). agnss_poll consumes it and
keeps only fetch execution: enabled/pacing/lockout and the need verdict
(`healthy < 8` or missing time/position).

## Findings

**F-1 — BUG (fixed by deleting E4): the ephemeris-refresh transition
ping-ponged at 1 Hz.** E4 fired while the fix was *current*; but
GNSS_ACQUIRE's first rule (C1) is `fix → REPORT_GNSS`, and under open sky the
very next PVT is fix-valid. So the machine oscillated REPORT_GNSS → ACQUIRE →
REPORT_GNSS every second for up to 15 minutes until the ephemeris expiry
crossed the threshold — logging TWO INF transition lines per second, which
the log uplink would ship: an instant data-plan fire. Never seen in the field
only because no session had yet held a fix for ~1¾ h+. It also could not
achieve its goal: a 1 s round trip creates no quiet window — and its target
state clears `publish_allowed`, so it silenced live tracking while holding a
good fix. Root cause: E4 predates 1 Hz batching — at the old
publish-every-10 s cadence a deliberate quiet window was needed; with uplink
flushing every ~50–120 s, tracking gaps are already quiet and the receiver
re-demodulates ephemeris during normal tracking (subframes repeat every
30 s). A genuinely lapsed ephemeris recovers via E2. **Resolution: E4
deleted** (a `!fix` gate was considered and rejected — it only moves the
bounce onto momentary gap epochs); `TRACKER_LOC_EPHE_REFRESH_MIN` removed.
The ztest pins the fixed behaviour: current fix + expiring ephemeris stays
in REPORT_GNSS.

**F-2 — the machine has no rest state (the parked-cost root cause).** After a
failed EXCLUSIVE, the sequence is CELL_LOOP →(seconds, via override) →
LTE_ATTACH → re-attach → REPORT_CELL → ACQUIRE → chopped → EXCLUSIVE →
timeout → … a ~6–7 min churn cycle, forever, narrating ~1 KB of logs per lap
— and if it *does* settle in CELL_LOOP, G2 re-enters ACQUIRE on a single weak
satellite with no rate limit, and the 30 s cell cadence ships ~7.4 MB/mo.
Nothing here is individually wrong; what's missing is backoff/quiescence for
"stationary and repeatedly failing". This is the adaptive-cadence work; it
should build on this table. **Resolved:** the parked states (merged into
one QUIESCENT state, 2026-07-14) — see that section. The 30 s CELL_LOOP cadence now
only runs while genuinely between states, not as a parking orbit.

**F-3 — `cell_sent` can be satisfied by a stale cell.** The uplink kinds-mask
latches on *any* delivered cell entry, including one queued in a previous
cycle (cell ring holds 8). Intent — "a coarse position reached the server
before GNSS gets the radio" — is arguably still met by a minutes-old cell.
Accepted as-is; noted so the test encodes the actual semantics.

**F-5 — OPEN QUESTION: registration loss mid-tracking turns GNSS off.**
The global override sends every state to LTE_ATTACH on registration loss,
and LTE_ATTACH's output is gnss_mode=OFF — designed for boot (a searching
modem starves a cold GNSS; nothing to upload anyway). But field data
(2026-07-12, 09:23: registered → network drop → 7 min re-registration)
shows mid-session losses can be long, and with a HELD fix that rule throws
away tracking the 34-min obs ring could have buffered and shipped after
re-attach. Candidate change: `!lte_registered && gps_current` keeps GNSS
tracking (a warm tracker is more chop-tolerant than a cold acquirer) and
buffers. Needs a hardware experiment: does tracking survive a searching
modem? Discussion pending — do not implement without it.

**F-6 — OPEN: indoors with a "moving" verdict churns (2026-07-12 field).**
Bag carried inside a building: a jitter wake set stationary=false, indoor
sky teased acquisition, and with !stationary the C2 EXCLUSIVE gate reopened
— ~12 min of hunt (two EXCLUSIVE laps) before the dwell completed. Neither
parked state can catch it: the fix flavor needs fixes, the no-fix entry
needs a stationary verdict, and the GPS verdict blocks the cell verdict for GPS_HOLD (10 min)
after the last fix. Candidate: let the cell-set verdict reclaim authority
faster once GPS goes dark, or add dwell credit from the pre-wake parked
spot. Discussion pending — self-resolves today at ~3 KB per episode.

**F-4 — blanked-epoch bookkeeping is correct but subtle.** `sky_empty` counts
only observed epochs; `ephemeris_visible` carries the last observed value
through blanked epochs; `chopped_streak` resets only on an *observed* clean
window. The tests must cover blanked-epoch sequences explicitly — this is
where regressions would be invisible in a happy-path sim.
