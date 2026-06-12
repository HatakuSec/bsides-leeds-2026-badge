#!/usr/bin/env bash
# Build the BSides Leeds 2026 badge firmware locally.
# Reports flash usage and exits non-zero on compile failure or overflow.
#
# Prerequisites:
#   brew install arduino-cli   (or see https://arduino.github.io/arduino-cli/)
#   arduino-cli core install megaTinyCore:megaavr@2.6.11
#
# Usage:
#   ./scripts/build-firmware.sh

set -euo pipefail

FQBN="megaTinyCore:megaavr:atxy4:chip=814"
MAX=8192
WARN_THRESHOLD=32

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$REPO_ROOT/firmware.ino"

if [ ! -f "$SRC" ]; then
  echo "ERROR: firmware.ino not found at $SRC" >&2
  exit 1
fi

# Prepare sketch folder (Arduino CLI requires .ino inside a matching folder name).
BUILD_DIR="$REPO_ROOT/.build/firmware"
mkdir -p "$BUILD_DIR"
cp "$SRC" "$BUILD_DIR/firmware.ino"

OUT_DIR="$REPO_ROOT/.build/out"
mkdir -p "$OUT_DIR"

echo "Compiling with FQBN: $FQBN"
echo "Source: $SRC"
echo ""

BUILD_LOG="$REPO_ROOT/.build/build.log"

rc=0
if ! arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT_DIR" --warnings all \
     --build-property "build.mrelax=-mrelax" \
     "$BUILD_DIR" \
     > "$BUILD_LOG" 2>&1; then
  rc=$?
fi

# Parse flash usage.
used=0
free=0
st="unknown"

if grep -qE 'Sketch uses [0-9]+ bytes' "$BUILD_LOG"; then
  used=$(grep -oE 'Sketch uses [0-9]+' "$BUILD_LOG" | grep -oE '[0-9]+')
  free=$((MAX - used))
  st="PASS"
elif grep -qE 'overflowed by [0-9]+ bytes' "$BUILD_LOG"; then
  over=$(grep -oE 'overflowed by [0-9]+' "$BUILD_LOG" | grep -oE '[0-9]+')
  used=$((MAX + over))
  free=$((-over))
  st="OVERFLOW"
else
  st="BUILD ERROR"
fi

pct=$(awk "BEGIN{printf \"%.1f\", $used*100/$MAX}")

# Warn if tight.
if [ "$free" -ge 0 ] && [ "$free" -lt "$WARN_THRESHOLD" ]; then
  st="WARN (< ${WARN_THRESHOLD} bytes free)"
fi

echo "────────────────────────────────────────"
echo "  Flash used : ${used} / ${MAX} bytes (${pct}%)"
echo "  Flash free : ${free} bytes"
echo "  Status     : ${st}"
echo "────────────────────────────────────────"

if [ "$rc" -ne 0 ]; then
  echo ""
  echo "BUILD FAILED — compiler output:"
  cat "$BUILD_LOG"
  exit "$rc"
fi

if [ "$free" -lt "$WARN_THRESHOLD" ] && [ "$free" -ge 0 ]; then
  echo ""
  echo "⚠  WARNING: fewer than ${WARN_THRESHOLD} bytes free. Adding more code is risky."
fi

echo ""
echo "Artifacts:"
echo "  $OUT_DIR/firmware.ino.hex"
echo "  $OUT_DIR/firmware.ino.bin"
echo ""
echo "Done."
