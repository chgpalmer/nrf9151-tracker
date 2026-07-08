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
import sqlite3
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

DB_DEFAULT = Path(__file__).parent / "tracker.db"


def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS positions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT    NOT NULL,
            received_ts INTEGER NOT NULL,
            lat REAL, lon REAL, alt REAL, acc REAL,
            spd REAL, hdg REAL, sats INTEGER
        )
    """)
    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_pos_dev_time
        ON positions(device_id, received_ts)
    """)
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
           (device_id, received_ts, lat, lon, alt, acc, spd, hdg, sats)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        (
            device_id, received_ts,
            d.get("lat"), d.get("lon"), d.get("alt"), d.get("acc"),
            d.get("spd"), d.get("hdg"), d.get("sats"),
        ),
    )
    conn.commit()
    print(f"[ingest] {device_id}  {d.get('lat'):.6f}, {d.get('lon'):.6f}  "
          f"spd {d.get('spd')}m/s  hdg {d.get('hdg')}°")


def main():
    parser = argparse.ArgumentParser(description="MQTT → SQLite ingest")
    parser.add_argument("--host",  default="localhost")
    parser.add_argument("--port",  default=1883, type=int)
    parser.add_argument("--topic", default="trackers/#")
    parser.add_argument("--db",    default=str(DB_DEFAULT))
    args = parser.parse_args()

    conn = init_db(Path(args.db))
    print(f"[ingest] db: {args.db}")

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
        if kind == "position":
            handle_position(conn, device_id, msg.payload.decode("utf-8", errors="replace"))
        # status/event reserved for later

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
