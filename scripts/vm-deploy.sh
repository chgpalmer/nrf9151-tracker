#!/usr/bin/env bash
# Staging deploy of the WORKING TREE to the VM for end-to-end hardware testing
# without committing. The device's server target is baked at build time, so a
# dev deploy necessarily REPLACES the production ingest for the session (same
# ports); Caddy proxies whichever tree is live. See the design at
# docs/superpowers/specs/2026-07-14-dev-loop-staging-design.md.
#
#   scripts/vm-deploy.sh dev    rsync the working tree, serve it (snapshots the
#                               live DB first, once per session)
#   scripts/vm-deploy.sh prod   restore the clean git checkout (requires a
#                               clean, pushed tree) and clear the session
#
# VM host comes from $TRACKER_VM (default: the `vm` ssh alias).
set -euo pipefail

MODE=${1:?usage: vm-deploy.sh dev|prod}
VM=${TRACKER_VM:-vm}
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

VM_WS=/home/ubuntu/src/nrf9151-tracker-ws
VM_PROD=$VM_WS/nrf9151-tracker
VM_DEV=$VM_WS/nrf9151-tracker-dev
VM_DB=$VM_PROD/server/tracker.db          # dev serves the LIVE db (design)
VM_CELLS=$VM_PROD/server/cells.db         # ...and the prod tower DB (big, static)
VENV=$VM_WS/.venv
MARKER=$VM_WS/.dev-session

banner() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

# Kill whatever holds the two ports (prod `make serve` wrapper or a prior dev
# session), then start the two Python services detached from this tree.
remote_restart() {  # $1 = tree dir, $2 = mode label
  ssh "$VM" "bash -s" <<EOF
set -e
fuser -k 8080/tcp 5683/udp >/dev/null 2>&1 || true
pkill -f '[m]ake --no-print-directory stop' >/dev/null 2>&1 || true
pkill -f '[c]oap_server.py' >/dev/null 2>&1 || true
sleep 1
cd "$1"
TRACKER_DEV_SESSION="$2" TRACKER_DB="$VM_DB" setsid nohup \
  "$VENV/bin/uvicorn" app:app --host 127.0.0.1 --port 8080 --app-dir server \
  >~/web.log 2>&1 </dev/null &
TRACKER_DEV_SESSION="$2" setsid nohup \
  "$VENV/bin/python3" -u server/coap_server.py --port 5683 --db "$VM_DB" \
  --cells-db "$VM_CELLS" >~/coap.log 2>&1 </dev/null &
sleep 4
curl -fsS http://127.0.0.1:8080/api/devices >/dev/null && echo "  web OK"
ss -ulnp 2>/dev/null | grep -q :5683 && echo "  coap OK"
EOF
}

case "$MODE" in
dev)
  banner "rsync working tree -> $VM:$VM_DEV"
  rsync -az --delete \
    --exclude build/ --exclude .git/ --exclude docs/superpowers/ \
    --exclude .env --exclude 'server/tracker.db*' --exclude server/cells.db \
    --exclude '__pycache__/' --exclude '*.pyc' \
    "$REPO_ROOT/" "$VM:$VM_DEV/"

  banner "snapshot live DB (once per session)"
  ssh "$VM" "bash -s" <<EOF
if [ ! -f "$MARKER" ]; then
  cp "$VM_DB" "$VM_DB.pre-dev" && echo "  snapshot -> tracker.db.pre-dev"
  date -u +%Y-%m-%dT%H:%M:%SZ > "$MARKER"
else
  echo "  session already open since \$(cat $MARKER) — snapshot kept"
fi
EOF

  banner "serve the dev tree (replaces prod for this session)"
  remote_restart "$VM_DEV" "dev"
  printf '\n\033[33m⚠  DEV TREE IS LIVE on %s — device data flows into the real DB.\n   `make deploy-prod` restores the committed checkout.\033[0m\n' "$VM"
  ;;

prod)
  banner "checking working tree is clean and pushed"
  cd "$REPO_ROOT"
  if [ -n "$(git status --porcelain)" ]; then
    echo "ABORT: working tree is dirty — commit or stash before deploy-prod." >&2
    exit 1
  fi
  if [ -n "$(git log --oneline @{u}.. 2>/dev/null)" ]; then
    echo "ABORT: unpushed commits — push before deploy-prod." >&2
    exit 1
  fi

  banner "restore clean checkout on $VM"
  ssh "$VM" "bash -s" <<EOF
set -e
cd "$VM_PROD"
git pull --ff-only
"$VENV/bin/pip" install --quiet -r server/requirements.txt
EOF
  remote_restart "$VM_PROD" "prod"
  ssh "$VM" "rm -f '$MARKER' && echo '  session marker cleared'"
  printf '\n\033[32m✓  PROD checkout live on %s.\033[0m\n' "$VM"
  ;;

*)
  echo "usage: vm-deploy.sh dev|prod" >&2; exit 1 ;;
esac
