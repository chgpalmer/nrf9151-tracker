/**
 * usage-model.js — the tracker's data/holes/latency cost model.
 *
 * One place for the numbers, shared by the Settings estimator (and, later,
 * anything else that reasons about cost). Every constant is CALIBRATED from
 * field measurements and should be re-tuned as the Usage-page ledger
 * accumulates — update the value AND its provenance comment together.
 *
 * The trade triangle: sample interval and flush interval jointly set
 * live latency, GNSS track holes, and data cost. Sessions (radio wakes)
 * dominate both holes and billing, not payload bytes.
 */

export const CAL = {
  // Wire encoding (proto v3, measured 2026-07-11 drive):
  ptBytes: 6,            // delta-coded point, ~5-7 B at 1 Hz driving
  anchorBytes: 30,       // full anchor fix per segment
  segMaxPts: 50,         // fixed pre-encode chunk (MTU discipline)
  datagramOverhead: 72,  // IP+UDP+CoAP framing per datagram

  // Radio sessions (measured 2026-07-12 ride + Velocity billing):
  sessionOverheadB: 800, // RRC signalling + per-session billing rounding
                         // (billed ~2x app-layer arithmetic on quiet days)
  holePerSessionS: 8,    // GNSS blackout per radio session on NB-IoT
                         // (measured 5-16 s; ~fixed, payload-independent)

  // Background (pre quiet-while-parked; refine from the Usage ledger):
  parkedBytesPerDay: 60000, // QUIESCENT checks + flushes + heartbeats
  logsBytesPerDay: 8000,    // INF traffic on a healthy day
};

/**
 * Estimate a configuration's cost.
 * @param {number} sampleS  seconds between GPS points while moving (>= 1)
 * @param {number} flushS   seconds between radio sessions while moving
 * @param {number} movingH  hours of riding per day
 * @returns {{latencyS:number, holePct:number, mbPerMonth:number,
 *            sessionsPerDay:number, ptsPerFlush:number}}
 */
export function estimate(sampleS, flushS, movingH) {
  const fl = Math.max(flushS, sampleS);
  const movingS = movingH * 3600;

  const ptsPerFlush = Math.max(1, Math.round(fl / sampleS));
  const segs = Math.ceil(ptsPerFlush / CAL.segMaxPts);
  const payload = ptsPerFlush * CAL.ptBytes + segs * CAL.anchorBytes;
  const wirePerFlush = payload + segs * CAL.datagramOverhead;

  const sessionsPerDay = movingS / fl;
  const movingBytesPerDay =
    sessionsPerDay * (wirePerFlush + CAL.sessionOverheadB);

  const bytesPerDay =
    movingBytesPerDay + CAL.parkedBytesPerDay + CAL.logsBytesPerDay;

  return {
    latencyS: fl,
    holePct: movingS > 0
      ? 100 * (sessionsPerDay * CAL.holePerSessionS) / movingS : 0,
    mbPerMonth: (bytesPerDay * 30) / 1048576,
    sessionsPerDay,
    ptsPerFlush,
  };
}
