#!/usr/bin/env bash
# Shared helpers for the heavy native build scripts.
#
# Everything here is portable: Linux/macOS, local machine or GitHub Actions.
# Nothing in this file compiles the engine; it only fetches, hashes, times and
# reports.  Source it from tools/ci/<task>.sh:
#
#   HERE=$(cd "$(dirname "$0")" && pwd)
#   # shellcheck source=common.sh
#   . "$HERE/common.sh"
#   ci_init
#
# Environment overrides (all optional):
#   WC_BUILD_DIR   where intermediates and metrics go (default build/ci)
#   WC_WASM        local wasm to use instead of downloading the pinned build
#   WASM2C         path to a local wasm2c binary (skips WABT download)
#   WABT_ROOT      path to an unpacked WABT tree (wasm2c/ runtime sources)
#   SIMDE_DIR      path to SIMDE headers, if your WABT needs them separately

set -euo pipefail

CI_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CI_ROOT=$(cd "$CI_HERE/../.." && pwd)
CI_BUILD_DIR=${WC_BUILD_DIR:-"$CI_ROOT/build/ci"}

# Pinned engine build.  Changing this is a deliberate engine update; the
# sha256 below is checked so CI can never silently build a different module.
WASM_VERSION=${WASM_VERSION:-1ef59424f70d4932}
WASM_BASE_URL="https://setup.rbxcdn.com/webclient/version-$WASM_VERSION-RobloxWebClient"
WASM_SHA256=${WASM_SHA256:-be370a4446dcfce62635f7e6d271526b15e2099507fb7250b2d3360eacd4a61f}
WABT_VERSION=${WABT_VERSION:-1.0.42}

ci_init() {
  mkdir -p "$CI_BUILD_DIR"
  CI_METRICS="$CI_BUILD_DIR/metrics.txt"
  : > "$CI_METRICS"
  CI_START=$SECONDS
}

ci_metric() {
  # Values must be single-line; sanitise so metrics.txt stays key=value.
  local value=${2//[$'\r\n']/ }
  printf '%s=%s\n' "$1" "$value" >> "$CI_METRICS"
}

ci_secs() { echo $((SECONDS - CI_START)); }

ci_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "common.sh: need sha256sum or shasum" >&2
    return 1
  fi
}

ci_nproc() {
  if command -v nproc >/dev/null 2>&1; then nproc
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu 2>/dev/null || echo 4
  else echo 4; fi
}

ci_download() {
  local url=$1 out=$2
  if ! command -v curl >/dev/null 2>&1; then
    echo "common.sh: curl is required" >&2
    return 1
  fi
  curl -fL --retry 3 --connect-timeout 20 -o "$out" "$url"
}

ci_fetch_wasm() {
  if [[ -n ${WC_WASM:-} ]]; then
    [[ -f $WC_WASM ]] || { echo "common.sh: WC_WASM=$WC_WASM not found" >&2; return 1; }
    echo "$WC_WASM"
    return 0
  fi
  local out="$CI_BUILD_DIR/WebClient-$WASM_VERSION.wasm"
  if [[ ! -f $out ]]; then
    echo "[ci] fetching pinned engine wasm ($WASM_VERSION)" >&2
    ci_download "$WASM_BASE_URL/WebClient.wasm" "$out"
  fi
  local got
  got=$(ci_sha256 "$out")
  if [[ $got != "$WASM_SHA256" ]]; then
    echo "[ci] wasm sha256 mismatch: got $got expected $WASM_SHA256" >&2
    return 1
  fi
  echo "$out"
}

ci_platform() {
  local os arch
  os=$(uname -s)
  arch=$(uname -m)
  case "$arch" in
    x86_64|amd64) arch=x64 ;;
    aarch64|arm64) arch=arm64 ;;
  esac
  case "$os" in
    Linux) echo "linux-$arch" ;;
    Darwin) echo "osx-$arch" ;;
    *) echo "$os-$arch" ;;
  esac
}

ci_fetch_wabt() {
  # Sets WASM2C, WABT_ROOT, WASM2C_INCLUDE.  A caller-provided WABT_ROOT wins.
  if [[ -n ${WABT_ROOT:-} ]]; then
    WASM2C=${WASM2C:-"$WABT_ROOT/bin/wasm2c"}
    WASM2C_INCLUDE="$WABT_ROOT/wasm2c"
  elif [[ -n ${WASM2C:-} && -n ${WABT_ROOT:-} ]]; then
    WASM2C_INCLUDE="$WABT_ROOT/wasm2c"
  else
    local plat tarball dir
    plat=$(ci_platform)
    tarball="wabt-$WABT_VERSION-$plat.tar.gz"
    dir="$CI_BUILD_DIR/$tarball"
    if [[ ! -f $dir ]]; then
      echo "[ci] fetching WABT $WABT_VERSION ($plat)" >&2
      ci_download "https://github.com/WebAssembly/wabt/releases/download/$WABT_VERSION/$tarball" "$dir"
    fi
    local root="$CI_BUILD_DIR/wabt-$WABT_VERSION-$plat"
    if [[ ! -d $root ]]; then
      mkdir -p "$root"
      tar -xzf "$dir" -C "$root" --strip-components=1
    fi
    WABT_ROOT=$root
    WASM2C="$root/bin/wasm2c"
    WASM2C_INCLUDE="$root/wasm2c"
  fi
  [[ -x $WASM2C ]] || { echo "common.sh: wasm2c not executable at $WASM2C" >&2; return 1; }
  WASM2C_VERSION=$("$WASM2C" --version 2>/dev/null || echo unknown)
  ci_metric wabt_version "$WASM2C_VERSION"
}

# Print the feature flags this wasm2c understands.  WABT enables some
# proposals by default (with --disable-X flags) and others via --enable-X;
# probing --help avoids an option mismatch across WABT releases.
ci_wasm2c_flags() {
  local bin=${1:-$WASM2C} help flags=""
  help=$("$bin" --help 2>&1 || true)
  for feature in exceptions threads function-references tail-call annotations code-metadata gc; do
    if [[ $help == *"--enable-$feature"* ]]; then
      flags="$flags --enable-$feature"
    fi
  done
  if [[ $help == *"--no-debug-names"* ]]; then
    flags="$flags --no-debug-names"
  fi
  printf '%s' "$flags"
}

ci_finish() {
  echo "[ci] metrics: $CI_METRICS"
  echo "[ci] elapsed: $(ci_secs)s"
  cat "$CI_METRICS"
}
