#!/usr/bin/env python3
"""
MQTT → SQLite ingest.

Subscribes to trackers/<device_id>/<kind> and stores position fixes.
The server stamps its own receive time (received_ts, Unix epoch) — the
firmware payload carries no timestamp.

Usage:
  python3 server/ingest.py [--host HOST] [--port PORT] [--topic TOPIC] [--db DB]
"""

import argparse
import json
import math
import sqlite3
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

DB_DEFAULT = Path(__file__).parent / "tracker.db"

# Local OpenCelliD lookup DB (built by build_cells.py from *.csv.gz exports).
CELLS_DB_DEFAULT = Path(__file__).parent / "cells.db"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS positions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts INTEGER NOT NULL,
            source      TEXT    NOT NULL DEFAULT 'gps',
            lat REAL, lon REAL, alt REAL, acc REAL,
            spd REAL, hdg REAL, sats INTEGER
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_pos_dev_time
        ON positions(device_id, received_ts)
    """)
    # Migrate older DBs created before the source column existed.
    cols = {r[1] for r in conn.execute("PRAGMA table_info(positions)")}
    if "source" not in cols:
        conn.execute(
            "ALTER TABLE positions ADD COLUMN source TEXT NOT NULL DEFAULT 'gps'")
    conn.commit()
    return conn


def handle_position(conn: sqlite3.Connection, device_id: str, payload: str) -> None:
    received_ts = int(time.time())
    try:
        d = json.loads(payload)
    except json.JSONDecodeError:
        print(f"[ingest] bad JSON on {device_id}: {payload!r}", file=sys.stderr)
        return

    conn.execute(
        """INSERT INTO positions
           (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, 'gps', ?, ?, ?, ?, ?, ?, ?)""",
        (
            device_id, received_ts,
            d.get("lat"), d.get("lon"), d.get("alt"), d.get("acc"),
            d.get("spd"), d.get("hdg"), d.get("sats"),
        ),
    )
    conn.commit()
    print(f"[ingest] {device_id} gps  {d.get('lat'):.6f}, {d.get('lon'):.6f}  "
          f"spd {d.get('spd')}m/s  hdg {d.get('hdg')}°")


def resolve_cell(payload: dict, cells: sqlite3.Connection | None):
    """Resolve a serving cell to (lat, lon, accuracy, how) via the local
    OpenCelliD DB. Returns None on miss or if no DB is loaded. Neighbor cells
    are not used (OpenCelliD keys on full mcc/net/area/cell identity, which
    neighbors lack).

    Two strategies, in order:

    'exact'  the full mcc/net/area/cell 4-tuple. Best, but OpenCelliD is
             crowdsourced and sparse: a given sector is often absent, and
             operators re-plan tracking areas, so a live TAC may not match the
             recorded one even when the tower itself is well surveyed.

    'enb'    LTE only. An LTE cell id is (eNB << 8) | sector, so we drop the
             sector and the TAC and average the sectors we do have for that
             eNB. Accuracy becomes the sector spread, since we know the tower,
             not which face of it is serving us.
    """
    if cells is None:
        return None
    mcc, mnc = payload.get("mcc"), payload.get("mnc")
    tac, cid = payload.get("tac"), payload.get("cid")

    row = cells.execute(
        "SELECT lat, lon, range FROM cells "
        "WHERE mcc=? AND net=? AND area=? AND cell=?",
        (mcc, mnc, tac, cid),
    ).fetchone()
    if row is not None:
        lat, lon, rng = row
        # OpenCelliD 'range' is a coverage radius estimate; use it as accuracy,
        # falling back to a coarse default when absent.
        return lat, lon, float(rng) if rng else 2000.0, "exact"

    if cid is None or mcc is None or mnc is None:
        return None
    enb = cid >> 8
    rows = cells.execute(
        "SELECT lat, lon, range FROM cells "
        "WHERE mcc=? AND net=? AND cell BETWEEN ? AND ? AND radio='LTE'",
        (mcc, mnc, enb << 8, (enb << 8) | 0xFF),
    ).fetchall()
    if not rows:
        return None

    lat = sum(r[0] for r in rows) / len(rows)
    lon = sum(r[1] for r in rows) / len(rows)
    # Accuracy: far enough out to cover every sector we averaged, but never
    # tighter than the towers' own claimed coverage radius.
    spread = max(_haversine_m(lat, lon, r[0], r[1]) for r in rows)
    rng = max((r[2] or 0) for r in rows)
    return lat, lon, max(spread, rng, 2000.0), f"enb {enb}, {len(rows)} sectors"


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in metres."""
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def handle_cell(conn: sqlite3.Connection, device_id: str, payload: str,
                cells: sqlite3.Connection | None) -> None:
    received_ts = int(time.time())
    try:
        d = json.loads(payload)
    except json.JSONDecodeError:
        print(f"[ingest] bad cell JSON on {device_id}: {payload!r}", file=sys.stderr)
        return

    fix = resolve_cell(d, cells)
    if fix is None:
        print(f"[ingest] {device_id} cell  unresolved "
              f"(mcc={d.get('mcc')} mnc={d.get('mnc')} "
              f"tac={d.get('tac')} cid={d.get('cid')})")
        return

    lat, lon, acc, how = fix
    conn.execute(
        """INSERT INTO positions
           (device_id, received_ts, source, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, 'cell', ?, ?, NULL, ?, NULL, NULL, NULL)""",
        (device_id, received_ts, lat, lon, acc),
    )
    conn.commit()
    print(f"[ingest] {device_id} cell  {lat:.6f}, {lon:.6f}  acc {acc:.0f}m ({how})")


def main():
    parser = argparse.ArgumentParser(description="MQTT → SQLite ingest")
    parser.add_argument("--host",  default="localhost")
    parser.add_argument("--port",  default=1883, type=int)
    parser.add_argument("--topic", default="trackers/#")
    parser.add_argument("--db",    default=str(DB_DEFAULT))
    parser.add_argument("--cells-db", default=str(CELLS_DB_DEFAULT),
                        help="OpenCelliD lookup DB (build with build_cells.py)")
    args = parser.parse_args()

    conn = init_db(Path(args.db))
    print(f"[ingest] db: {args.db}")

    cells = None
    if Path(args.cells_db).exists():
        cells = sqlite3.connect(f"file:{args.cells_db}?mode=ro", uri=True,
                                check_same_thread=False)
        n = cells.execute("SELECT COUNT(*) FROM cells").fetchone()[0]
        print(f"[ingest] cell DB: {args.cells_db} ({n} cells)")
    else:
        print(f"[ingest] no cell DB at {args.cells_db} — cell fixes won't resolve "
              f"(run: make cells)")

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print(f"[ingest] connected to {args.host}:{args.port} — "
                  f"subscribing to {args.topic!r}")
            client.subscribe(args.topic)
        else:
            print(f"[ingest] connection refused: {reason_code}", file=sys.stderr)

    def on_message(client, userdata, msg):
        # topic: trackers/<device_id>/<kind>
        parts = msg.topic.split("/")
        if len(parts) != 3:
            return
        _, device_id, kind = parts
        payload = msg.payload.decode("utf-8", errors="replace")
        if kind == "position":
            handle_position(conn, device_id, payload)
        elif kind == "cell":
            handle_cell(conn, device_id, payload, cells)
        # status/event reserved for later

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    # Broker may not be up yet (e.g. started moments earlier). Retry briefly.
    for attempt in range(30):
        try:
            client.connect(args.host, args.port, keepalive=60)
            break
        except (ConnectionRefusedError, OSError) as e:
            if attempt == 29:
                print(f"[ingest] cannot reach broker {args.host}:{args.port}: {e}",
                      file=sys.stderr)
                return
            time.sleep(0.5)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[ingest] stopped")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
