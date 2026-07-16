#!/usr/bin/env python3
"""
Tracker web app — FastAPI API + static frontend (server/static/).

Endpoints:
  GET /api/devices    known devices with last_seen + position count
  GET /api/positions  position history for one device (relative or absolute range)
  GET /*              static frontend (server/static/, index.html)

Usage:
  uvicorn app:app --reload --host 0.0.0.0 --port 8080 --app-dir server
"""

import os
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

import alerts

# TRACKER_DB overrides the DB path so tests can run against a seeded copy
# without ever touching the real one.
DB_DEFAULT = Path(os.environ.get("TRACKER_DB",
                                 Path(__file__).parent / "tracker.db"))
STATIC_DIR = Path(__file__).parent / "static"


def get_db(path: Path = DB_DEFAULT) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.db = get_db()
    # Ensure the armed-state table exists even if the web app starts before
    # the ingest (both create it; idempotent).
    alerts.ensure_schema(app.state.db)
    yield
    app.state.db.close()


app = FastAPI(title="nRF9151 Tracker", lifespan=lifespan)


@app.middleware("http")
async def no_stale_assets(request, call_next):
    """Force revalidation of everything we serve.

    The JS is ES modules importing each other by path: a cached app.js from
    one deploy importing modules deleted in the next 404s and kills the whole
    page (browsers and Cloudflare both cache .js by default). no-cache still
    allows caching, but requires revalidation, so a deploy can never serve a
    mixed module graph. The assets are tiny; correctness beats cache hits.
    """
    resp = await call_next(request)
    resp.headers["Cache-Control"] = "no-cache"
    return resp


@app.get("/api/devices")
async def devices():
    db = app.state.db
    # last_seen = anything the server has heard, not just positions. A
    # healthy parked night can produce NO position/log rows at all (checks
    # repeat-suppressed, INF quiet, serving cell stable) while datagrams
    # still arrive — the usage ledger records every one of those, so it is
    # the ground truth for "alive". Devices still appear only once they
    # have a position.
    rows = db.execute(
        "SELECT p.device_id, "
        "       MAX(p.ls, COALESCE(l.ls, 0), COALESCE(u.ls, 0)) AS last_seen, "
        "       p.n, COALESCE(a.armed, 0) AS armed "
        "FROM (SELECT device_id, MAX(received_ts) AS ls, COUNT(*) AS n "
        "      FROM positions GROUP BY device_id) p "
        "LEFT JOIN (SELECT device_id, MAX(received_ts) AS ls "
        "           FROM logs WHERE origin = 'device' GROUP BY device_id) l "
        "  ON l.device_id = p.device_id "
        "LEFT JOIN (SELECT device_id, MAX(received_ts) AS ls "
        "           FROM usage GROUP BY device_id) u "
        "  ON u.device_id = p.device_id "
        "LEFT JOIN device_alerts a ON a.device_id = p.device_id "
        "ORDER BY last_seen DESC"
    ).fetchall()
    return JSONResponse([dict(r) for r in rows])


@app.post("/api/arm")
async def arm(request: Request):
    """Arm/disarm a device's motion alerts. Body: {device, armed}. The ingest
    reads this flag when a MotionEvent lands and pushes only when armed."""
    body = await request.json()
    device = body.get("device")
    armed = 1 if body.get("armed") else 0
    if not device:
        return JSONResponse({"error": "device required"}, status_code=400)
    db = app.state.db
    db.execute(
        "INSERT INTO device_alerts (device_id, armed) VALUES (?, ?) "
        "ON CONFLICT(device_id) DO UPDATE SET armed = excluded.armed",
        (device, armed))
    db.commit()
    return JSONResponse({"device": device, "armed": armed})


@app.get("/api/positions")
async def positions(device: str, since: int = 0,
                    from_ts: float = 0, to_ts: float = 0, limit: int = 5000):
    """Position history for one device, chronological.

    Live uses `since` (minutes back from now). Replay uses absolute epoch
    bounds `from_ts`/`to_ts`; when both are given they take precedence over
    `since`.
    """
    db = app.state.db
    params = [device]
    # Cell rows carry their tower identity (cell_events shares the insert
    # timestamp) so the detail pane can say WHICH tower, not just where.
    sql = ("SELECT p.id, p.source, p.lat, p.lon, p.alt, p.acc, p.spd, p.hdg, "
           "p.sats, p.received_ts, "
           "ce.mcc, ce.mnc, ce.tac, ce.cell_id, ce.rsrp_dbm, ce.act "
           "FROM positions p LEFT JOIN cell_events ce "
           "ON p.source = 'cell' AND ce.device_id = p.device_id "
           "AND ce.received_ts = p.received_ts "
           "WHERE p.device_id = ?")
    if from_ts and to_ts:
        sql += " AND p.received_ts BETWEEN ? AND ?"
        params += [from_ts, to_ts]
    elif since > 0:
        sql += " AND p.received_ts >= ?"
        params.append(int(time.time()) - since * 60)
    sql += " ORDER BY p.received_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.execute(sql, params).fetchall()
    return JSONResponse([dict(r) for r in reversed(rows)])


@app.get("/api/cells")
async def cells(device: str, from_ts: float = 0, to_ts: float = 0,
                limit: int = 2000):
    """Serving-cell history, chronological: which tower, what technology
    (3GPP AcT: 7 LTE-M, 9 NB-IoT, 0 unknown), what signal. Includes cells
    the tower DB couldn't place (lat/lon null)."""
    db = app.state.db
    params = [device]
    sql = ("SELECT id, received_ts, mcc, mnc, tac, cell_id, rsrp_dbm, act, "
           "lat, lon, acc FROM cell_events WHERE device_id = ?")
    if from_ts and to_ts:
        sql += " AND received_ts BETWEEN ? AND ?"
        params += [from_ts, to_ts]
    sql += " ORDER BY received_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.execute(sql, params).fetchall()
    return JSONResponse([dict(r) for r in reversed(rows)])


@app.get("/api/logs")
async def logs(device: str, from_ts: float = 0, to_ts: float = 0,
               min_level: int = 4, limit: int = 2000,
               origin: str = "device"):
    """Log lines, chronological. min_level filters by severity (Zephyr
    numbering: 1=ERR 2=WRN 3=INF 4=DBG — lower is more severe, so
    min_level=2 returns ERR+WRN). origin 'device' (default) = the device's
    uplinked logs; 'server' = the server's own events concerning that
    device; 'all' = both interleaved — the correlated view. Whenever server
    rows are included, so are the GLOBAL ones (device_id '_server': startup
    markers, assistance-supply health) — an outage that starves every
    device belongs in any single device's story (2026-07-16)."""
    db = app.state.db
    sel = "SELECT id, received_ts, level, module, text, origin FROM logs "
    if origin == "all":
        params = [device, min_level]
        sql = sel + ("WHERE (device_id = ? OR device_id = '_server') "
                     "AND level <= ?")
    elif origin == "server":
        params = [device, min_level]
        sql = sel + ("WHERE (device_id = ? OR device_id = '_server') "
                     "AND origin = 'server' AND level <= ?")
    else:
        params = [device, origin, min_level]
        sql = sel + "WHERE device_id = ? AND origin = ? AND level <= ?"
    if from_ts and to_ts:
        sql += " AND received_ts BETWEEN ? AND ?"
        params += [from_ts, to_ts]
    sql += " ORDER BY received_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.execute(sql, params).fetchall()
    return JSONResponse([dict(r) for r in reversed(rows)])


@app.get("/api/events")
async def events(device: str, from_ts: float = 0, to_ts: float = 0,
                 limit: int = 500):
    """Events (something happened, vs positions = where it was), newest
    first. Each row carries the nearest position at that time (within 5 min)
    so the UI can place it on a map without a second round trip."""
    db = app.state.db
    params = [device]
    sql = "SELECT id, received_ts, kind, reason FROM events WHERE device_id = ?"
    if from_ts and to_ts:
        sql += " AND received_ts BETWEEN ? AND ?"
        params += [from_ts, to_ts]
    sql += " ORDER BY received_ts DESC LIMIT ?"
    params.append(limit)
    out = []
    for r in db.execute(sql, params).fetchall():
        e = dict(r)
        pos = db.execute(
            "SELECT lat, lon FROM positions WHERE device_id = ? "
            "AND ABS(received_ts - ?) < 300 "
            "ORDER BY ABS(received_ts - ?) LIMIT 1",
            (device, e["received_ts"], e["received_ts"])).fetchone()
        e["lat"] = pos["lat"] if pos else None
        e["lon"] = pos["lon"] if pos else None
        out.append(e)
    return JSONResponse(out)


# Per-datagram framing estimate for on-air bytes: 20 IP + 8 UDP + ~44 CoAP
# header/token/options. Billed usage additionally rounds per RRC session
# (field-calibrated ~2x on quiet days), which no per-datagram sum can see --
# the UI says so instead of pretending.
WIRE_OVERHEAD_B = 72


@app.get("/api/usage")
async def usage(device: str, days: int = 14):
    """Per-UTC-day data usage from the ingest ledger: datagram count, payload
    bytes, and estimated on-air bytes (payload + framing), split by kind."""
    db = app.state.db
    since = time.time() - days * 86400
    rows = db.execute(
        """SELECT date(received_ts, 'unixepoch') AS day,
                  kind,
                  COUNT(*)   AS datagrams,
                  SUM(bytes) AS payload
           FROM usage WHERE device_id = ? AND received_ts >= ?
           GROUP BY day, kind ORDER BY day""",
        (device, since)).fetchall()
    out = {}
    for r in rows:
        d = out.setdefault(r["day"], {
            "day": r["day"], "datagrams": 0, "payload": 0,
            "obs_bytes": 0, "agnss_bytes": 0, "est_wire": 0,
        })
        d["datagrams"] += r["datagrams"]
        d["payload"] += r["payload"]
        d[r["kind"] + "_bytes"] = r["payload"]
        d["est_wire"] += r["payload"] + r["datagrams"] * WIRE_OVERHEAD_B
    return JSONResponse(list(out.values()))


# Static frontend. Mounted last so /api/* routes above take precedence.
# html=True serves index.html for "/" (and as the SPA fallback).
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8080, reload=False,
                app_dir=str(Path(__file__).parent))
