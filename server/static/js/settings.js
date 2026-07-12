/**
 * settings.js — the Settings page's live trade-off estimator.
 *
 * The sliders don't control the device yet (that waits for an authenticated
 * downlink); they drive the SAME cost model we plan with (usage-model.js),
 * so the page doubles as the design calculator: drag "live view" toward
 * realtime and watch the data/holes price of watching a friend borrow the
 * bike. Device's current build values are marked on the sliders.
 */

import { estimate } from '/js/usage-model.js';

const $ = (id) => document.getElementById(id);

function fmtInterval(s) {
  return s >= 60 ? `${(s / 60).toFixed(s % 60 ? 1 : 0)} min` : `${s} s`;
}

function recalc() {
  const sampleS = Number($('set-sample').value);
  const flushS = Number($('set-flush').value);
  const movingH = Number($('set-riding').value);

  $('set-sample-val').textContent =
    sampleS === 1 ? '1 s (1 Hz)' : fmtInterval(sampleS);
  $('set-flush-val').textContent = fmtInterval(flushS);
  $('set-riding-val').textContent = `${movingH} h/day`;

  const e = estimate(sampleS, flushS, movingH);
  $('est-latency').textContent = fmtInterval(e.latencyS);
  $('est-holes').textContent = `${e.holePct.toFixed(1)}% of ride time`;
  $('est-data').textContent = `${e.mbPerMonth.toFixed(1)} MB/mo`;
  $('est-detail').textContent =
    `${Math.round(e.sessionsPerDay)} radio sessions/day · ` +
    `${e.ptsPerFlush} points per flush · ` +
    `10 MB plan: ${e.mbPerMonth > 10 ? 'OVER' : 'ok'}`;
  $('est-data').classList.toggle('over', e.mbPerMonth > 10);
}

export function init() {
  if (!$('set-sample')) return;
  ['set-sample', 'set-flush', 'set-riding'].forEach((id) =>
    $(id).addEventListener('input', recalc));
  recalc();
}
