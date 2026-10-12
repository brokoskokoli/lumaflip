#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Takes every screenshot of the README and the website and exports them
# (docs/RELEASING.md "Screenshots"). OPENS WINDOWS for several minutes: run it in CI
# (.github/workflows/screenshots.yml) or on a Mac nobody is working on.
#
#   tools/screenshots/make_screenshots.sh [<lf_screenshots binary>]
#
# Environment: LF_SHOT_PHOTOS (default build/sample-photos, filled by
# tools/fetch_sample_photos.py), LF_SHOT_OUT (default build/screenshots), LF_SHOT_LANGS
# (default "en de"). Masters go to $LF_SHOT_OUT/masters, the exported files to docs/images
# and website/images/shots (tools/screenshots/export_images.sh).
set -uo pipefail
cd "$(dirname "$0")/../.."
TOOL="${1:-build/screenshot-tool/tools/screenshots/lf_screenshots}"
PHOTOS="${LF_SHOT_PHOTOS:-build/sample-photos}"
OUT="${LF_SHOT_OUT:-build/screenshots}"
LANGS="${LF_SHOT_LANGS:-en de}"
[[ -x "$TOOL" ]] || { echo "make_screenshots.sh: $TOOL not built (cmake --build <dir> --target lf_screenshots)" >&2; exit 1; }
[[ -d "$PHOTOS" ]] || { echo "make_screenshots.sh: no photos in $PHOTOS (tools/fetch_sample_photos.py)" >&2; exit 1; }

rm -rf "$OUT/masters"
mkdir -p "$OUT/masters"
# Which shot in which appearance: the hero in both, the gallery mixed, the demo dark (photos
# look best on the dark background).
LIGHT="viewer,info,grid,map,preferences"
DARK="viewer,culling,compare,demo"
for lang in $LANGS; do
  "$TOOL" --photos "$PHOTOS" --out "$OUT/masters" --lang "$lang" --appearance light --shots "$LIGHT"
  "$TOOL" --photos "$PHOTOS" --out "$OUT/masters" --lang "$lang" --appearance dark --shots "$DARK"
done

# Everything but the folder map is required (the map needs MapKit tiles from the network and
# the Screen Recording permission for the window capture).
missing=0
for lang in $LANGS; do
  for f in viewer-$lang-light viewer-$lang-dark info-$lang-light grid-$lang-light preferences-$lang-light \
           culling-$lang-dark compare-$lang-dark; do
    [[ -f "$OUT/masters/$f.png" ]] || { echo "missing: $f.png" >&2; missing=1; }
  done
  [[ -f "$OUT/masters/demo-$lang-dark/frames.txt" ]] || { echo "missing: demo-$lang-dark" >&2; missing=1; }
  [[ -f "$OUT/masters/map-$lang-light.png" ]] || echo "note: no folder map shot for $lang"
done
[[ $missing == 0 ]] || exit 1

tools/screenshots/export_images.sh "$OUT/masters"
