#!/usr/bin/env bash
# Download the Stage A engine library from a GitHub Release.
#
#   ENGINE_RUN_ID=45927310475 tools/ci/fetch_engine.sh
#   # or omit ENGINE_RUN_ID to use the newest ci-* release
#
# Writes the assets into build/ci/engine/<tag>/ and points build/ci/engine_dir
# at it, so tools/ci/host.sh picks it up automatically.  This is a convenience
# cache, not a build dependency: tools/ci/engine.sh works on any machine.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

REPO=${ENGINE_REPO:-kingdudely/Weblox}
RUN_ID=${ENGINE_RUN_ID:-}
if [[ -z $RUN_ID ]]; then
  command -v gh >/dev/null 2>&1 || {
    echo "fetch_engine.sh: set ENGINE_RUN_ID or install gh" >&2
    exit 2
  }
  RUN_ID=$(gh release view --repo "$REPO" --json tagName --jq '.tagName' \
    2>/dev/null | sed -n 's/^ci-//p' | head -1)
fi
[[ -n $RUN_ID ]] || { echo "fetch_engine.sh: no engine release found" >&2; exit 2; }

TAG="ci-$RUN_ID"
DEST="$CI_BUILD_DIR/engine/$TAG"
mkdir -p "$DEST"

if command -v gh >/dev/null 2>&1; then
  gh release download "$TAG" --repo "$REPO" --dir "$DEST" \
    --pattern 'libweblox_engine.a' --pattern 'WC.h' \
    --pattern 'manifest.json' --pattern 'env.sh' --clobber || {
      echo "fetch_engine.sh: gh release download failed; check ENGINE_REPO/ENGINE_RUN_ID" >&2
      exit 2
    }
else
  API="https://api.github.com/repos/$REPO/releases/tags/$TAG"
  curl -fL --retry 3 "$API" \
    | grep -o '"browser_download_url": *"[^"]*"' \
    | cut -d'"' -f4 \
    | while read -r url; do
        case "$url" in
          *libweblox_engine.a|*WC.h|*manifest.json|*env.sh)
            ci_download "$url" "$DEST/$(basename "$url")" ;;
        esac
      done
fi

[[ -f $DEST/env.sh ]] || {
  echo "fetch_engine.sh: release $TAG has no engine assets" >&2
  exit 2
}
printf '%s\n' "$DEST" > "$CI_BUILD_DIR/engine_dir"
ci_metric engine_release "$TAG"
ci_metric engine_dir "$DEST"
ci_metric lib_bytes "$(wc -c < "$DEST/libweblox_engine.a" | tr -d ' ')"
ci_finish
echo "[fetch] engine ready: $DEST"
