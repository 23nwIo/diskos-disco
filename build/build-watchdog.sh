#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 diskOS contributors
# Build diskos-watchdog, the open-source replacement for the stock cmd_watchdog (static 32-bit little-endian MIPS).
#   CROSS=/path/to/mipsel-linux-musl-  build/build-watchdog.sh [OUT]
# Default output: build/out/diskos-watchdog. Never pass -D overrides (device path, test hooks) for a device build.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src/watchdog"
OUT=${1:-"$HERE/out/diskos-watchdog"}
CROSS=${CROSS:-mipsel-linux-musl-}
mkdir -p "$(dirname "$OUT")"
"${CROSS}gcc" -Os -static -Wall -Wextra -Werror -ffile-prefix-map="$SRC/"= -o "$OUT" "$SRC/main.c" "$SRC/watchdog.c"
"${CROSS}strip" "$OUT" || true
chmod 755 "$OUT"
sha256sum "$OUT"
