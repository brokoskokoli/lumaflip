#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Lumaflip-<version>-x86_64.AppImage from a configured and built Linux build tree
# (docs/LINUX.md). Used by the "Linux app" CI job; runs on any x86_64 Linux with the build
# dependencies installed. Downloads linuxdeploy + its Qt plugin (pinned by SHA-256).
#
#   tools/linux_appimage.sh <build dir> [output dir]
#
# Environment: QMAKE (Qt's qmake; default: qmake6/qmake on PATH or $QT_ROOT_DIR/bin/qmake).
#
# What goes in: `cmake --install` (bin/lumaflip, bin/lumaflip-decoder, desktop entry, AppStream
# metadata, icons, translations), Qt with the xcb, wayland and offscreen platform plugins
# (the decoder helper runs offscreen), the image format plugins, the decoder libraries and
# libheif's AV1 decoder plugin. libheif's HEVC plugin (libde265) is deliberately left out
# (HEVC patents, SPEC 11): the AppImage opens AVIF, but HEIC only via a separately
# installed decoder (see docs/LINUX.md).
set -euo pipefail

BUILD="${1:?usage: tools/linux_appimage.sh <build dir> [output dir]}"
OUT="${2:-dist}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "$BUILD" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
APPDIR="$BUILD/AppDir"
TOOLS="$BUILD/appimage-tools"
ID="io.github.brokoskokoli.lumaflip"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

fetch() { # url sha dest
  if [[ -f "$3" ]] && echo "$2  $3" | sha256sum -c --status; then return; fi
  curl -fsSL --retry 3 -o "$3.part" "$1"
  echo "$2  $3.part" | sha256sum -c --quiet
  mv "$3.part" "$3"
  chmod +x "$3"
}
mkdir -p "$TOOLS"
fetch "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA" "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA" "$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage"
export PATH="$TOOLS:$PATH"
# No FUSE needed (CI runners and containers).
export APPIMAGE_EXTRACT_AND_RUN=1

if [[ -z "${QMAKE:-}" ]]; then
  if [[ -n "${QT_ROOT_DIR:-}" && -x "$QT_ROOT_DIR/bin/qmake" ]]; then
    QMAKE="$QT_ROOT_DIR/bin/qmake"
  else
    QMAKE="$(command -v qmake6 || command -v qmake)"
  fi
fi
export QMAKE
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"

VERSION="$(sed -n 's/^project(Lumaflip VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"

echo "== install into $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr

# libheif decoder plugins: AV1 only (dav1d, else aom). Never libde265 (HEVC).
HEIF_PLUGINS="$(pkg-config --variable=libdir libheif 2>/dev/null || true)/libheif/plugins"
DEPS_ONLY=()
if [[ -d "$HEIF_PLUGINS" ]]; then
  mkdir -p "$APPDIR/usr/lib/libheif/plugins"
  for p in dav1d aomdec; do
    if [[ -f "$HEIF_PLUGINS/libheif-$p.so" ]]; then
      cp "$HEIF_PLUGINS/libheif-$p.so" "$APPDIR/usr/lib/libheif/plugins/"
      DEPS_ONLY+=("--deploy-deps-only=$APPDIR/usr/lib/libheif/plugins/libheif-$p.so")
      break
    fi
  done
fi

# Platform plugins: xcb (default), wayland and offscreen (decoder helper).
extra=()
for f in "$QT_PLUGINS"/platforms/libqoffscreen.so "$QT_PLUGINS"/platforms/libqwayland*.so; do
  [[ -f "$f" ]] && extra+=("$(basename "$f")")
done
EXTRA_PLATFORM_PLUGINS="$(IFS=';'; echo "${extra[*]}")"
export EXTRA_PLATFORM_PLUGINS
export EXTRA_QT_MODULES="svg;dbus"
echo "== extra platform plugins: $EXTRA_PLATFORM_PLUGINS"

cd "$OUT"
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/lumaflip" \
  --executable "$APPDIR/usr/bin/lumaflip-decoder" \
  --desktop-file "$APPDIR/usr/share/applications/$ID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/$ID.png" \
  "${DEPS_ONLY[@]}" \
  --plugin qt \
  --output appimage

APPIMAGE="$(ls -t "$OUT"/Lumaflip-*-x86_64.AppImage | head -n1)"
# Patent check (SPEC 11): no HEVC decoder in the image.
if find "$APPDIR" -name 'libde265*' -o -name 'libheif-libde265*' | grep -q .; then
  echo "error: libde265 ended up in the AppImage" >&2
  exit 1
fi
echo "== $APPIMAGE"
ls -l "$APPIMAGE"
