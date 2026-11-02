#!/usr/bin/env bash
# Run the client as an Isolated Web App.
#
#   ./tools/run_iwa.sh                 # stage, serve, open
#   PLACE=12345 ./tools/run_iwa.sh     # different place
#
# First run installs the app into the profile below; a change to the manifest
# reinstalls it, because permissions policy and permissions_policy are read at
# install time and nothing else. Source changes are live: the dev-mode proxy
# forwards every request to rkhost.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
port="${PORT:-8770}"
profile="${WEBLOX_IWA_PROFILE:-$HOME/.weblox-iwa}"
place="${PLACE:-13569002876}"
engine="${ENGINE_DIR:-/tmp/opencode/rw}"
wasm="${WASM:-$here/build/WebClient.patched.wasm}"
chrome="${CHROME:-google-chrome}"

manifest="$here/iwa/.well-known/manifest.webmanifest"
stamp="$profile/.weblox-manifest"

"$here/tools/stage_engine.sh"

if ! curl -sf -o /dev/null "http://127.0.0.1:$port/.well-known/manifest.webmanifest"; then
  "$here/app/rkhost/target/debug/rkhost" --root "$here/iwa" --port "$port" \
    > /tmp/opencode/rkhost-iwa.log 2>&1 &
  server=$!
  trap 'kill $server 2>/dev/null || true' EXIT
  sleep 1
fi

app_id() {
  grep -ho "isolated-app://[a-z0-9]\{40,\}" "$profile/Default/Preferences" 2>/dev/null | head -1
}

wanted="$(sha1sum "$manifest" | cut -d' ' -f1)"
have="$(cat "$stamp" 2>/dev/null || true)"

if [ -z "$(app_id)" ] || [ "$wanted" != "$have" ]; then
  echo "installing the app into $profile (one time, or after a manifest change)…"
  "$chrome" --user-data-dir="$profile" \
    --enable-features=IsolatedWebApps,IsolatedWebAppDevMode \
    --install-isolated-web-app-from-url="http://127.0.0.1:$port/" \
    --no-first-run --no-default-browser-check about:blank \
    >/dev/null 2>&1 &
  installer=$!
  for _ in $(seq 1 30); do
    sleep 1
    [ -n "$(app_id)" ] && break
  done
  # Only the instance this script started: the profile path appears in the
  # script's own command line, so a pattern kill would take the shell with it.
  kill "$installer" 2>/dev/null || true
  sleep 2
  mkdir -p "$profile"
  printf '%s\n' "$wanted" > "$stamp"
fi

id="$(app_id)"
if [ -z "$id" ]; then
  echo "install failed: no isolated-app origin in $profile/Default/Preferences"
  exit 1
fi

echo "opening $id/?placeId=$place"
exec "$chrome" --user-data-dir="$profile" \
  --enable-features=IsolatedWebApps,IsolatedWebAppDevMode \
  --no-first-run --no-default-browser-check \
  "$id/?placeId=$place"
