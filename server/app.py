#!/usr/bin/env python3
"""
Tracker web app — FastAPI + Leaflet.

Endpoints:
  GET /              HTML map (Leaflet, no external JS deps at runtime)
  GET /api/locations last N location fixes as JSON
  GET /api/status    row counts and latest received timestamp

Usage:
  uvicorn server.app:app --reload --host 0.0.0.0 --port 8080
  or: python3 server/app.py
"""

import json
import sqlite3
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, JSONResponse

DB_DEFAULT = Path(__file__).parent / "location.db"


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
    background: rgba(255,255,255,0.92); padding: 10px 14px;
    border-radius: 6px; font-size: 13px; min-width: 200px;
    box-shadow: 0 2px 8px rgba(0,0,0,0.2);
  }
  #hud h3 { margin: 0 0 6px; font-size: 15px; }
  #hud p  { margin: 2px 0; }
  .acc { color: #666; font-size: 11px; }
</style>
</head>
<body>
<div id="map"></div>
<div id="hud">
  <h3>nRF9151 Tracker</h3>
  <p id="status">connecting...</p>
  <p id="coords"></p>
  <p id="ts" class="acc"></p>
  <p id="acc" class="acc"></p>
</div>
<script>
const map = L.map('map').setView([51.5074, -0.1278], 14);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '© OpenStreetMap contributors'
}).addTo(map);

let marker = null;
let path = L.polyline([], {color: '#2563eb', weight: 3}).addTo(map);
let firstFix = true;

async function poll() {
  try {
    const r = await fetch('/api/locations?limit=200');
    const locs = await r.json();
    if (!locs.length) {
      document.getElementById('status').textContent = 'waiting for fix...';
      return;
    }

    // Draw full path
    const pts = locs.map(l => [l.lat, l.lon]);
    path.setLatLngs(pts);

    // Latest fix
    const latest = locs[locs.length - 1];
    const ll = [latest.lat, latest.lon];

    if (!marker) {
      marker = L.circleMarker(ll, {radius: 8, color: '#2563eb', fillColor: '#60a5fa', fillOpacity: 0.9}).addTo(map);
    } else {
      marker.setLatLng(ll);
    }

    if (firstFix) { map.setView(ll, 16); firstFix = false; }

    document.getElementById('status').textContent = `${locs.length} fix(es)`;
    document.getElementById('coords').textContent = `${latest.lat.toFixed(6)}, ${latest.lon.toFixed(6)}`;
    document.getElementById('ts').textContent = latest.ts || latest.received;
    document.getElementById('acc').textContent = latest.acc != null ? `acc ±${latest.acc}m  sats ${latest.sats_used}` : '';
  } catch(e) {
    document.getElementById('status').textContent = 'error: ' + e.message;
  }
}

poll();
setInterval(poll, 5000);
</script>
</body>
</html>
"""


@app.get("/", response_class=HTMLResponse)
async def index():
    return MAP_HTML


@app.get("/api/locations")
async def locations(limit: int = 200):
    db = app.state.db
    rows = db.execute(
        "SELECT lat, lon, alt, acc, sats_used, ts, received FROM location "
        "WHERE is_fix = 1 ORDER BY id DESC LIMIT ?",
        (limit,),
    ).fetchall()
    # Return chronological order
    return JSONResponse([dict(r) for r in reversed(rows)])


@app.get("/api/status")
async def status():
    db = app.state.db
    total = db.execute("SELECT COUNT(*) FROM location").fetchone()[0]
    fixes = db.execute("SELECT COUNT(*) FROM location WHERE is_fix=1").fetchone()[0]
    latest = db.execute(
        "SELECT received FROM location ORDER BY id DESC LIMIT 1"
    ).fetchone()
    return {
        "total_messages": total,
        "fixes": fixes,
        "latest_received": latest[0] if latest else None,
    }


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8080, reload=False, app_dir=str(Path(__file__).parent))
