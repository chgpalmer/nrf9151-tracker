#!/usr/bin/env python3
"""
Tracker web app — FastAPI + Leaflet.

Endpoints:
  GET /               HTML map with device selector + time-window filter
  GET /api/devices    known devices with last_seen + position count
  GET /api/positions  filtered position history for one device

Usage:
  uvicorn app:app --reload --host 0.0.0.0 --port 8080 --app-dir server
"""

import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, JSONResponse

DB_DEFAULT = Path(__file__).parent / "tracker.db"


def get_db(path: Path = DB_DEFAULT) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.db = get_db()
    yield
    app.state.db.close()


app = FastAPI(title="nRF9151 Tracker", lifespan=lifespan)

MAP_HTML = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>nRF9151 Tracker</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
  * { box-sizing: border-box; }
  body { margin: 0; font-family: system-ui, sans-serif; }
  #app { display: flex; height: 100vh; }
  #map { flex: 1; height: 100%; }
  #side {
    width: 340px; height: 100%; overflow-y: auto;
    border-left: 1px solid #ddd; background: #fafafa;
    padding: 12px 14px; font-size: 13px;
  }
  #side h1 { font-size: 16px; margin: 0 0 10px; }
  label { font-size: 11px; color: #666; display: block; margin: 10px 0 3px; }
  select { width: 100%; padding: 4px; }
  .badge {
    display: flex; align-items: center; gap: 8px;
    padding: 10px 12px; border-radius: 8px; margin: 10px 0;
    font-size: 15px; font-weight: 600;
  }
  .badge .sub { font-size: 11px; font-weight: 400; color: #444; }
  .badge.online  { background: #dcfce7; color: #166534; }
  .badge.stale   { background: #fef9c3; color: #854d0e; }
  .badge.offline { background: #fee2e2; color: #991b1b; }
  .badge.none    { background: #eee;    color: #666; }
  .win { display: flex; gap: 4px; margin-top: 4px; flex-wrap: wrap; }
  .win button {
    flex: 1; padding: 4px 0; font-size: 11px; cursor: pointer;
    border: 1px solid #ccc; background: #fff; border-radius: 4px;
  }
  .win button.active { background: #2563eb; color: #fff; border-color: #2563eb; }
  .chart-box { margin-top: 12px; height: 120px; position: relative; }
  table { width: 100%; border-collapse: collapse; margin-top: 6px; font-size: 12px; }
  th, td { text-align: left; padding: 3px 5px; border-bottom: 1px solid #eee; }
  th { color: #666; font-weight: 600; position: sticky; top: 0; background: #fafafa; }
  #tbl-wrap { max-height: 260px; overflow-y: auto; margin-top: 4px;
              border: 1px solid #eee; border-radius: 4px; }
  tbody tr { cursor: pointer; }
  tbody tr:hover { background: #eef4ff; }
  tbody tr.sel { background: #dbeafe; }
  .muted { color: #666; font-size: 11px; }
  .leaflet-popup-content { font-size: 12px; line-height: 1.5; }
</style>
</head>
<body>
<div id="app">
  <div id="map"></div>
  <div id="side">
    <h1>nRF9151 Tracker</h1>
    <label>Device</label>
    <select id="device"></select>
    <div id="badge" class="badge none">— <span class="sub"></span></div>
    <label>Time window</label>
    <div class="win" id="windows"></div>
    <label>Speed (km/h)</label>
    <div class="chart-box"><canvas id="speedChart"></canvas></div>
    <label>Accuracy (m)</label>
    <div class="chart-box"><canvas id="accChart"></canvas></div>
    <label>History (<span id="count">0</span>)</label>
    <div id="tbl-wrap">
      <table>
        <thead><tr><th>Time</th><th>Src</th><th>km/h</th><th>Acc</th><th>Sats</th></tr></thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>
  </div>
</div>
<script>
const COLOR = '#2563eb';
const map = L.map('map').setView([51.5074, -0.1278], 13);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '© OpenStreetMap contributors'
}).addTo(map);

const track = L.polyline([], {color: COLOR, weight: 2, opacity: 0.6}).addTo(map);
const layer = L.layerGroup().addTo(map);   // markers + accuracy circles
let markersById = {};                       // fix id -> marker
let centered = false;
let lastDevice = null;

// ── time window ──
const WINDOWS = [
  {label: '15m', min: 15}, {label: '1h', min: 60}, {label: '6h', min: 360},
  {label: '24h', min: 1440}, {label: 'all', min: 0},
];
let sinceMin = 60;
const winDiv = document.getElementById('windows');
WINDOWS.forEach(w => {
  const b = document.createElement('button');
  b.textContent = w.label;
  if (w.min === sinceMin) b.classList.add('active');
  b.onclick = () => {
    sinceMin = w.min;
    [...winDiv.children].forEach(c => c.classList.remove('active'));
    b.classList.add('active');
    centered = false;
    poll();
  };
  winDiv.appendChild(b);
});

const deviceSel = document.getElementById('device');
deviceSel.onchange = () => { centered = false; poll(); };

// ── charts ──
const chartOpts = {
  responsive: true, maintainAspectRatio: false, animation: false,
  scales: { x: { display: false }, y: { beginAtZero: true } },
  plugins: { legend: { display: false } },
  elements: { point: { radius: 0 } },
};
const speedChart = new Chart(document.getElementById('speedChart'), {
  type: 'line',
  data: { labels: [], datasets: [{ data: [], borderColor: COLOR, borderWidth: 1.5, tension: 0.2 }] },
  options: chartOpts,
});
const accChart = new Chart(document.getElementById('accChart'), {
  type: 'line',
  data: { labels: [], datasets: [{ data: [], borderColor: '#dc2626', borderWidth: 1.5, tension: 0.2 }] },
  options: chartOpts,
});

// ── helpers ──
const kmh = mps => (mps ?? 0) * 3.6;
function fmtTime(ts) {
  return new Date(ts * 1000).toLocaleTimeString();
}
function fmtAge(sec) {
  if (sec < 60) return sec + 's ago';
  if (sec < 3600) return Math.floor(sec / 60) + 'm ago';
  if (sec < 86400) return Math.floor(sec / 3600) + 'h ago';
  return Math.floor(sec / 86400) + 'd ago';
}
function popupHtml(p) {
  const age = Math.round(Date.now()/1000 - p.received_ts);
  const cell = p.source === 'cell';
  return `<b>${new Date(p.received_ts*1000).toLocaleString()}</b><br>`
    + `Source <b>${cell ? 'CELL (approx)' : 'GPS'}</b><br>`
    + `Lat ${p.lat.toFixed(6)} &nbsp; Lon ${p.lon.toFixed(6)}<br>`
    + (cell ? '' : `Alt ${p.alt?.toFixed(1) ?? '?'} m<br>`)
    + `Accuracy ${p.acc?.toFixed(1) ?? '?'} m`
    + (cell ? '' : ` &nbsp; Speed ${kmh(p.spd).toFixed(1)} km/h`)
    + `<br>`
    + (cell ? '' : `Heading ${p.hdg?.toFixed(0) ?? '?'}° &nbsp; Sats ${p.sats ?? '?'}<br>`)
    + `Age ${fmtAge(age)}`;
}

function setBadge(pts) {
  const el = document.getElementById('badge');
  if (!pts.length) { el.className = 'badge none'; el.innerHTML = '— <span class="sub"></span>'; return; }
  const newest = pts[pts.length - 1].received_ts;
  const age = Math.round(Date.now()/1000 - newest);
  let cls, txt;
  if (age < 30)      { cls = 'online';  txt = '🟢 Online'; }
  else if (age < 300){ cls = 'stale';   txt = '🟡 Stale'; }
  else               { cls = 'offline'; txt = '🔴 Offline'; }
  el.className = 'badge ' + cls;
  el.innerHTML = `${txt} <span class="sub">last seen ${fmtAge(age)}</span>`;
}

function focusFix(p) {
  map.setView([p.lat, p.lon], Math.max(map.getZoom(), 17));
  const m = markersById[p.id];
  if (m) m.openPopup();
  document.querySelectorAll('#tbody tr').forEach(tr =>
    tr.classList.toggle('sel', Number(tr.dataset.id) === p.id));
}

const CELL_COLOR = '#d97706';  // amber for coarse cell-based fixes

function rebuildMap(pts) {
  layer.clearLayers();
  markersById = {};
  track.setLatLngs(pts.map(p => [p.lat, p.lon]));
  pts.forEach((p, i) => {
    const isLast = i === pts.length - 1;
    const cell = p.source === 'cell';
    const c = cell ? CELL_COLOR : COLOR;
    if (p.acc) {
      L.circle([p.lat, p.lon], {
        radius: p.acc, color: c, weight: 1, opacity: 0.3,
        fillColor: c, fillOpacity: 0.07,
      }).addTo(layer);
    }
    const m = L.circleMarker([p.lat, p.lon], {
      radius: isLast ? 7 : 4,
      color: c, weight: isLast ? 3 : 1,
      fillColor: isLast ? (cell ? '#fbbf24' : '#60a5fa') : '#fff',
      fillOpacity: 0.9,
    }).bindPopup(popupHtml(p)).addTo(layer);
    markersById[p.id] = m;
  });
}

function rebuildCharts(pts) {
  const labels = pts.map(p => fmtTime(p.received_ts));
  speedChart.data.labels = labels;
  speedChart.data.datasets[0].data = pts.map(p => +kmh(p.spd).toFixed(1));
  speedChart.update();
  accChart.data.labels = labels;
  accChart.data.datasets[0].data = pts.map(p => p.acc ?? null);
  accChart.update();
}

function rebuildTable(pts) {
  document.getElementById('count').textContent = pts.length;
  const tb = document.getElementById('tbody');
  tb.innerHTML = '';
  // newest first
  for (let i = pts.length - 1; i >= 0; i--) {
    const p = pts[i];
    const tr = document.createElement('tr');
    tr.dataset.id = p.id;
    const cell = p.source === 'cell';
    tr.innerHTML = `<td>${fmtTime(p.received_ts)}</td>`
      + `<td style="color:${cell ? '#d97706' : '#2563eb'}">${cell ? 'cell' : 'gps'}</td>`
      + `<td>${cell ? '—' : kmh(p.spd).toFixed(1)}</td>`
      + `<td>${p.acc?.toFixed(0) ?? '?'}</td>`
      + `<td>${cell ? '—' : (p.sats ?? '?')}</td>`;
    tr.onclick = () => focusFix(p);
    tb.appendChild(tr);
  }
}

async function loadDevices() {
  try {
    const r = await fetch('/api/devices');
    const devs = await r.json();
    const cur = deviceSel.value;
    deviceSel.innerHTML = '';
    devs.forEach(d => {
      const o = document.createElement('option');
      o.value = d.device_id;
      o.textContent = `${d.device_id} (${d.n})`;
      deviceSel.appendChild(o);
    });
    if (cur) deviceSel.value = cur;
    if (!deviceSel.value && devs.length) deviceSel.value = devs[0].device_id;
  } catch(e) { /* ignore */ }
}

async function poll() {
  const device = deviceSel.value;
  if (device !== lastDevice) { centered = false; lastDevice = device; }
  if (!device) { setBadge([]); return; }
  try {
    const q = new URLSearchParams({device, limit: 500});
    if (sinceMin > 0) q.set('since', sinceMin);
    const r = await fetch('/api/positions?' + q);
    const pts = await r.json();

    setBadge(pts);
    rebuildMap(pts);
    rebuildCharts(pts);
    rebuildTable(pts);

    if (pts.length && !centered) {
      const last = pts[pts.length - 1];
      map.setView([last.lat, last.lon], 16);
      centered = true;
    }
  } catch(e) {
    document.getElementById('badge').innerHTML = 'error: ' + e.message;
  }
}

async function tick() { await loadDevices(); await poll(); }
tick();
setInterval(tick, 5000);
</script>
</body>
</html>
"""


@app.get("/", response_class=HTMLResponse)
async def index():
    return MAP_HTML


@app.get("/api/devices")
async def devices():
    db = app.state.db
    rows = db.execute(
        "SELECT device_id, MAX(received_ts) AS last_seen, COUNT(*) AS n "
        "FROM positions GROUP BY device_id ORDER BY last_seen DESC"
    ).fetchall()
    return JSONResponse([dict(r) for r in rows])


@app.get("/api/positions")
async def positions(device: str, since: int = 0, limit: int = 500):
    db = app.state.db
    params = [device]
    sql = ("SELECT id, source, lat, lon, alt, acc, spd, hdg, sats, received_ts "
           "FROM positions WHERE device_id = ?")
    if since > 0:
        sql += " AND received_ts >= ?"
        params.append(int(time.time()) - since * 60)
    sql += " ORDER BY received_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.execute(sql, params).fetchall()
    return JSONResponse([dict(r) for r in reversed(rows)])


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8080, reload=False,
                app_dir=str(Path(__file__).parent))
