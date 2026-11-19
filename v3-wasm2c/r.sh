#!/bin/sh
# One join attempt, logged and summarised.  usage: sh r.sh [seconds] [tag]
# The interesting markers are grepped out at the end so a run can be judged
# without opening the log.
sec=${1:-60}
tag=${2:-run}
log=/tmp/opencode/${tag}.log
mkdir -p /tmp/opencode
WC_PLACE=${WC_PLACE:-1818}
WC_COOKIE_FILE=${WC_COOKIE_FILE:-/home/john/king.txt}
WC_LOOP_MS=$((sec * 1000))
export WC_PLACE WC_COOKIE_FILE WC_LOOP_MS
timeout $((sec + 40)) ./luau_probe2 900 > "$log" 2>&1
rc=$?
printf 'rc=%s log=%s\n' "$rc" "$log"
grep -ac 'Tick ' "$log" | sed 's/^/ticks=/'
grep -aE 'submitStart|scrips|join: init|EXPERIENCE JOINED|JOIN FAILED|\[http\]|\[rna\]|RBXCRASH|\[assert\]|\[trap\]|\[segv\]|\[ci\]|\[rt2' "$log" | tail -12
