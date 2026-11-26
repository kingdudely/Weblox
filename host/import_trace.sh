#!/bin/sh
# Thin wrapper: parse HOST_TRACE=1 logs for import slots and signatures.
#   host/import_trace.sh imports-join-trace.log
#   host/import_trace.sh --all-stubs imports-join-trace.log
#   host/import_trace.sh --stub-catalog
exec python3 "$(dirname "$0")/import_trace.py" "$@"
