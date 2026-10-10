#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads Sparkle 2 (MIT, https://sparkle-project.org) for automatic updates
# (SPEC 4.12 F-MAC-08, 10.1) into third_party/Sparkle/ (git-ignored):
#
#   third_party/Sparkle/Sparkle.framework   linked and embedded into Lumaflip.app
#   third_party/Sparkle/bin/                generate_appcast, sign_update, generate_keys
#   third_party/Sparkle/LICENSE             bundled with the app (About dialog)
#
# Usage: tools/fetch_sparkle.sh [--force]
#   --force   re-download even if the pinned version is already present
#
# The archive is pinned by version and SHA-256. The hash below is the one GitHub
# publishes for the release asset (API field "digest" of Sparkle-<version>.tar.xz,
# https://github.com/sparkle-project/Sparkle/releases/tag/<version>); the download is
# rejected on any mismatch. Sparkle does not sign the archive itself; its framework is
# ad-hoc signed and re-signed by tools/package.sh.
# Updating: change SPARKLE_VERSION and SPARKLE_SHA256 together, run this script, then
# tools/package.sh and the update test in docs/MANUAL_CHECKS.md.
#
# Nothing outside third_party/Sparkle and a fresh mktemp directory is touched; an
# existing third_party/Sparkle is only replaced if this script created it.
set -euo pipefail
cd "$(dirname "$0")/.."

SPARKLE_VERSION="2.10.0"
SPARKLE_SHA256="c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c"
URL="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"

DEST="third_party/Sparkle"
STAMP_NAME=".lumaflip-sparkle"
STAMP="$DEST/$STAMP_NAME"
WANT="$SPARKLE_VERSION $SPARKLE_SHA256"

FORCE=0
case "${1:-}" in
  --force) FORCE=1 ;;
  "") ;;
  -h|--help) sed -n '3,22p' "$0"; exit 0 ;;
  *) echo "fetch_sparkle.sh: unknown option $1" >&2; exit 2 ;;
esac

die() { printf 'fetch_sparkle.sh: %s\n' "$*" >&2; exit 1; }

if [[ $FORCE == 0 && -f "$STAMP" && "$(cat "$STAMP")" == "$WANT" && -d "$DEST/Sparkle.framework" ]]; then
  echo "Sparkle $SPARKLE_VERSION already in $DEST"
  exit 0
fi
if [[ -e "$DEST" && ! -f "$STAMP" ]]; then
  die "$DEST exists but was not created by this script - move it away and run again"
fi

mkdir -p third_party
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-sparkle.XXXXXX")
cleanup() { [[ -n "${WORK:-}" && -d "$WORK" ]] && rm -r -- "$WORK"; }
trap cleanup EXIT

echo "Downloading Sparkle $SPARKLE_VERSION"
curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
  --output "$WORK/sparkle.tar.xz" "$URL"
actual=$(shasum -a 256 "$WORK/sparkle.tar.xz" | awk '{print $1}')
[[ "$actual" == "$SPARKLE_SHA256" ]] || die "SHA-256 mismatch: expected $SPARKLE_SHA256, got $actual"
echo "SHA-256 ok: $actual"

mkdir "$WORK/x"
tar -xJf "$WORK/sparkle.tar.xz" -C "$WORK/x" --no-same-owner
[[ -d "$WORK/x/Sparkle.framework" && -x "$WORK/x/bin/generate_appcast" && -f "$WORK/x/LICENSE" ]] \
  || die "unexpected archive layout"
codesign --verify --deep --strict "$WORK/x/Sparkle.framework" || die "Sparkle.framework signature invalid"
fw_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  "$WORK/x/Sparkle.framework/Versions/B/Resources/Info.plist")
[[ "$fw_version" == "$SPARKLE_VERSION" ]] || die "framework version $fw_version != $SPARKLE_VERSION"
printf '%s\n' "$WANT" > "$WORK/x/$STAMP_NAME"

# Replace an older copy created by this script (checked above: it carries the stamp).
if [[ -d "$DEST" && ! -L "$DEST" && -f "$STAMP" ]]; then
  mv "$DEST" "$WORK/old"
fi
mv "$WORK/x" "$DEST"
echo "Sparkle $SPARKLE_VERSION in $DEST (framework, bin/, LICENSE)"
