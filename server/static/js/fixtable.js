/**
 * fixtable.js — collapsible locations table shown under the map.
 *
 * Newest fix first. Clicking a row hands the fix back to the page module
 * (which pans the map and opens the detail drawer). The table only builds
 * DOM while it is open; while collapsed, render() just updates the count.
 */

import { fmtTime, fmtAcc, mpsToKph, fmtCoord } from '/js/format.js';

const MAX_ROWS = 500;

export function createFixTable(containerId, { onRowClick, onRowHover, startOpen = false } = {}) {
  const root = document.getElementById(containerId);
  root.classList.add('ftable');
  root.innerHTML = `
    <button class="ftable-head" type="button" aria-expanded="${startOpen}">
      <span class="ftable-title">Locations</span>
      <span class="ftable-count" data-role="count"></span>
      <span class="ftable-chevron">▾</span>
    </button>
    <div class="ftable-scroll">
      <table class="ftable-table">
        <thead>
          <tr><th>Time</th><th>Src</th><th>Lat</th><th>Lon</th><th>Acc</th><th>Speed</th><th>Sats</th></tr>
        </thead>
        <tbody></tbody>
      </table>
    </div>`;

  const head    = root.querySelector('.ftable-head');
  const countEl = root.querySelector('[data-role="count"]');
  const tbody   = root.querySelector('tbody');

  let open  = startOpen;
  let fixes = [];
  let dirty = false;

  root.classList.toggle('open', open);

  head.addEventListener('click', () => {
    open = !open;
    root.classList.toggle('open', open);
    head.setAttribute('aria-expanded', String(open));
    if (open && dirty) renderBody();
  });

  tbody.addEventListener('click', e => {
    const tr = e.target.closest('tr[data-i]');
    if (!tr) return;
    const fix = fixes[parseInt(tr.dataset.i, 10)];
    if (fix && onRowClick) onRowClick(fix);
  });

  if (onRowHover) {
    tbody.addEventListener('mouseover', e => {
      const tr = e.target.closest('tr[data-i]');
      onRowHover(tr ? fixes[parseInt(tr.dataset.i, 10)] : null);
    });
    tbody.addEventListener('mouseleave', () => onRowHover(null));
  }

  function renderBody() {
    dirty = false;
    const rows = [];
    // newest first; fixes arrive oldest-first from the API
    const n = Math.min(fixes.length, MAX_ROWS);
    for (let k = 0; k < n; k++) {
      const i = fixes.length - 1 - k;
      const f = fixes[i];
      const isGps = f.source === 'gps';
      const kph = mpsToKph(f.spd);
      rows.push(`<tr data-i="${i}">
        <td>${fmtTime(f.received_ts)}</td>
        <td><span class="ftable-src ${isGps ? 'gps' : 'cell'}">${isGps ? '● GPS' : '◆ CELL'}</span></td>
        <td>${fmtCoord(f.lat)}</td>
        <td>${fmtCoord(f.lon)}</td>
        <td>${fmtAcc(f.acc)}</td>
        <td>${kph != null ? `${kph} km/h` : '—'}</td>
        <td>${f.sats != null ? f.sats : '—'}</td>
      </tr>`);
    }
    tbody.innerHTML = rows.join('');
  }

  function render(newFixes) {
    fixes = newFixes || [];
    countEl.textContent = fixes.length === 0 ? 'no fixes'
      : fixes.length > MAX_ROWS ? `${fixes.length} fixes (showing ${MAX_ROWS})`
      : `${fixes.length} fix${fixes.length !== 1 ? 'es' : ''}`;
    if (open) renderBody();
    else dirty = true;
  }

  return { render };
}
