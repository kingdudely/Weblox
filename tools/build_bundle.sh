#!/usr/bin/env bash
# Build the app as a signed Isolated Web App bundle.
#
#   ./tools/build_bundle.sh
#   WEBLOX_IWA_KEY=~/.weblox-iwa-key.pem ./tools/build_bundle.sh
#
# The signing key fixes the app's origin. Keep it: re-signing with another key
# creates a different app, not an update. The key is never copied into the repo.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
key="${WEBLOX_IWA_KEY:-$HOME/.weblox-iwa-key.pem}"
dist="$here/build/bundle"
wbn="$here/build/Weblox.wbn"
swbn="$here/build/Weblox.swbn"

"$here/tools/stage_engine.sh"

if [ ! -f "$key" ]; then
  echo "generating signing key: $key"
  openssl genpkey -algorithm Ed25519 -out "$key"
  chmod 600 "$key"
fi

rm -rf "$dist" "$wbn" "$swbn"
mkdir -p "$dist"
# copy -a keeps ./.well-known/manifest.webmanifest, which the IWA install needs.
cp -a "$here/iwa/." "$dist/"

id="$(npx --yes --package=wbn-sign wbn-dump-id -k "$key" -s | tail -1)"
case "$id" in
  isolated-app://*/) ;;
  *)
    echo "could not derive the bundle id (got '$id')" >&2
    exit 1
    ;;
esac

echo "bundling $(du -sh "$dist" | cut -f1) as $id"
npx --yes wbn --dir "$dist" --baseURL "$id" --output "$wbn"
npx --yes --package=wbn-sign wbn-sign -i "$wbn" -k "$key" -o "$swbn"

npx --yes --package=wbn-sign wbn-sign info "$swbn" | tail -6
echo
echo "signed: $swbn ($(du -h "$swbn" | cut -f1))"
echo "install: chrome://iwa-dev/ -> Install IWA from Signed Web Bundle"
