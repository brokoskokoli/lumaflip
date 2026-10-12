#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders the Inno Setup wizard images from the app icon (packaging/macos/icon/lumaflip.svg):
#   packaging/windows/wizard/small-<w>.bmp  icon in the wizard's top-right corner
#                                           (55x55 at 100 % … 138x138 at 250 % DPI)
#   packaging/windows/wizard/large-<w>.bmp  left panel of the welcome/finish page
#                                           (164x314 at 100 % … 410x785 at 250 % DPI)
# 24-bit BMP (Inno Setup's safest format). Needs resvg and ImageMagick (brew install resvg imagemagick).
# Run after changing the icon and commit the results.
set -euo pipefail
cd "$(dirname "$0")/.."
icon=packaging/macos/icon/lumaflip.svg
out=packaging/windows/wizard
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lf-wizard.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$out"

# Small: icon on white (the wizard header is white in the modern style).
for w in 55 64 69 83 92 110 119 138; do
  resvg -w "$w" -h "$w" "$icon" "$tmp/s.png"
  magick "$tmp/s.png" -background white -alpha remove -alpha off -type TrueColor "BMP3:$out/small-$w.bmp"
done

# Large: light teal gradient (website palette) with the icon in the upper third.
for scale in 100 125 150 175 200 225 250; do
  w=$(( 164 * scale / 100 )); h=$(( 314 * scale / 100 )); s=$(( 112 * scale / 100 ))
  magick -size "${w}x${h}" gradient:'#F4FAFA'-'#CFE6E8' "$tmp/bg.png"
  resvg -w "$s" -h "$s" "$icon" "$tmp/i.png"
  magick "$tmp/bg.png" "$tmp/i.png" -gravity north -geometry "+0+$(( 56 * scale / 100 ))" -composite \
    -alpha off -type TrueColor "BMP3:$out/large-$w.bmp"
done
echo "Wizard images written to $out"
