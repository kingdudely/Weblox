#!/usr/bin/env bash
# Run the client in dev mode (hot reload through rkhost).
#
# The installed app is the signed bundle from tools/build_bundle.sh; this script
# is only for iterating on the engine without rebuilding the bundle.
#
#   ./tools/run_iwa.sh                 # stage, serve, open
#   PLACE=12345 ./tools/run_iwa.sh     # different place
#
# First run installs the app into the profile below; a manifest change
# reinstalls it, because permissions policy and protocol handlers are read at
# install time and nothing else. Source changes are live: the dev-mode proxy
# forwards every request to rkhost.
set -euo pipefail

pidfile="/tmp/weblox-rkhost.pid"
if [ "${1:-}" = "--stop" ]; then
  if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
    kill "$(cat "$pidfile")" && echo "stopped the dev server"
  else
    echo "no dev server started by this script"
  fi
  rm -f "$pidfile"
  exit 0
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
port="${PORT:-8770}"
if [ "$port" = 0 ]; then
  port=8770
fi
origin="http://127.0.0.1:$port"
profile="${WEBLOX_IWA_PROFILE:-$HOME/.weblox-iwa}"
chrome="${CHROME:-google-chrome}"
manifest="$here/iwa/.well-known/manifest.webmanifest"
stamp="$profile/.weblox-manifest"
log="${RKLOG:-/tmp/opencode/rkhost-iwa.log}"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

"$here/tools/stage_engine.sh"

# Serve the staged bundle unless something is already answering on the port.
if ! curl -sf -o /dev/null "$origin/.well-known/manifest.webmanifest"; then
  mkdir -p "$(dirname "$log")"
  # Detached: the installed app proxies every request here, so the server must
  # outlive this script. --stop knows the pid.
  setsid "$here/app/rkhost/target/debug/rkhost" --root "$here/iwa" --port "$port" > "$log" 2>&1 < /dev/null &
  echo "$!" > "$pidfile"
  for _ in $(seq 1 40); do
    curl -sf -o /dev/null "$origin/" && break
    sleep 0.25
  done
  if ! curl -sf -o /dev/null "$origin/"; then
    echo "rkhost did not start; see $log"
    exit 1
  fi
fi

# The desktop entry Chrome writes at install is both the completion marker and
# the source of the app id. The origin string shows up in Preferences while the
# install is still running, so it cannot be used for either.
entry() {
  grep -l "x-scheme-handler/roblox-player" "$apps"/chrome-*.desktop 2>/dev/null | head -1
}

wanted="$(sha1sum "$manifest" | cut -d' ' -f1)"
have="$(cat "$stamp" 2>/dev/null || true)"

if [ -z "$(entry)" ] || [ "$wanted" != "$have" ]; then
  echo "installing the app into $profile (one time, or after a manifest change)…"
  before="$(ls "$apps"/chrome-*.desktop 2>/dev/null | sort || true)"
  after="$before"
  "$chrome" --user-data-dir="$profile" \
    --enable-features=IsolatedWebApps,IsolatedWebAppDevMode \
    --install-isolated-web-app-from-url="$origin/" \
    --no-first-run --no-default-browser-check about:blank \
    >/dev/null 2>&1 &
  installer=$!
  for _ in $(seq 1 30); do
    after="$(ls "$apps"/chrome-*.desktop 2>/dev/null | sort || true)"
    [ "$after" != "$before" ] && break
    sleep 1
  done
  # A manifest update rewrites the existing entry rather than adding one.
  if [ "$after" = "$before" ]; then
    sleep 8
  fi
  # Only the instance this script started: the profile path is in the script's
  # own command line too, so a pattern kill would take the shell with it.
  kill "$installer" 2>/dev/null || true
  sleep 2
  mkdir -p "$profile"
  printf '%s\n' "$wanted" > "$stamp"
fi

appid="$(sed -n 's/.*--app-id=\([a-z]\{32\}\).*/\1/p' "$(entry)" 2>/dev/null | head -1)"
if [ -z "$appid" ]; then
  echo "install failed: no app entry claiming roblox-player: (check chrome://web-app-internals)"
  exit 1
fi
# Nothing to open: the app has no stored session, so a game starts only from
# roblox.com's Play button, whose link carries the one-time ticket.
echo "dev app installed (app id $appid). Dev server is serving $origin (pid $(cat "$pidfile" 2>/dev/null || echo unknown))."
echo "Open roblox.com in a browser and press Play. Stop the server with: $0 --stop"
echo "Logs: $log"
