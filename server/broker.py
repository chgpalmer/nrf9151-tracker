#!/usr/bin/env python3
"""
MQTT subscriber for nRF9151 tracker.

Subscribes to the tracker topic and prints each payload as formatted JSON
with a timestamp. Requires a running Mosquitto broker (apt install mosquitto).

Usage:
  pip install paho-mqtt
  mosquitto -v &
  python3 server/broker.py [--host HOST] [--port PORT] [--topic TOPIC]
"""

import argparse
import json
import sys
from datetime import datetime, timezone

import paho.mqtt.client as mqtt


def on_connect(client, userdata, flags, reason_code, properties):
    topic = userdata["topic"]
    if reason_code == 0:
        print(f"[broker] connected — subscribing to {topic!r}", flush=True)
        client.subscribe(topic)
    else:
        print(f"[broker] connection refused: {reason_code}", file=sys.stderr)


def on_message(client, userdata, msg):
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    payload = msg.payload.decode("utf-8", errors="replace")
    try:
        parsed = json.loads(payload)
        pretty = json.dumps(parsed, indent=2)
    except json.JSONDecodeError:
        pretty = payload

    print(f"\n[{ts}] {msg.topic}")
    print(pretty, flush=True)


def main():
    parser = argparse.ArgumentParser(description="nRF9151 MQTT subscriber")
    parser.add_argument("--host",  default="localhost", help="Broker host (default: localhost)")
    parser.add_argument("--port",  default=1883, type=int, help="Broker port (default: 1883)")
    parser.add_argument("--topic", default="trackers/#", help="Topic filter (default: trackers/#)")
    args = parser.parse_args()

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        userdata={"topic": args.topic},
    )
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"[broker] connecting to {args.host}:{args.port}...")
    client.connect(args.host, args.port, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[broker] stopped")


if __name__ == "__main__":
    main()
