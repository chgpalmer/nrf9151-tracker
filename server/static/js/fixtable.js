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

  /* Render a MAX_ROWS window of the (newest-first) table. By default the
   * newest rows; pass an index to center the window on that fix so
   * cross-view selection can land on days with thousands of fixes. */
  let windowStartK = 0;

  function renderBody(centerI = null) {
    dirty = false;
    if (centerI != null) {
      const kFix = fixes.length - 1 - centerI;
      windowStartK = Math.max(0, Math.min(kFix - (MAX_ROWS >> 1),
                                          fixes.length - MAX_ROWS));
    }
    const rows = [];
    // newest first; fixes arrive oldest-first from the API
    const n = Math.min(fixes.length - windowStartK, MAX_ROWS);
    for (let k = windowStartK; k < windowStartK + n; k++) {
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
    windowStartK = 0; // new data: window back to the newest rows
    countEl.textContent = fixes.length === 0 ? 'no fixes'
      : fixes.length > MAX_ROWS ? `${fixes.length} fixes (showing ${MAX_ROWS})`
      : `${fixes.length} fix${fixes.length !== 1 ? 'es' : ''}`;
    if (open) renderBody();
    else dirty = true;
  }

  function setOpen(v) {
    if (open === v) return;
    open = v;
    root.classList.toggle('open', open);
    head.setAttribute('aria-expanded', String(open));
    if (open && dirty) renderBody();
  }

  /* Mark the row for this fix as selected and scroll it into view (opening
   * the table if needed) — the table's half of cross-view selection sync. */
  function highlight(fix) {
    setOpen(true);
    if (dirty) renderBody();
    tbody.querySelectorAll('tr.sel').forEach(tr => tr.classList.remove('sel'));
    const i = fixes.indexOf(fix);
    if (i < 0) return;
    let tr = tbody.querySelector(`tr[data-i="${i}"]`);
    if (!tr) {
      // Selected fix is outside the rendered window (dense day):
      // re-window the table around it.
      renderBody(i);
      tr = tbody.querySelector(`tr[data-i="${i}"]`);
    }
    if (tr) {
      tr.classList.add('sel');
      tr.scrollIntoView({ block: 'nearest' });
    }
  }

  return { render, setOpen, highlight };
}
