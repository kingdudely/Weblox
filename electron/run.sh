#!/bin/sh
# WC_COOKIE='_|WARNING...' ./run.sh <placeId>
# WC_COOKIE_FILE=/path/cookie ./run.sh <placeId>
set -e
cd "$(dirname "$0")"
export WC_PLACE="${1:-${WC_PLACE:-}}"
unset WAYLAND_DISPLAY
exec ./node_modules/.bin/electron . --ozone-platform=x11
