#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds a self-contained, signed Lumaflip.app and the release files (SPEC 10.1, 11):
#
#   dist/Lumaflip-<version>.dmg   (+ .sha256)   DMG with the app and an /Applications link
#   dist/Lumaflip-<version>.zip   (+ .sha256)   zipped app (ditto, keeps signatures)
#
# Usage: tools/package.sh [--skip-build] [--notarize] [--notarize-dry-run] [--keep-work]
#
#   --skip-build         use the existing build/release (otherwise configure + build Release)
#   --notarize           notarize + staple app and DMG via tools/notarize.sh (needs a
#                        Developer ID identity and notary credentials, see docs/RELEASING.md)
#   --notarize-dry-run   run the notarization steps as a dry run (prints the commands)
#
# Environment:
#   LF_SIGN_IDENTITY   codesign identity, e.g. "Developer ID Application: Name (TEAMID)".
#                      Default "-" = ad-hoc signature (local builds; Gatekeeper will
#                      reject the app on other Macs).
#   LF_SIGN_KEYCHAIN   optional keychain that holds the identity (CI)
#   LF_NOTARY_*        passed through to tools/notarize.sh
#
# Never deletes anything outside its own fresh temporary directory: staging happens in
# a new mktemp directory, results are (over)written in dist/.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

SKIP_BUILD=0; NOTARIZE=0; NOTARIZE_DRY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1 ;;
    --notarize) NOTARIZE=1 ;;
    --notarize-dry-run) NOTARIZE=1; NOTARIZE_DRY=1 ;;
    -h|--help) sed -n '3,24p' "$0"; exit 0 ;;
    *) echo "package.sh: unknown option $1" >&2; exit 2 ;;
  esac; shift
done

export PATH="/opt/homebrew/bin:$PATH"
QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo /opt/homebrew/opt/qt)"
BUILD_DIR="build/release"
DIST="$ROOT/dist"
IDENTITY="${LF_SIGN_IDENTITY:--}"
ENTITLEMENTS="$ROOT/packaging/macos/Lumaflip.entitlements"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mpackage.sh: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. Release build -------------------------------------------------------------
if [[ $SKIP_BUILD == 0 ]]; then
  step "Configure + build Release ($BUILD_DIR)"
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DLF_WERROR=ON -DLF_BUILD_TESTS=OFF -DQT_NO_PRIVATE_MODULE_WARNING=ON >/dev/null
  cmake --build "$BUILD_DIR"
fi
BUILT_APP="$BUILD_DIR/src/Lumaflip.app"
[[ -x "$BUILT_APP/Contents/MacOS/Lumaflip" ]] || die "$BUILT_APP missing - build first"
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUILT_APP/Contents/Info.plist")
ARCHS=$(lipo -archs "$BUILT_APP/Contents/MacOS/Lumaflip")
echo "Lumaflip $VERSION ($ARCHS)"

# ---- 2. Stage + deploy Qt ---------------------------------------------------------
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-package.XXXXXX")
APP="$WORK/Lumaflip.app"
step "Staging in $WORK"
ditto "$BUILT_APP" "$APP"

step "Deploy Qt frameworks, plugins, translations, licences"
python3 tools/macos_deploy.py "$APP" --qt-prefix "$QT_PREFIX"

step "Check: nothing references Homebrew or other foreign paths (otool -L)"
python3 tools/macos_deploy.py --verify "$APP"

# ---- 3. Sign (inside-out, no --deep) ----------------------------------------------
SIGN=(codesign --force --sign "$IDENTITY")
if [[ "$IDENTITY" == "-" ]]; then
  # Ad-hoc: no Hardened Runtime. Its library validation only accepts libraries signed by
  # the same Team ID, and ad-hoc signatures have none - the app would not start.
  SIGN+=(--timestamp=none)
  echo "Signing ad-hoc without Hardened Runtime (set LF_SIGN_IDENTITY for a Developer ID signature)"
else
  SIGN+=(--options runtime --timestamp)
  echo "Signing with Hardened Runtime: $IDENTITY"
fi
[[ -n "${LF_SIGN_KEYCHAIN:-}" ]] && SIGN+=(--keychain "$LF_SIGN_KEYCHAIN")

step "Code signing"
# Order matters: nested code first, the bundle last. Loose dylibs and plugins ...
while IFS= read -r -d '' f; do
  "${SIGN[@]}" "$f"
done < <(find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f -name '*.dylib' -print0)
# ... then each framework version (its binary + Resources/Info.plist) ...
for fw in "$APP"/Contents/Frameworks/*.framework; do
  [[ -d "$fw" ]] || continue
  for v in "$fw"/Versions/*; do
    [[ -L "$v" ]] || "${SIGN[@]}" "$v"
  done
done
# ... and finally the app with its entitlements.
"${SIGN[@]}" --entitlements "$ENTITLEMENTS" "$APP"

step "Verify signature"
codesign --verify --deep --strict --verbose=1 "$APP"
codesign -dv --verbose=2 "$APP" 2>&1 | grep -E '^(Identifier|Format|Authority|TeamIdentifier|Signature|Runtime|CodeDirectory)' || true
codesign -d --entitlements - --xml "$APP" 2>/dev/null | plutil -p - 2>/dev/null || true
echo "Gatekeeper (spctl) - expected to reject builds that are not notarized:"
spctl --assess --type execute -vvv "$APP" 2>&1 || true

# ---- 4. Notarize the app (optional) -----------------------------------------------
NOTARY_ARGS=(); [[ $NOTARIZE_DRY == 1 ]] && NOTARY_ARGS+=(--dry-run)
if [[ $NOTARIZE == 1 ]]; then
  step "Notarize app"
  tools/notarize.sh "${NOTARY_ARGS[@]+"${NOTARY_ARGS[@]}"}" "$APP"
fi

# ---- 5. ZIP + DMG -----------------------------------------------------------------
mkdir -p "$DIST"
ZIP="$DIST/Lumaflip-$VERSION.zip"
DMG="$DIST/Lumaflip-$VERSION.dmg"
step "ZIP: $ZIP"
rm -f "$ZIP" "$ZIP.sha256"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"

step "DMG: $DMG"
DMG_ROOT="$WORK/dmg"
mkdir "$DMG_ROOT"
ditto "$APP" "$DMG_ROOT/Lumaflip.app"
ln -s /Applications "$DMG_ROOT/Applications"
rm -f "$DMG" "$DMG.sha256"
hdiutil create -volname "Lumaflip $VERSION" -srcfolder "$DMG_ROOT" -fs HFS+ -format UDZO \
  -imagekey zlib-level=9 -quiet "$DMG"
if [[ "$IDENTITY" != "-" ]]; then
  DMG_SIGN=(codesign --force --sign "$IDENTITY" --timestamp)
  [[ -n "${LF_SIGN_KEYCHAIN:-}" ]] && DMG_SIGN+=(--keychain "$LF_SIGN_KEYCHAIN")
  "${DMG_SIGN[@]}" "$DMG"
  codesign --verify --strict --verbose=2 "$DMG"
fi

if [[ $NOTARIZE == 1 ]]; then
  step "Notarize DMG"
  tools/notarize.sh "${NOTARY_ARGS[@]+"${NOTARY_ARGS[@]}"}" "$DMG"
fi

# ---- 6. Checksums -----------------------------------------------------------------
step "SHA-256"
for f in "$ZIP" "$DMG"; do
  (cd "$DIST" && shasum -a 256 "$(basename "$f")" > "$(basename "$f").sha256" && cat "$(basename "$f").sha256")
done

step "Done"
du -sh "$APP" "$ZIP" "$DMG" | sed "s|$ROOT/||"
echo "Signed app (staging): $APP"
