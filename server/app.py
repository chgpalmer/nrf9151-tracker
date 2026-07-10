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

import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

DB_DEFAULT = Path(__file__).parent / "tracker.db"
STATIC_DIR = Path(__file__).parent / "static"


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


@app.get("/api/devices")
async def devices():
    db = app.state.db
    rows = db.execute(
        "SELECT device_id, MAX(received_ts) AS last_seen, COUNT(*) AS n "
        "FROM positions GROUP BY device_id ORDER BY last_seen DESC"
    ).fetchall()
    return JSONResponse([dict(r) for r in rows])


@app.get("/api/positions")
async def positions(device: str, since: int = 0,
                    from_ts: int = 0, to_ts: int = 0, limit: int = 5000):
    """Position history for one device, chronological.

    Live uses `since` (minutes back from now). Replay uses absolute epoch
    bounds `from_ts`/`to_ts`; when both are given they take precedence over
    `since`.
    """
    db = app.state.db
    params = [device]
    sql = ("SELECT id, source, lat, lon, alt, acc, spd, hdg, sats, received_ts "
           "FROM positions WHERE device_id = ?")
    if from_ts and to_ts:
        sql += " AND received_ts BETWEEN ? AND ?"
        params += [from_ts, to_ts]
    elif since > 0:
        sql += " AND received_ts >= ?"
        params.append(int(time.time()) - since * 60)
    sql += " ORDER BY received_ts DESC LIMIT ?"
    params.append(limit)
    rows = db.execute(sql, params).fetchall()
    return JSONResponse([dict(r) for r in reversed(rows)])


# Static frontend. Mounted last so /api/* routes above take precedence.
# html=True serves index.html for "/" (and as the SPA fallback).
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("app:app", host="0.0.0.0", port=8080, reload=False,
                app_dir=str(Path(__file__).parent))
