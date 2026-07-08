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
<style>
  body { margin: 0; font-family: sans-serif; }
  #map { height: 100vh; }
  #hud {
    position: absolute; top: 10px; right: 10px; z-index: 1000;
    background: rgba(255,255,255,0.94); padding: 12px 14px;
    border-radius: 6px; font-size: 13px; min-width: 220px;
    box-shadow: 0 2px 8px rgba(0,0,0,0.2);
  }
  #hud h3 { margin: 0 0 8px; font-size: 15px; }
  #hud p  { margin: 3px 0; }
  #hud label { font-size: 11px; color: #666; display: block; margin: 6px 0 2px; }
  #hud select { width: 100%; padding: 3px; }
  .win { display: flex; gap: 4px; margin-top: 4px; flex-wrap: wrap; }
  .win button {
    flex: 1; padding: 4px 0; font-size: 11px; cursor: pointer;
    border: 1px solid #ccc; background: #f5f5f5; border-radius: 4px;
  }
  .win button.active { background: #2563eb; color: #fff; border-color: #2563eb; }
  .muted { color: #666; font-size: 11px; }
</style>
</head>
<body>
<div id="map"></div>
<div id="hud">
  <h3>nRF9151 Tracker</h3>
  <label>Device</label>
  <select id="device"></select>
  <label>Time window</label>
  <div class="win" id="windows"></div>
  <p id="status" class="muted">loading...</p>
  <p id="coords"></p>
  <p id="vel" class="muted"></p>
  <p id="meta" class="muted"></p>
</div>
<script>
const map = L.map('map').setView([51.5074, -0.1278], 13);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '© OpenStreetMap contributors'
}).addTo(map);

let marker = null;
let path = L.polyline([], {color: '#2563eb', weight: 3}).addTo(map);
let centered = false;

const WINDOWS = [
  {label: '15m', min: 15}, {label: '1h', min: 60}, {label: '6h', min: 360},
  {label: '24h', min: 1440}, {label: 'all', min: 0},
];
let sinceMin = 60;  // default 1h

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
  if (!device) {
    document.getElementById('status').textContent = 'no devices yet';
    return;
  }
  try {
    const q = new URLSearchParams({device, limit: 500});
    if (sinceMin > 0) q.set('since', sinceMin);
    const r = await fetch('/api/positions?' + q);
    const pts = await r.json();
    if (!pts.length) {
      document.getElementById('status').textContent = 'no fixes in window';
      path.setLatLngs([]);
      return;
    }

    path.setLatLngs(pts.map(p => [p.lat, p.lon]));

    const last = pts[pts.length - 1];
    const ll = [last.lat, last.lon];
    if (!marker) {
      marker = L.circleMarker(ll, {radius: 8, color: '#2563eb', fillColor: '#60a5fa', fillOpacity: 0.9}).addTo(map);
    } else {
      marker.setLatLng(ll);
    }
    if (!centered) { map.setView(ll, 16); centered = true; }

    document.getElementById('status').textContent = `${pts.length} fix(es)`;
    document.getElementById('coords').textContent = `${last.lat.toFixed(6)}, ${last.lon.toFixed(6)}`;
    document.getElementById('vel').textContent =
      `spd ${last.spd?.toFixed(1) ?? '?'} m/s   hdg ${last.hdg?.toFixed(0) ?? '?'}°`;
    const age = Math.round(Date.now()/1000 - last.received_ts);
    document.getElementById('meta').textContent =
      `acc ±${last.acc ?? '?'}m   sats ${last.sats ?? '?'}   ${age}s ago`;
  } catch(e) {
    document.getElementById('status').textContent = 'error: ' + e.message;
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
    sql = ("SELECT lat, lon, alt, acc, spd, hdg, sats, received_ts "
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
