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

        # Cross-view selection: clicking the speed chart must open the fix
        # drawer AND highlight the matching table row.
        box = page.locator("#chart-speed").bounding_box()
        if box:
            page.mouse.click(box["x"] + box["width"] * 0.5,
                             box["y"] + box["height"] * 0.5)
            page.wait_for_timeout(400)
            if page.locator("#fix-drawer.open").count() == 0:
                problems.append("chart click did not open the fix drawer")
            if page.locator(".ftable-table tr.sel").count() == 0:
                problems.append("chart click did not highlight a table row")

        # Day steppers must navigate without errors.
        for tid in ("#date-prev", "#date-next"):
            if page.locator(tid).count():
                page.click(tid)
                page.wait_for_timeout(400)

        # Logs page: rows render, WRN filter narrows, ALL widens.
        page.goto(args.url + "/#logs", wait_until="networkidle")
        page.wait_for_timeout(600)
        n_inf = page.locator("#log-tbody tr").count()
        if n_inf == 0:
            problems.append("logs page rendered no rows at +INF")
        page.click('#logs-level .pill[data-level="2"]')
        page.wait_for_timeout(600)
        n_wrn = page.locator("#log-tbody tr").count()
        if not (0 < n_wrn < n_inf):
            problems.append(f"level filter broken: {n_inf} rows at INF, {n_wrn} at WRN")
        page.click('#logs-level .pill[data-level="4"]')
        page.wait_for_timeout(600)
        if page.locator("#log-tbody tr").count() <= n_inf:
            problems.append("ALL level filter did not widen the row set")

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
