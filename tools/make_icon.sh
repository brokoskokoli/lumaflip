#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders packaging/macos/icon/lumaflip.svg into packaging/macos/icon/Lumaflip.icns.
# Only needed after changing the SVG; the .icns is committed.
# Requires rsvg-convert (brew install librsvg) and iconutil (Xcode Command Line Tools).
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:$PATH"

SVG=packaging/macos/icon/lumaflip.svg
OUT=packaging/macos/icon/Lumaflip.icns
command -v rsvg-convert >/dev/null || { echo "rsvg-convert missing (brew install librsvg)" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-icon.XXXXXX")
set_dir="$work/Lumaflip.iconset"
mkdir "$set_dir"
for size in 16 32 128 256 512; do
  rsvg-convert -w "$size" -h "$size" "$SVG" -o "$set_dir/icon_${size}x${size}.png"
  rsvg-convert -w $((size * 2)) -h $((size * 2)) "$SVG" -o "$set_dir/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$set_dir" -o "$OUT"
echo "wrote $OUT ($(stat -f %z "$OUT") bytes); iconset in $set_dir"
