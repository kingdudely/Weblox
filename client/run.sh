#!/usr/bin/env bash
# Run the already-built client. Override any of these in the environment.
set -euo pipefail
cd "$(dirname "$0")"
export WC_COOKIE_FILE="${WC_COOKIE_FILE:-/home/john/mizg.txt}"
# Never close/reopen the window automatically: the user decides when to restart.
export WC_WATCHDOG_MS="${WC_WATCHDOG_MS:-0}"
export WC_REJOIN="${WC_REJOIN:-0}"
# Roblox/qdmux uses WebTransport draft-02; Deno's built-in implementation
# cannot send the required CONNECT header. Default to the working path.
export WC_WT_DRAFT02="${WC_WT_DRAFT02:-1}"
exec ./client/client "${1:-${WC_PLACE:-189707}}"
