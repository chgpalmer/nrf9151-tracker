/**
 * trips.js — segment a day of fixes into trips.
 *
 * Garmin-style rule: a trip is a maximal run of GPS fixes containing no data
 * gap longer than GAP_S and no stationary spell longer than STOP_S. A fix is
 * "moving" when its Doppler speed clears MOVE_MPS or it stepped a real
 * distance from the previous fix (covers cell-noise-free but speed-less
 * points). Runs that are too short or never really went anywhere are noise,
 * not trips.
 *
 * Cell fixes never form trips; they are ambient context.
 */

const GAP_S      = 300;  // data gap that always ends a trip (signal loss, parked)
const STOP_S     = 120;  // stationary longer than this ends the trip
const MOVE_MPS   = 0.8;  // Doppler speed above this counts as moving
const STEP_M     = 10;   // or a positional step above this
const MIN_TRIP_S = 60;   // discard shorter trips
const MIN_DIST_M = 80;   // discard trips that never went anywhere
const TRIP_RADIUS_M = 150; // and must actually GO somewhere: max distance
                           // from the start fix. Parked multipath jitter
                           // accumulates path length without ever leaving
                           // the garden (2026-07-12: 27 fakes <= 102 m,
                           // real rides >= 1565 m); loop rides still range
                           // far out before returning, so this keeps them.

export function haversineM(lat1, lon1, lat2, lon2) {
  const r = 6371000, toRad = Math.PI / 180;
  const dp = (lat2 - lat1) * toRad, dl = (lon2 - lon1) * toRad;
  const a = Math.sin(dp / 2) ** 2 +
    Math.cos(lat1 * toRad) * Math.cos(lat2 * toRad) * Math.sin(dl / 2) ** 2;
  return 2 * r * Math.asin(Math.sqrt(a));
}

/**
 * @param {Array} fixes — a day of fixes, chronological (gps + cell mixed)
 * @returns {Array<{start_ts, end_ts, dist_m, max_spd, n}>}
 */
export function segmentTrips(fixes) {
  const gps = fixes.filter(f => f.source === 'gps');
  if (gps.length < 2) return [];

  // Annotate movement: speed if we have it, positional step as fallback.
  const moving = gps.map((f, i) => {
    if (f.spd != null && f.spd >= MOVE_MPS) return true;
    if (i === 0) return false;
    const p = gps[i - 1];
    return haversineM(p.lat, p.lon, f.lat, f.lon) >= STEP_M;
  });

  const trips = [];
  let start = null;   // index of the trip's first moving fix
  let lastMove = null; // index of the most recent moving fix

  const close = (endIdx) => {
    if (start == null) return;
    const pts = gps.slice(start, endIdx + 1);
    let dist = 0;
    for (let i = 1; i < pts.length; i++) {
      dist += haversineM(pts[i - 1].lat, pts[i - 1].lon, pts[i].lat, pts[i].lon);
    }
    const dur = pts[pts.length - 1].received_ts - pts[0].received_ts;
    let far = 0;
    for (let i = 1; i < pts.length; i++) {
      far = Math.max(far,
        haversineM(pts[0].lat, pts[0].lon, pts[i].lat, pts[i].lon));
    }
    if (dur >= MIN_TRIP_S && dist >= MIN_DIST_M && far >= TRIP_RADIUS_M) {
      trips.push({
        start_ts: pts[0].received_ts,
        end_ts:   pts[pts.length - 1].received_ts,
        dist_m:   dist,
        max_spd:  Math.max(...pts.map(p => p.spd ?? 0)),
        n:        pts.length,
      });
    }
    start = lastMove = null;
  };

  for (let i = 0; i < gps.length; i++) {
    if (start != null) {
      const gap = gps[i].received_ts - gps[i - 1].received_ts;
      const stopped = gps[i].received_ts - gps[lastMove].received_ts;
      if (gap > GAP_S || stopped > STOP_S) {
        close(lastMove); // trim the stationary/gap tail off the trip
      }
    }
    if (moving[i]) {
      if (start == null) start = i;
      lastMove = i;
    }
  }
  close(lastMove ?? -1);

  return trips;
}

export function fmtDistM(m) {
  return m >= 1000 ? `${(m / 1000).toFixed(1)} km` : `${Math.round(m)} m`;
}
