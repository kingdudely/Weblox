#!/usr/bin/env bash
# Run the already-built client. Override any of these in the environment.
set -euo pipefail
cd "$(dirname "$0")"
export WC_COOKIE_FILE="${WC_COOKIE_FILE:-/home/john/mizg.txt}"
export WC_WATCHDOG_MS="${WC_WATCHDOG_MS:-30000}"
exec ./client/client "${1:-${WC_PLACE:-189707}}"
