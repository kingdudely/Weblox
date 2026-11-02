#!/usr/bin/env bash
# Run the client as an Isolated Web App.
#
#   ./tools/run_iwa.sh                 # start the server, open the app
#   PLACE=12345 ./tools/run_iwa.sh     # different place
#
# First run installs the app into the profile below (needs the dev-mode flag,
# which Chrome only honours at startup), then opens it. Source changes are live
# because the dev-mode proxy forwards every request to rkhost.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
port="${PORT:-8770}"
profile="${WEBLOX_IWA_PROFILE:-$HOME/.weblox-iwa}"
place="${PLACE:-13569002876}"
engine="${ENGINE_DIR:-/tmp/opencode/rw}"
cdn="${CDN_BASE:-https://setup.rbxcdn.com/webclient/version-75f1039cb4964863-RobloxWebClient}"
wasm="${WASM:-$here/build/WebClient.patched.wasm}"
chrome="${CHROME:-google-chrome}"

if [ ! -f "$wasm" ]; then
  echo "no patched module at $wasm - build it first:"
  echo "  ./tools/build_patched.sh /path/to/WebClient.wasm $wasm"
  exit 1
fi

# The server is a child of this script; Ctrl-C stops both.
if ! curl -sf -o /dev/null "http://127.0.0.1:$port/.well-known/manifest.webmanifest"; then
  "$here/app/rkhost/target/debug/rkhost" \
    --iwa --root "$here/iwa" --port "$port" \
    --wasm "$wasm" --engine-dir "$engine" --cdn-base "$cdn" \
    > /tmp/opencode/rkhost-iwa.log 2>&1 &
  server=$!
  trap 'kill $server 2>/dev/null || true' EXIT
  sleep 1
fi

app_id() {
  grep -ho "isolated-app://[a-z0-9]\{40,\}" "$profile/Default/Preferences" 2>/dev/null | head -1
}

id="$(app_id)"
if [ -z "$id" ]; then
  echo "installing the app into $profile (one time)…"
  "$chrome" --user-data-dir="$profile" \
    --enable-features=IsolatedWebApps,IsolatedWebAppDevMode \
    --install-isolated-web-app-from-url="http://127.0.0.1:$port/" \
    --no-first-run --no-default-browser-check about:blank \
    >/dev/null 2>&1 &
  for _ in $(seq 1 30); do
    sleep 1
    id="$(app_id)"
    [ -n "$id" ] && break
  done
  pkill -f "user-data-dir=$profile" 2>/dev/null || true
  sleep 2
fi

if [ -z "$id" ]; then
  echo "install failed: no isolated-app origin in $profile/Default/Preferences"
  exit 1
fi

echo "opening $id/?placeId=$place"
exec "$chrome" --user-data-dir="$profile" \
  --enable-features=IsolatedWebApps,IsolatedWebAppDevMode \
  --no-first-run --no-default-browser-check \
  "$id/?placeId=$place"
