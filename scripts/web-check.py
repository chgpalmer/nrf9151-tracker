#!/usr/bin/env python3
"""
Headless-browser check of the web UI: load it, EXECUTE it, and fail loudly.

Parse checks and curl probes cannot catch a page that serves fine but dies at
runtime (broken import, missing element, bad API shape) — this can. It drives
the real page in headless Chromium and fails on:

  - any console error or uncaught page error
  - any failed subresource (404 module import, CDN miss)
  - the map page not actually rendering (no Leaflet container / no chips bar)

and then exercises the basic interactions (chip clicks, source toggles).

Usage:
  scripts/web-check.py [--url http://127.0.0.1:8080]

Needs: pip install playwright && playwright install chromium
(dev-only; not part of server requirements)
"""

import argparse
import sys

from playwright.sync_api import sync_playwright


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--screenshot", default=None, help="save a PNG here")
    args = ap.parse_args()

    problems = []
    console_errors = []

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1400, "height": 900})

        page.on("console", lambda m: console_errors.append(f"console.{m.type}: {m.text}")
                if m.type in ("error",) else None)
        page.on("pageerror", lambda e: console_errors.append(f"pageerror: {e}"))
        # Leaflet aborts in-flight tile fetches whenever the view moves;
        # ERR_ABORTED on basemap tiles is normal, not a failure.
        page.on("requestfailed", lambda r: problems.append(
            f"request failed: {r.url} ({r.failure})")
            if not ("ERR_ABORTED" in str(r.failure) and "basemaps" in r.url) else None)
        page.on("response", lambda r: problems.append(
            f"HTTP {r.status}: {r.url}") if r.status >= 400 else None)

        page.goto(args.url, wait_until="networkidle", timeout=30000)

        # The page must have actually built itself.
        checks = [
            (".leaflet-container", "Leaflet map initialised"),
            ("#trip-chips", "trip chips bar present"),
            ("#tl-track", "timeline present"),
            ("#chart-speed", "charts present"),
        ]
        for selector, what in checks:
            if page.locator(selector).count() == 0:
                problems.append(f"missing: {what} ({selector})")

        # Exercise the chips and toggles; each click must not throw.
        for chip in page.locator("#trip-chips button").all():
            chip.click()
            page.wait_for_timeout(150)
        for tid in ("#filter-gps", "#filter-cell", "#filter-gps", "#filter-cell"):
            if page.locator(tid).count():
                page.click(tid)
                page.wait_for_timeout(100)

        page.wait_for_timeout(500)

        if args.screenshot:
            page.screenshot(path=args.screenshot)

        browser.close()

    problems.extend(console_errors)
    if problems:
        print("WEB CHECK FAILED:")
        for pr in problems:
            print("  -", pr)
        sys.exit(1)
    print("web check OK: page executes cleanly, UI renders, interactions work")


if __name__ == "__main__":
    main()
