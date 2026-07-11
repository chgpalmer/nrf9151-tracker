# loc_fsm decision table (review of apps/tracker/src/loc/loc_fsm.c)

Reviewed 2026-07-12 against the code as of `defcb04`. This table is the
normative description of the GNSS/LTE arbitration; the ztest suite encodes it
row by row. If code and table disagree, one of them is wrong — fix whichever.

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
| C2 | `lte_chops_gnss` | GNSS_EXCLUSIVE | "LTE chops GNSS" |
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
| E4 | *(removed — see finding F-1)* | | while the fix is current, REPORT_GNSS never exits |

### CELL_LOOP — cell cadence; sight-triggered retry
| # | Condition | Next | |
|---|---|---|---|
| G1 | `fix` | REPORT_GNSS | "fix returned" |
| G2 | `sats_in_view` (one epoch, any strength) | GNSS_ACQUIRE | "satellites sighted" |

## Outputs by state

| State | gnss_wanted | lte_wanted | publish_allowed | prefer_cell | interval |
|---|---|---|---|---|---|
| LTE_ATTACH | no* | yes | no | — | — |
| REPORT_CELL | yes | yes | yes | yes | 1 s |
| GNSS_ACQUIRE | yes | yes | **no** | — | — |
| GNSS_EXCLUSIVE | yes | **no** | **no** | — | — |
| REPORT_GNSS | yes | yes | yes | only if fix stale | 1 s |
| CELL_LOOP | yes | yes | yes | yes | 30 s |

\* `gnss_wanted = lte_registered || EXCLUSIVE`, so GNSS is off while attaching.

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
should build on this table.

**F-3 — `cell_sent` can be satisfied by a stale cell.** The uplink kinds-mask
latches on *any* delivered cell entry, including one queued in a previous
cycle (cell ring holds 8). Intent — "a coarse position reached the server
before GNSS gets the radio" — is arguably still met by a minutes-old cell.
Accepted as-is; noted so the test encodes the actual semantics.

**F-4 — blanked-epoch bookkeeping is correct but subtle.** `sky_empty` counts
only observed epochs; `ephemeris_visible` carries the last observed value
through blanked epochs; `chopped_streak` resets only on an *observed* clean
window. The tests must cover blanked-epoch sequences explicitly — this is
where regressions would be invisible in a happy-path sim.
