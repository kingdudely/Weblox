#!/usr/bin/env bash
# Platform names for prebuilt engine assets.  Sourced by build scripts.
# Runs on Linux, macOS and Windows (Git Bash).

UNAME_S=$(uname -s 2>/dev/null || echo unknown)
UNAME_M=$(uname -m 2>/dev/null || echo unknown)

case "$UNAME_S" in
  Linux*)   PLAT_OS=linux ;;
  Darwin*)  PLAT_OS=macos ;;
  MINGW*|MSYS*|CYGWIN*) PLAT_OS=windows ;;
  *)        PLAT_OS=$(printf '%s' "$UNAME_S" | tr '[:upper:]' '[:lower:]') ;;
esac

case "$UNAME_M" in
  x86_64|amd64) PLAT_ARCH=x64 ;;
  arm64|aarch64) PLAT_ARCH=arm64 ;;
  *) PLAT_ARCH=$(printf '%s' "$UNAME_M" | tr '[:upper:]' '[:lower:]') ;;
esac

PLATFORM="$PLAT_OS-$PLAT_ARCH"

# CRLF is fine on Windows; keep objects and archives predictable.
case "$PLAT_OS" in
  windows)
    PIC_FLAG=""
    STATIC_NAME=""
    SHARED_NAME="engine-$PLATFORM.dll"
    IMPORT_NAME="engine-$PLATFORM.lib"
    SHARED_FLAG=""
    ;;
  macos)
    PIC_FLAG="-fPIC"
    STATIC_NAME=""
    SHARED_NAME="engine-$PLATFORM.dylib"
    IMPORT_NAME=""
    SHARED_FLAG="-dynamiclib"
    ;;
  *)
    PIC_FLAG="-fPIC"
    STATIC_NAME=""
    SHARED_NAME="engine-$PLATFORM.so"
    IMPORT_NAME=""
    SHARED_FLAG="-shared"
    ;;
esac

# Keep this readable for humans and AIs: every name is explicit.
printf 'PLATFORM=%s STATIC=%s SHARED=%s\n' "$PLATFORM" "$STATIC_NAME" "$SHARED_NAME" >/dev/null
