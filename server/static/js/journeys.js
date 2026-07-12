/**
 * journeys.js — the trips table in the side panel.
 *
 * Always-open (the side-panel tab is the toggle). One row per detected
 * trip; clicking a row selects that trip on the map — the table replaced
 * the per-trip chips that used to crowd the controls bar.
 */

import { fmtDistM } from '/js/trips.js';
import { mpsToKph } from '/js/format.js';

export function createJourneysTable(containerId, { onSelect } = {}) {
  const root = document.getElementById(containerId);
  root.classList.add('jtable');
  root.innerHTML = `
    <div class="jtable-scroll">
      <table class="ftable-table jtable-table">
        <thead>
          <tr><th>#</th><th>Start</th><th>Dur</th><th>Dist</th><th>Max</th></tr>
        </thead>
        <tbody></tbody>
      </table>
    </div>
    <div class="jtable-empty" hidden>No journeys detected this day.</div>`;

  const tbody   = root.querySelector('tbody');
  const emptyEl = root.querySelector('.jtable-empty');

  tbody.addEventListener('click', e => {
    const tr = e.target.closest('tr[data-i]');
    if (tr && onSelect) onSelect(parseInt(tr.dataset.i, 10));
  });

  function hhmm(ts) {
    return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  function dur(secs) {
    const m = Math.round(secs / 60);
    return m < 60 ? `${m}m` : `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, '0')}m`;
  }

  /** @param trips  segmentTrips() output
   *  @param activeI index of the selected trip, or null */
  function render(trips, activeI = null) {
    emptyEl.hidden = trips.length > 0;
    tbody.innerHTML = trips.map((t, i) => `
      <tr data-i="${i}" class="${i === activeI ? 'active' : ''}">
        <td>T${i + 1}</td>
        <td>${hhmm(t.start_ts)}</td>
        <td>${dur(t.end_ts - t.start_ts)}</td>
        <td>${fmtDistM(t.dist_m)}</td>
        <td>${t.max_spd ? `${mpsToKph(t.max_spd)} km/h` : '—'}</td>
      </tr>`).join('');
  }

  return { render };
}
