#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds only the app (no tests, no benchmarks) and starts it - for trying Lumaflip.
# Usage: tools/run.sh [file or folder ...]
#   LF_RUN_BUILD_TYPE=Debug tools/run.sh   (default: RelWithDebInfo, build dir build/run)
set -euo pipefail
cd "$(dirname "$0")/.."

[[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt 2>/dev/null || echo /opt/homebrew/opt/qt)}"
BUILD_DIR="build/run"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="${LF_RUN_BUILD_TYPE:-RelWithDebInfo}" \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DLF_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD_DIR" --target Lumaflip

APP="$BUILD_DIR/src/Lumaflip.app"
if [[ $# -gt 0 ]]; then
  open -a "$PWD/$APP" "$@"
else
  open "$APP"
fi
echo "Gestartet: $APP"
