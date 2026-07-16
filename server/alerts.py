"""Motion alerts: when an *armed* device reports movement, push to the owner.

The device stays dumb (it just emits MotionEvents); arming is server-side
state (device_alerts.armed, toggled from the webapp) and delivery is a plain
ntfy.sh HTTP POST. Split so the gating is unit-testable without a network:
maybe_alert() does only main-thread DB work and returns a payload to send;
post_ntfy() does the blocking POST (run it off the event loop).

Privacy: the push carries NO location — an ntfy topic is only as private as
its (guessable) name, so the alert just says "it moved" and links to the
Caddy-password-protected webapp, where the position lives behind auth.
"""

import time
import urllib.request

# One push per device per this window, so a persistent jostle (or a thief
# fumbling with the lock) doesn't spam the phone.
ALERT_COOLDOWN_S = 300


def ensure_schema(conn):
    """Create the armed-state table. Both the ingest and the web app call
    this on startup (idempotent) so neither races the other to create it."""
    conn.execute(
        "CREATE TABLE IF NOT EXISTS device_alerts ("
        "  device_id     TEXT PRIMARY KEY, "
        "  armed         INTEGER NOT NULL DEFAULT 0, "
        "  last_alert_ts REAL    NOT NULL DEFAULT 0)")
    conn.commit()


def maybe_alert(conn, device_id, event_ts, ntfy_url, web_url=None, now=None,
                kind="motion"):
    """Decide whether this event warrants a push. Main-thread DB only, no
    network. Returns (outcome, payload): payload is a dict for post_ntfy()
    when outcome == 'send', else None. Outcomes: send | disarmed | no-url |
    cooldown.

    kind 'motion' = the device reported an IMU wake; 'boot' = the device's
    boot identity arrived while armed — a power cut on an armed asset (the
    device forgets it was parked across a reboot, so it will NOT raise a
    motion event; this server-side alert closes that hole — battery pulled,
    2026-07-16). Both kinds share ONE cooldown: legitimate handling fires
    wake + boot within seconds, and one ping per 5 min is the contract."""
    if now is None:
        now = time.time()

    row = conn.execute(
        "SELECT armed, last_alert_ts FROM device_alerts WHERE device_id = ?",
        (device_id,)).fetchone()
    armed = row[0] if row else 0
    last_alert_ts = row[1] if row else 0.0

    if not armed:
        return ("disarmed", None)
    if not ntfy_url:
        return ("no-url", None)
    if now - last_alert_ts < ALERT_COOLDOWN_S:
        return ("cooldown", None)

    hhmm = time.strftime("%H:%M", time.localtime(event_ts))
    if kind == "boot":
        body = (f"{device_id} REBOOTED while armed at {hhmm} — "
                f"power was cut; tap to see where")
        title, tags = "Reboot while armed", "electric_plug"
    else:
        body = f"{device_id} moved while armed at {hhmm} — tap to see where"
        title, tags = "Motion alert", "rotating_light"
    # Click through to the auth-gated webapp (Events page), never a bare
    # location: keeps the position behind the Caddy password.
    click = f"{web_url.rstrip('/')}/#events" if web_url else None

    # Claim the cooldown now (optimistic): a failed POST just misses one
    # alert rather than risking a burst.
    conn.execute(
        "UPDATE device_alerts SET last_alert_ts = ? WHERE device_id = ?",
        (now, device_id))
    conn.commit()

    return ("send", {"body": body, "click": click,
                     "title": title, "tags": tags})


def post_ntfy(ntfy_url, payload, timeout=5):
    """Blocking POST to ntfy.sh — run under asyncio.to_thread(). Title/emoji
    ride HTTP headers (kept ASCII; the emoji comes from the Tags header, so
    header encoding stays clean)."""
    headers = {
        "Title": payload.get("title", "Motion alert"),
        "Priority": "high",
        "Tags": payload.get("tags", "rotating_light"),
    }
    if payload.get("click"):
        headers["Click"] = payload["click"]
    req = urllib.request.Request(
        ntfy_url, data=payload["body"].encode("utf-8"),
        headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status
