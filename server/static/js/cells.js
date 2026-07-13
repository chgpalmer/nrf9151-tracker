/**
 * cells.js — serving-cell history table in the side panel.
 *
 * Connectivity, not location: which tower, what technology, what signal —
 * the "was there a tower swap there?" view. Rows where the tower CHANGED
 * from the previous sample get a marker; rows the tower DB couldn't place
 * are dimmed (they still prove who the device was talking to).
 */

export function actName(act) {
  return act === 9 ? 'NB-IoT' : act === 7 ? 'LTE-M' : '—';
}

export function createCellsTable(containerId, { onSelect } = {}) {
  const root = document.getElementById(containerId);
  root.classList.add('jtable');
  root.innerHTML = `
    <div class="jtable-scroll">
      <table class="ftable-table jtable-table">
        <thead>
          <tr><th>Time</th><th>PLMN</th><th>Cell</th><th>dBm</th><th>Tech</th></tr>
        </thead>
        <tbody></tbody>
      </table>
    </div>
    <div class="jtable-empty" hidden>No cell reports this day.</div>`;

  const tbody   = root.querySelector('tbody');
  const emptyEl = root.querySelector('.jtable-empty');

  tbody.addEventListener('click', e => {
    const tr = e.target.closest('tr[data-i]');
    if (tr && onSelect) onSelect(parseInt(tr.dataset.i, 10));
  });

  function hhmm(ts) {
    return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  /** @param cells  /api/cells rows, chronological
   *  @param activeI selected row index, or null */
  function render(cells, activeI = null) {
    emptyEl.hidden = cells.length > 0;
    tbody.innerHTML = cells.map((c, i) => {
      const swap  = i > 0 && c.cell_id !== cells[i - 1].cell_id;
      const cls = [
        i === activeI ? 'active' : '',
        swap ? 'cell-swap' : '',
        c.lat == null ? 'cell-unresolved' : '',
      ].join(' ');
      return `
      <tr data-i="${i}" class="${cls}"
          title="TAC 0x${(c.tac ?? 0).toString(16).toUpperCase()}${c.lat == null ? ' — not in tower DB' : ''}">
        <td>${hhmm(c.received_ts)}</td>
        <td>${c.mcc ?? '—'}-${String(c.mnc ?? 0).padStart(2, '0')}</td>
        <td>${c.cell_id ?? '—'}</td>
        <td>${c.rsrp_dbm || '—'}</td>
        <td>${actName(c.act)}</td>
      </tr>`;
    }).join('');
  }

  return { render };
}
