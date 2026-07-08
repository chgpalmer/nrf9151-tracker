#!/usr/bin/env python3
"""
MQTT → SQLite ingest.

Subscribes to the tracker topic and writes each fix into location.db.
Dummy payloads (no lat/lon) are stored with is_fix=0.

Usage:
  python3 server/ingest.py [--host HOST] [--port PORT] [--topic TOPIC] [--db DB]
"""

import argparse
import json
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt

DB_DEFAULT = Path(__file__).parent / "location.db"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS location (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            received  TEXT    NOT NULL,
            is_fix    INTEGER NOT NULL DEFAULT 0,
            lat       REAL,
            lon       REAL,
            alt       REAL,
            acc       REAL,
            sats_used INTEGER,
            ts        TEXT,
            raw       TEXT    NOT NULL
        )
    """)
    conn.commit()
    return conn


def insert(conn: sqlite3.Connection, payload: str) -> None:
    received = datetime.now(timezone.utc).isoformat()
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        print(f"[ingest] bad JSON: {payload!r}", file=sys.stderr)
        return

    is_fix = 1 if "lat" in data else 0
    conn.execute(
        """INSERT INTO location (received, is_fix, lat, lon, alt, acc, sats_used, ts, raw)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        (
            received,
            is_fix,
            data.get("lat"),
            data.get("lon"),
            data.get("alt"),
            data.get("acc"),
            data.get("sats_used"),
            data.get("ts"),
            payload,
        ),
    )
    conn.commit()

    if is_fix:
        print(f"[ingest] fix  {data['lat']:.6f}, {data['lon']:.6f}  acc {data.get('acc')}m")
    else:
        print(f"[ingest] dummy payload received")


def main():
    parser = argparse.ArgumentParser(description="MQTT → SQLite ingest")
    parser.add_argument("--host",  default="localhost")
    parser.add_argument("--port",  default=1883, type=int)
    parser.add_argument("--topic", default="tracker/#")
    parser.add_argument("--db",    default=str(DB_DEFAULT))
    args = parser.parse_args()

    conn = init_db(Path(args.db))
    print(f"[ingest] db: {args.db}")

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print(f"[ingest] connected to {args.host}:{args.port} — subscribing to {args.topic!r}")
            client.subscribe(args.topic)
        else:
            print(f"[ingest] connection refused: {reason_code}", file=sys.stderr)

    def on_message(client, userdata, msg):
        insert(conn, msg.payload.decode("utf-8", errors="replace"))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[ingest] stopped")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
