#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Turns the master PNGs of lf_screenshots (2x, window frame and shadow, transparent) into the
# files the README and the website use:
#
#   docs/images/<shot>-<lang>-<appearance>.webp     README (1600 px wide)
#   docs/images/demo-<lang>.webp                    README demo animation (960 px wide)
#   website/images/shots/<shot>-<lang>-<appearance>.webp   website (1280 px wide)
#   website/images/shots/<shot>-<lang>-<appearance>.png    fallback for <picture> (960 px, 256 colours)
#   website/images/shots/demo-<lang>.webp           website demo animation (1200 px wide)
#   website/images/shots/demo-<lang>.png            its first frame (poster / fallback)
#
# Usage: tools/screenshots/export_images.sh <masters dir> [<repo root>]
# Requires sips (macOS), cwebp and img2webp (brew install webp); pngquant if installed
# (brew install pngquant), otherwise the fallback PNGs are not reduced to 256 colours.
set -euo pipefail
MASTERS="${1:?usage: export_images.sh <masters dir> [<repo root>]}"
ROOT="${2:-$(cd "$(dirname "$0")/../.." && pwd)}"
[[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
for tool in sips cwebp img2webp; do
  command -v "$tool" >/dev/null || { echo "export_images.sh: $tool missing" >&2; exit 1; }
done
DOCS="$ROOT/docs/images"
SITE="$ROOT/website/images/shots"
mkdir -p "$DOCS" "$SITE"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lf-export.XXXXXX")
trap 'rm -r -- "$WORK"' EXIT

resize() { # <in.png> <width> <out.png>
  sips -Z "$2" "$1" --out "$3" >/dev/null
}
webp() { # <in.png> <quality> <out.webp>
  cwebp -quiet -q "$2" -m 6 -sharp_yuv -alpha_q 90 "$1" -o "$3"
}
quantize() { # <in.png> <out.png>
  if command -v pngquant >/dev/null; then
    pngquant --quality=55-85 --speed 1 --strip --force --output "$2" "$1"
  else
    cp "$1" "$2"
  fi
}

count=0
for master in "$MASTERS"/*.png; do
  [[ -f "$master" ]] || continue
  name=$(basename "$master" .png)
  [[ "$name" == debug-* ]] && continue # diagnostics of the tool, not for publishing
  resize "$master" 1600 "$WORK/readme.png"
  webp "$WORK/readme.png" 88 "$DOCS/$name.webp"
  resize "$master" 1280 "$WORK/site.png"
  webp "$WORK/site.png" 85 "$SITE/$name.webp"
  resize "$master" 960 "$WORK/fallback.png"
  quantize "$WORK/fallback.png" "$SITE/$name.png"
  count=$((count + 1))
done

# Demo animations: frames-<lang>-<appearance>/frames.txt lists "frame-NNN.png <ms>".
for dir in "$MASTERS"/demo-*/; do
  [[ -f "$dir/frames.txt" ]] || continue
  base=$(basename "$dir")        # demo-en-dark
  lang=$(echo "$base" | cut -d- -f2)
  for target in "readme:960:78:$DOCS/demo-$lang.webp" "site:1200:80:$SITE/demo-$lang.webp"; do
    IFS=: read -r kind width quality out <<<"$target"
    args=(-loop 0 -lossy -q "$quality" -m 6)
    while read -r frame ms; do
      [[ -n "$frame" ]] || continue
      resize "$dir/$frame" "$width" "$WORK/$kind-$frame"
      args+=(-d "$ms" "$WORK/$kind-$frame")
    done <"$dir/frames.txt"
    img2webp "${args[@]}" -o "$out" >/dev/null
  done
  first=$(head -1 "$dir/frames.txt" | cut -d' ' -f1)
  resize "$dir/$first" 1200 "$WORK/poster.png"
  quantize "$WORK/poster.png" "$SITE/demo-$lang.png"
  count=$((count + 1))
done

echo "exported $count images"
ls -l "$DOCS" "$SITE"
du -ch "$DOCS"/* "$SITE"/* | tail -1
