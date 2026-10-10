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
#   --keep-work          keep the staging directory (signed app + DMG root) for
#                        inspection; its path is printed at the end. Default: removed
#
# Environment:
#   LF_SIGN_IDENTITY   codesign identity, e.g. "Developer ID Application: Name (TEAMID)".
#                      Default "-" = ad-hoc signature (local builds; Gatekeeper will
#                      reject the app on other Macs).
#   LF_SIGN_KEYCHAIN   optional keychain that holds the identity (CI)
#   LF_NOTARY_*        passed through to tools/notarize.sh
#
# Never deletes anything outside its own fresh temporary directory: staging happens in
# a new mktemp directory (removed on exit unless --keep-work), results are (over)written
# in dist/.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

SKIP_BUILD=0; NOTARIZE=0; NOTARIZE_DRY=0; KEEP_WORK=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1 ;;
    --notarize) NOTARIZE=1 ;;
    --notarize-dry-run) NOTARIZE=1; NOTARIZE_DRY=1 ;;
    --keep-work) KEEP_WORK=1 ;;
    -h|--help) sed -n '3,28p' "$0"; exit 0 ;;
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
  # Explicit: optimized (-O3 -DNDEBUG), no sanitizers, no tests - even if the cache in
  # build/release was configured differently before.
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DLF_WERROR=ON -DLF_SANITIZE=OFF -DLF_BUILD_TESTS=OFF -DQT_NO_PRIVATE_MODULE_WARNING=ON >/dev/null
  cmake --build "$BUILD_DIR"
fi
BUILT_APP="$BUILD_DIR/src/Lumaflip.app"
[[ -x "$BUILT_APP/Contents/MacOS/Lumaflip" ]] || die "$BUILT_APP missing - build first"

step "Check: optimized Release build"
# The compile commands of our own sources must carry -O2/-O3 and -DNDEBUG, never -O0 or
# sanitizer flags (guards --skip-build against a Debug cache in build/release).
python3 -I - "$BUILD_DIR" "$(pwd -P)/src" <<'PY'
import json, pathlib, re, sys
build, src = pathlib.Path(sys.argv[1]), sys.argv[2]
cache = (build / "CMakeCache.txt").read_text(errors="replace")
bt = re.search(r"^CMAKE_BUILD_TYPE:\w+=(.*)$", cache, re.M)
if not bt or bt.group(1).strip() != "Release":
    sys.exit(f"package.sh: {build} is not a Release build (CMAKE_BUILD_TYPE={bt.group(1) if bt else '?'})")
cmds = json.loads((build / "compile_commands.json").read_text())
own = [c for c in cmds if c["file"].startswith(src)]
bad = []
for c in own:
    cmd = c.get("command") or " ".join(c.get("arguments", []))
    if not re.search(r"(^|\s)-O[23s](\s|$)", cmd) or "-DNDEBUG" not in cmd \
            or re.search(r"(^|\s)-O0(\s|$)|-fsanitize", cmd):
        bad.append(c["file"])
if not own or bad:
    sys.exit("package.sh: not optimized: " + (", ".join(bad[:5]) or "no compile commands for src/"))
print(f"Release, {len(own)} sources with -O2/-O3 -DNDEBUG, no sanitizers")
PY
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$BUILT_APP/Contents/Info.plist")
ARCHS=$(lipo -archs "$BUILT_APP/Contents/MacOS/Lumaflip")
echo "Lumaflip $VERSION ($ARCHS)"

# ---- 2. Stage + deploy Qt ---------------------------------------------------------
WORK_PARENT="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
WORK=$(mktemp -d "$WORK_PARENT/lumaflip-package.XXXXXX")
# Removes the staging directory on exit - only this mktemp directory: the path must be a
# real directory (no symlink) directly inside $WORK_PARENT named lumaflip-package.*.
# shellcheck disable=SC2329 # invoked by the EXIT trap
cleanup_work() {
  local rc=$?
  if [[ $KEEP_WORK == 1 ]]; then
    echo "Staging directory kept (--keep-work): ${WORK:-}"
  elif [[ -n "${WORK:-}" && -d "$WORK" && ! -L "$WORK" && "$(dirname "$WORK")" == "$WORK_PARENT" \
          && "$(basename "$WORK")" == lumaflip-package.?????? ]]; then
    rm -r -- "$WORK" || echo "package.sh: could not remove $WORK" >&2
  fi
  return "$rc"
}
trap cleanup_work EXIT
APP="$WORK/Lumaflip.app"
step "Staging in $WORK"
ditto "$BUILT_APP" "$APP"

# Qt's CMake finalizer writes its default source language ("en") into the bundle; the
# app's development language is German (UI texts, CFBundleLocalizations = de). Fixed in
# src/CMakeLists.txt (QT_I18N_SOURCE_LANGUAGE); enforced here for any older build dir.
PLIST="$APP/Contents/Info.plist"
if [[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleDevelopmentRegion' "$PLIST" 2>/dev/null)" != de ]]; then
  /usr/libexec/PlistBuddy -c 'Delete :CFBundleDevelopmentRegion' "$PLIST" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c 'Add :CFBundleDevelopmentRegion string de' "$PLIST"
  echo "CFBundleDevelopmentRegion set to de"
fi

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
[[ $KEEP_WORK == 1 ]] && echo "Signed app (staging): $APP"
exit 0
