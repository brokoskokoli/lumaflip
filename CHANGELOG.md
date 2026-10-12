<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Changelog

All notable changes to Lumaflip are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Requirement IDs in brackets
refer to [SPEC.md](SPEC.md).

## [Unreleased]

### Added

- **Previous/next arrow buttons on the image**: round, semi-transparent buttons at the left and
  right edge fade in when the mouse moves and out after 1.5 s without a move, like the title strip.
  A click flips like ←/→ (wrap, archives, right-to-left comics); no button where there is nothing
  to go to, none in the grid, compare, crop, slideshow or with the loupe. Settings → Display →
  "Show arrow buttons at the image edges" (on by default). [F-NAV-09]
- **Windows: images in the order of the Explorer window** — opening an image from Explorer
  takes over the sort column and direction (and grouping where possible) of the Explorer
  window showing its folder; on by default, can be turned off in Settings → Navigation.
  [F-SORT-04]

### Fixed

- **Windows and Linux: the sort status named the Finder** — it now says "Explorer" on Windows
  ("As in Explorer: …", "Name ↑ (Explorer view not found)") and "file manager" on Linux, as do
  the menu item and the settings. [F-SORT-05]
- **Windows and Linux: "Show in Finder"** — the File menu command, the button on the error card
  and the shortcut list now say "Show in File Explorer" on Windows and "Show in File Manager" on
  Linux; the "When opening from …" setting and the missing-permissions hint name the platform's
  file manager, too. [F-FILE-02]

## [1.2.1] - 2026-10-12

A new icon, reviewed translations in all 18 languages and an offer to become the default app.
Windows builds are now code-signed and update themselves.

### Added

- **Windows: automatic updates** with WinSparkle — "Check for Updates…" and the automatic daily
  check as on macOS, EdDSA-signed installers from a separate feed; the installer updates in place
  and restarts Lumaflip. [F-MAC-08]
- **Windows: signed releases** — installer, ZIP, `Lumaflip.exe`, the decoder helper and
  `WinSparkle.dll` are code-signed (Azure Artifact Signing) and timestamped.
- **Offer to become the default app**: opening an image of a type another app opens by default
  shows an info bar at the top of the window ("Set", "All Image Formats…", "Don't Ask Again",
  close) – once per type, only in the installed app, never in full screen or a slideshow; can be
  turned off in Settings → General. [F-MAC-09]

### Changed

- **German UI texts** follow the macOS German terminology and one term per concept
  ([translations/GLOSSARY.md](translations/GLOSSARY.md)): "Sichern" for save, "Widerrufen" for undo,
  "Benutzte Dokumente", "Markieren/Ablehnen" for pick/reject, "App" instead of "Programm",
  consistent "du". Existing translations were kept (`tools/i18n_rename_source.py`). [F-I18N-01]
- **All 17 translations reviewed** against the terms macOS, Windows and Lightroom use in each
  language (menu names, standard commands, punctuation and form of address as on the platform);
  every language is fully translated. Native-speaker reviews are still welcome
  ([translations/REVIEW_STATUS.md](translations/REVIEW_STATUS.md)). [F-I18N-01]
- **New app icon** "Sun Chevron": a forward arrow with the sun in its opening, on teal; the website
  uses the matching colours.

### Fixed

- A language switch now reaches every window, also one whose native window Qt re-created at
  that moment (some texts stayed in the old language). [F-I18N-03]
- Update notes no longer show the line breaks of the changelog source.

## [1.2.0] - 2026-10-11

Milestone M4 — the first public release: signed and notarized for macOS 15+ on Apple Silicon,
with automatic updates. Windows and Linux builds are previews.

### Added

- **Comics, archives and PDFs** (CBZ/CBR/CB7, ZIP/RAR/7z, PDF): browse them like a folder, with
  thumbnails, a right-to-left mode for manga, ⌥←/→ to the neighbouring archive and live reload
  when the archive changes; read-only, extracted only to a temporary cache. [F-OPEN-10]
- **Live Text** (⇧⌘L): select and copy text in photos, look up subjects (macOS VisionKit,
  on-device, only on request). [F-MAC-06]
- **Quick Look** (⌘Y) and **Services** ("Open in Lumaflip" from other apps). [F-MAC-07]
- **Set as desktop picture** for the current screen or all screens. [F-FILE-09]
- **Custom actions**: run a shell command or a Shortcut on the current image or selection from a
  key or the menu, confirmed once per command, with timeout. [F-FILE-10]
- **Crop** (⇧C): handles, aspect presets (free, original, 1:1, 4:3, 3:2, 16:9, portrait), rule of
  thirds, size in pixels; Enter saves into the file (undoable), "Save As…" writes a new file. JPEGs
  are cropped losslessly on the block grid; PNG/TIFF re-encoded exactly; lossy formats only after
  asking. [F-EDIT-03]
- **Export / convert** (⇧⌘E) the image or the grid selection to JPEG, HEIC, AVIF, PNG or TIFF with
  quality, maximum size, metadata (all / without GPS / none), color space, HDR gain map or SDR,
  name pattern and target folder; progress with cancel; sources are never changed. [F-EDIT-04]
- **Photo overlays**: clipping warning (J), focus peaking (⇧J), AF points from the maker notes
  (⇧A; Canon, Nikon live view, Sony, Fujifilm, EXIF subject area). [F-INFO-07]
- **Compare**: difference, blend and wipe of two images. [F-GRID-04]
- **Zoom per image for the session**: a zoomed or panned image keeps its zoom and visible part when
  you browse away and come back (per window, in memory only, up to 500 images; independent of the
  window size and full screen). Enter or "View › Reset to Standard Zoom" returns to the standard
  zoom mode; without a changed zoom Enter still renames. "Lock Zoom" takes precedence. Can be
  switched off in Preferences › Display. [F-VIEW-14]
- **Borderless windows** (new default): the image reaches the top edge; a semi-transparent strip
  with the window title and the window buttons fades in when the mouse moves and out after
  1.5 s. Drag it to move the window (Aero Snap and Snap Layouts on Windows), double-click to
  zoom (macOS follows the system setting). *Settings → Window → Title bar*: "show when the mouse
  moves" or "always show"; *Window → Minimal Window* switches a single window. [F-VIEW-13/15]
- **Linux** (preview, M4): the full app on Linux with an AppImage. Freedesktop integration (open
  with / default app via desktop entries and `mimeapps.list`, reveal in the file manager over
  D-Bus, trash with undo, inotify folder watching incl. subfolders, tags in `user.xdg.tags`),
  decoders for HEIF/AVIF (libheif), JPEG XL (libjxl), camera RAW (LibRaw) and OpenEXR, and a
  seccomp-bpf + Landlock sandbox for the decoder helper. Desktop entry, AppStream metadata,
  icons, Flatpak manifest. See [docs/LINUX.md](docs/LINUX.md). [SPEC 9.3, 13]
- **Windows 10/11 (preview)**: the app built with MSVC and Qt 6.11, with a native platform layer
  — WIC as system decoder (HEIF/AVIF/WebP/JPEG XL/RAW with the Store codec extensions), Explorer
  name order, Recycle Bin with undo, recursive folder watching, Open with, Default apps,
  wallpaper, monitor ICC profile and scRGB HDR output, the decoder helper in a low integrity
  sandbox with a job object. Installer (Inno Setup) and portable ZIP from CI; not code-signed yet
  (SmartScreen warns). See docs/WINDOWS.md. [SPEC 9.3, 13]

### Security

- Review round for M4 and the ports: decoder helper hardened on Linux (stricter seccomp filter,
  read-only file system even without Landlock, no inherited file descriptors, pixels returned in
  sealed memory) and Windows (own desktop, memory limit); pixel and memory limits for HEIF/AVIF,
  JPEG XL and OpenEXR; bounded archive scans against decompression bombs; imported settings can no
  longer bring pre-confirmed custom actions.

### Fixed

- Crop "Save As" never replaces an existing file without asking; Esc cancels a save in progress;
  custom actions on plain keys (F5, K …) work; full-screen toggles during the macOS animation are
  no longer lost; difference compare no longer blocks the window and keeps aspect ratios; clipping
  warning judged in the image's own colour space; JPEG XL colours on Linux/Windows.
- **Animated GIF, WebP, APNG and AVIF play as the file says**: the file's loop count is honored
  (a GIF without loop extension plays once and stays on its last frame; Space plays it again),
  frame delays of 10 ms or less play at 100 ms like in browsers on every platform, animated AVIF
  uses its frame delays, and a paused animation also resumes when the window is activated again
  (not only on the window state change macOS may report late). Windows: animated WebP plays
  (the WIC codec returned its frames as pages). New tests check playback on the rendered
  canvas, not only in the player. [F-FMT-03, F-IN-06]

## [1.1.0] - internal milestone (not published)

Milestone M3. Acceptance testing: [docs/TESTING-M3.md](docs/TESTING-M3.md).

### Added

- **18 languages**: English, German, French, Spanish, Italian, Portuguese (Brazil and Portugal),
  Dutch, Polish, Swedish, Danish, Norwegian Bokmål, Finnish, Czech, Japanese, Chinese (Simplified and
  Traditional), Korean. Follows the system language (including macOS per-app language), can be fixed
  in *Settings → General*, switches without restart; numbers, sizes and dates follow the locale.
  Translations are machine-made and not yet reviewed. [F-I18N-01…06]
- **Culling mode** (⌘K): 0–5 stars, pick (`P`), reject (`X`), clear (`U`), auto-advance after
  rating, rating keys also during a slideshow. [F-CULL-01/03/06, F-SLIDE-03]
- **Ratings stored where other apps find them**: XMP sidecar (Lightroom, Capture One, darktable),
  embedded XMP (JPEG, HEIC, TIFF, PNG, DNG; atomic, no pixel re-encoding) and/or Finder tags.
  [F-CULL-02]
- Rating filters (picks, ≥ n stars, unrated, rejects) and bulk actions: move all rejected to the
  Trash or to `_Rejected/`. [F-CULL-04/05]
- **Quick move/copy**: `M` / `C` + `1`–`9` into preset folders, `M M` repeats the last target,
  undo, and a Replace / Keep Both / Skip dialog on name conflicts. [F-FILE-08/11]
- **Grid view** with multi-select; **compare** 2–4 images with synchronised zoom and pan (⌥ moves one
  independently); **loupe** (hold `L`); **linked windows** that flip, zoom and pan together.
  [F-GRID-02/03, F-VIEW-11, F-WIN-07]
- **HDR/EDR**: gain-map HDR (Apple HDR, ISO 21496-1, Ultra HDR JPEG), HDR AVIF/HEIF/JXL (PQ/HLG) and
  OpenEXR with extended dynamic range on EDR displays; HDR badge, SDR preview toggle, view-only
  exposure control for EXR/HDR. [F-COL-03…05]
- Navigation: include subfolders, sibling folder (⌥← / ⌥→), go to image (⌘G), shuffle, quick filter
  by file name (⌘F). [F-OPEN-08/09, F-NAV-06/08]
- RAW+JPEG pairs optionally shown as one image. [F-FMT-06]
- GPS in the info panel with a mini map after a click and "Open in Maps"; window with all raw
  EXIF/IPTC/XMP metadata (search, copy). [F-INFO-04/05]
- **Folder map** (View → Map, ⇧⌘M): every geotagged image of the collection on a map, clustered,
  hover previews, the current image follows while flipping. Map data loads only when opened.
  [F-INFO-08]
- Always on top and a minimal window mode. [F-VIEW-12/13]
- Formats: full SVG 1.1 via resvg (Qt SVG as fallback); GIMP XCF (visible layers composited),
  Photoshop Large Document (PSB), QOI, PCX, PBM/PGM/PPM/PAM/PFM, SGI, OpenRaster, Krita.

### Security

- Non-system decoders (Qt image plugins, SVG, Lumaflip's own parsers) run in the sandboxed helper
  process `lumaflip-decoder` (no network, no writes); a crashing or hanging helper is restarted and
  the image shows an error card.

## [1.0.0] - internal milestone (not published)

Milestone M2 — a complete everyday viewer. Acceptance testing: [docs/TESTING-M2.md](docs/TESTING-M2.md).

### Added

- Live folder watching (FSEvents): new, deleted and renamed files appear immediately, the current
  image is kept. [F-OPEN-07, F-SORT-07]
- Start screen with drop area and recent items; *Open Recent* menu. [F-OPEN-11/12]
- Trackpad swipe and pinch; `Z` toggles fit ↔ 100 % at the cursor; lock zoom while flipping.
  [F-NAV-04, F-VIEW-03/05]
- Formats: PSD, OpenEXR, Radiance HDR, JPEG 2000, TGA, DDS, KTX, SVG (Qt SVG); animations (GIF,
  APNG, WebP, AVIF/HEICS, JXL) with play/pause, frame stepping and speed; multi-page TIFF/ICO/HEIF;
  RAW shown from the embedded preview first; CMYK. [F-FMT-03…05, F-COL-06]
- Colour profile follows the window to another display. [F-COL-02]
- Info panel (stays open while flipping) with file, image and camera data, histogram and Finder tags;
  filmstrip. [F-INFO-02/03/06, F-GRID-01]
- Rename, Open With / open in editor, Share, drag out to other apps, lossless JPEG rotation.
  [F-FILE-04…07, F-EDIT-02]
- Slideshow with interval, repeat, shuffle and cross-fade. [F-SLIDE-01/02]
- All shortcuts and mouse/trackpad inputs remappable with conflict detection, profile presets,
  settings export/import. [F-IN-01…18, F-SET-02/03]
- Window tiling, macOS tabs, window restoration. [F-WIN-04…06]
- "Set as default app for images" with per-type selection. [F-MAC-04]
- Opt-in live sort order from the frontmost Finder window; manual sort order remembered per folder.
  [F-SORT-04/06]
- Automatic updates via Sparkle with an EdDSA-signed appcast. [F-MAC-08]

## [0.1.0] - internal milestone (not published)

Milestone M1 — MVP. Acceptance testing: [docs/TESTING-M1.md](docs/TESTING-M1.md).

### Added

- Open by double-click, Open With, Dock, drag and drop, ⌘O and command line; one file opens its
  folder, several files form their own collection, a folder opens its first image. [F-OPEN-01…06]
- The opened image appears immediately; the folder is read and sorted in the background.
- ← / → navigation with preloading, first/last image, configurable end-of-collection behaviour,
  position display. [F-NAV-01…03/07]
- **Finder sort order** from `.DS_Store` and global Finder settings, with Finder-identical name
  sorting and visible sort status; manual sort options. [F-SORT-01/02/03/05/08]
- Zoom modes (fit, 100 %, fill width/height/window), zoom shortcuts, panning, high-quality scaling,
  Retina-correct 100 %, EXIF orientation, background choice, full screen. [F-VIEW-01/02/04/06…10]
- Formats via Apple ImageIO: JPEG, PNG, GIF, BMP, TIFF, WebP, ICO/CUR, ICNS, HEIC/HEIF, AVIF,
  JPEG XL and RAW; error card for broken files. [F-FMT-01/02]
- ICC colour management without flattening to sRGB. [F-COL-01]
- Move to Trash with undo, reveal in Finder, copy image/path, rotate and flip in the view.
  [F-FILE-01…03, F-EDIT-01]
- Multiple independent windows sharing one decoder pool and cache; native menus; light/dark mode;
  settings window. [F-WIN-01/02/03/08/09/10, F-MAC-01/02/05, F-SET-01/04]
- Graceful handling of macOS privacy prompts; no Full Disk Access required. [F-PRIV-01/02]

[Unreleased]: https://github.com/brokoskokoli/lumaflip/compare/v1.2.1...HEAD
[1.2.1]: https://github.com/brokoskokoli/lumaflip/releases/tag/v1.2.1
[1.2.0]: https://github.com/brokoskokoli/lumaflip/releases/tag/v1.2.0
