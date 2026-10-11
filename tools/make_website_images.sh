#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders the website's images from the app icon (packaging/macos/icon/lumaflip.svg):
#   website/favicon.png (64), website/apple-touch-icon.png (180), website/images/icon.png (512)
# and the link preview website/images/social-preview.png (1280 x 640) from the template
# packaging/brand/social-preview.svg. Only needed after changing the icon or the template; the
# PNGs are committed. Requires resvg (brew install resvg); the preview's text uses the system's
# Helvetica Neue, so run it on macOS for identical results.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:$PATH"

SVG=packaging/macos/icon/lumaflip.svg
PREVIEW=packaging/brand/social-preview.svg
command -v resvg >/dev/null || { echo "resvg missing (brew install resvg)" >&2; exit 1; }

resvg -w 64 -h 64 "$SVG" website/favicon.png
resvg -w 180 -h 180 "$SVG" website/apple-touch-icon.png
resvg -w 512 -h 512 "$SVG" website/images/icon.png
resvg -w 1280 -h 640 "$PREVIEW" website/images/social-preview.png
echo "wrote website/favicon.png, website/apple-touch-icon.png, website/images/icon.png, website/images/social-preview.png"
