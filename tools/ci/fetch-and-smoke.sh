#!/usr/bin/env bash
# CI wrapper: download the newest engine Release, then smoke-link and run it.
# Dispatch with: gh workflow run heavy.yml -f task=custom \
#   -f script=tools/ci/fetch-and-smoke.sh -f upload-release=false

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

bash "$HERE/fetch_engine.sh"
bash "$HERE/smoke.sh"
ci_finish
