#!/usr/bin/env bash
# Build a self-contained static libquiche (with BoringSSL) for the native host.
#
# The Rust `quiche` crate 0.30.1 is the exact version used by the proven
# tools/quiche-probe. Its crate-type includes `staticlib`/`cdylib` and it ships
# the C API in include/quiche.h, so we can build the C API as one static
# archive and link it straight into the C++ host with g++.
#
# Two small, mechanical patches are applied to a *copy* of the crate in /tmp:
#   1. quiche 0.30.1 gates `#[cfg(feature = "ffi")]` behind the optional
#      `cdylib-link-lines` build dependency, which is not in our offline cargo
#      cache. That dependency only emits macOS cdylib link flags; a Linux
#      staticlib does not need it, so the feature is made self-contained.
#   2. The Rust H3 API has `Config::set_additional_settings()` but the C API
#      does not expose it. Roblox needs the four draft-02 WebTransport
#      settings, so we add a tiny `extern "C"` wrapper and its header decl.
#
# Nothing is written into the repository; all outputs live under $PREFIX.
set -euo pipefail

QUICHE_VERSION="${QUICHE_VERSION:-0.30.1}"
# WC_QUICHE_DIR is the canonical name (see host/Makefile and host/BUILD.md);
# QUICHE_PREFIX remains as a backwards-compatible alias.
PREFIX="${WC_QUICHE_DIR:-${QUICHE_PREFIX:-/tmp/weblox-quiche}}"
WORK="$PREFIX/build"
CARGO_HOME_DIR="${CARGO_HOME:-$HOME/.cargo}"

if [ -n "${QUICHE_CRATE_DIR:-}" ]; then
  CRATE_SRC="$QUICHE_CRATE_DIR"
else
  CRATE_SRC=""
  for d in "$CARGO_HOME_DIR"/registry/src/*/quiche-"$QUICHE_VERSION"; do
    if [ -d "$d" ]; then CRATE_SRC="$d"; break; fi
  done
fi
if [ -z "$CRATE_SRC" ]; then
  echo "build_quiche.sh: quiche $QUICHE_VERSION source not found in $CARGO_HOME_DIR/registry/src" >&2
  echo "  set QUICHE_CRATE_DIR=/path/to/quiche-$QUICHE_VERSION or run:" >&2
  echo "  cargo fetch in tools/quiche-probe to populate the registry cache" >&2
  exit 1
fi

mkdir -p "$PREFIX/include" "$PREFIX/lib" "$WORK/src"

# Only copy once; the marker makes repeated builds incremental.
if [ ! -f "$WORK/.patched-$QUICHE_VERSION" ]; then
  rm -rf "$WORK/quiche-src" "$WORK/src"
  cp -R "$CRATE_SRC" "$WORK/quiche-src"
  mkdir -p "$WORK/src"
  python3 - "$WORK/quiche-src" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1]).resolve()

# (1) Drop the unavailable optional macOS-only build dependency.
cargo = root / "Cargo.toml"
s = cargo.read_text()
s = s.replace('ffi = ["dep:cdylib-link-lines"]', 'ffi = []')
s = re.sub(r'\[build-dependencies\.cdylib-link-lines\][\s\S]*?(?=\n\[|\Z)', '', s)
cargo.write_text(s)

build = root / "src" / "build.rs"
s = build.read_text()
s = s.replace('    #[cfg(feature = "ffi")]\n'
              '    if target_os != "windows" {\n'
              '        cdylib_link_lines::metabuild();\n'
              '    }\n', '')
build.write_text(s)

# (2) Expose H3 additional settings through the C ABI.
ffi = root / "src" / "h3" / "ffi.rs"
s = ffi.read_text()
marker = 'quiche_h3_config_set_additional_settings'
if marker not in s:
    s += '''
// Weblox extension: quiche 0.30.1 exposes set_additional_settings() in Rust
// but not through the C ABI. Used for the WebTransport draft-02 SETTINGS.
#[no_mangle]
pub extern "C" fn quiche_h3_config_set_additional_settings(
    config: &mut h3::Config,
    keys: *const u64,
    values: *const u64,
    len: size_t,
) -> c_int {
    if keys.is_null() || values.is_null() {
        return -1;
    }
    let settings = unsafe {
        slice::from_raw_parts(keys, len)
            .iter()
            .zip(slice::from_raw_parts(values, len))
            .map(|(&k, &v)| (k, v))
            .collect::<Vec<_>>()
    };
    match config.set_additional_settings(settings) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}
'''
ffi.write_text(s)

header = root / "include" / "quiche.h"
s = header.read_text()
needle = ('void quiche_h3_config_enable_extended_connect(quiche_h3_config *config, '
          'bool enabled);')
if marker not in s:
    assert needle in s, "unexpected quiche.h layout"
    s = s.replace(needle, needle + '''

// Weblox extension (see host/build_quiche.sh): send arbitrary H3 SETTINGS.
// Returns 0 on success, -1 on invalid/duplicate/prohibited identifiers.
int quiche_h3_config_set_additional_settings(quiche_h3_config *config,
                                             const uint64_t *keys,
                                             const uint64_t *values,
                                             size_t len);''')
header.write_text(s)
PY
  cat > "$WORK/Cargo.toml" <<'EOF'
[package]
name = "weblox-quiche-static"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]
path = "src/lib.rs"

[dependencies]
quiche = { path = "quiche-src", features = ["ffi"] }

[profile.release]
debug = false
panic = "abort"
EOF
  cat > "$WORK/src/lib.rs" <<'EOF'
// Pull the patched quiche crate (including all #[no_mangle] C API symbols and
// its statically linked BoringSSL) into one archive for the C++ host.
#[no_mangle]
pub extern "C" fn weblox_quiche_static_keepalive() -> u32 {
    (quiche::Config::new(quiche::PROTOCOL_VERSION).is_ok() &&
     quiche::h3::Config::new().is_ok()) as u32
}
EOF
  touch "$WORK/.patched-$QUICHE_VERSION"
fi

# Offline first; fall back to a normal build if crates are not cached yet.
if ! (cd "$WORK" && CARGO_TARGET_DIR="$PREFIX/target" \
        CARGO_NET_OFFLINE=true cargo build --release --offline); then
  echo "build_quiche.sh: offline build failed; retrying with network" >&2
  (cd "$WORK" && CARGO_TARGET_DIR="$PREFIX/target" cargo build --release)
fi

cp "$WORK/quiche-src/include/quiche.h" "$PREFIX/include/quiche.h"
ARCHIVE="$PREFIX/target/release/libweblox_quiche_static.a"
cp "$ARCHIVE" "$PREFIX/lib/libweblox_quiche.a"
if command -v strip >/dev/null 2>&1; then
  strip --strip-debug "$PREFIX/lib/libweblox_quiche.a" || true
fi

cat > "$PREFIX/link.mk" <<EOF
WEBLOX_QUICHE_PREFIX=$PREFIX
WEBLOX_QUICHE_CXXFLAGS=-I$PREFIX/include
WEBLOX_QUICHE_LDFLAGS=-L$PREFIX/lib -lweblox_quiche -ldl -lpthread -lm
EOF

echo "libquiche (quiche $QUICHE_VERSION + BoringSSL) built in $PREFIX"
echo "  include: $PREFIX/include/quiche.h"
echo "  archive: $PREFIX/lib/libweblox_quiche.a ($(du -h "$PREFIX/lib/libweblox_quiche.a" | cut -f1))"
echo "  CXXFLAGS: -I$PREFIX/include"
echo "  LDFLAGS:  -L$PREFIX/lib -lweblox_quiche -ldl -lpthread -lm"
