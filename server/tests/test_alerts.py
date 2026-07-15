"""alerts.maybe_alert gating — the decision to push, tested without a network.

The real ntfy POST (post_ntfy) is not exercised here; only the armed/cooldown
logic that decides whether to send.
"""

import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import alerts

DEV = "test-dev"


def _db(armed=0, last_alert_ts=0.0):
    conn = sqlite3.connect(":memory:")
    alerts.ensure_schema(conn)
    conn.execute("CREATE TABLE positions (device_id TEXT, received_ts REAL, lat REAL, lon REAL)")
    conn.execute(
        "INSERT INTO device_alerts (device_id, armed, last_alert_ts) VALUES (?, ?, ?)",
        (DEV, armed, last_alert_ts))
    conn.commit()
    return conn


def test_disarmed_does_not_send():
    conn = _db(armed=0)
    outcome, payload = alerts.maybe_alert(conn, DEV, time.time(), "https://ntfy.sh/x")
    assert outcome == "disarmed"
    assert payload is None


def test_armed_sends_and_claims_cooldown():
    now = 1_000_000.0
    conn = _db(armed=1, last_alert_ts=0.0)
    outcome, payload = alerts.maybe_alert(
        conn, DEV, now, "https://ntfy.sh/x", web_url="https://t.example.com", now=now)
    assert outcome == "send"
    assert "moved while armed" in payload["body"]
    assert payload["click"] == "https://t.example.com/#events"
    # cooldown is claimed immediately
    last = conn.execute(
        "SELECT last_alert_ts FROM device_alerts WHERE device_id = ?", (DEV,)).fetchone()[0]
    assert last == now


def test_within_cooldown_does_not_send():
    now = 1_000_000.0
    conn = _db(armed=1, last_alert_ts=now - (alerts.ALERT_COOLDOWN_S - 10))
    outcome, _ = alerts.maybe_alert(conn, DEV, now, "https://ntfy.sh/x", now=now)
    assert outcome == "cooldown"


def test_cooldown_elapsed_sends_again():
    now = 1_000_000.0
    conn = _db(armed=1, last_alert_ts=now - (alerts.ALERT_COOLDOWN_S + 10))
    outcome, _ = alerts.maybe_alert(conn, DEV, now, "https://ntfy.sh/x", now=now)
    assert outcome == "send"


def test_no_url_does_not_send():
    outcome, payload = alerts.maybe_alert(_db(armed=1), DEV, time.time(), "")
    assert outcome == "no-url"
    assert payload is None


def test_no_click_without_web_url():
    now = 1_000_000.0
    conn = _db(armed=1)
    _, payload = alerts.maybe_alert(conn, DEV, now, "https://ntfy.sh/x", web_url="", now=now)
    assert payload["click"] is None
