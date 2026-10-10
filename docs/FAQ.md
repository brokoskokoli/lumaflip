<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Frequently asked questions

- [Which formats are supported?](#which-formats-are-supported)
- [How does "sorted like Finder" work?](#how-does-sorted-like-finder-work)
- [What do I need to see HDR?](#what-do-i-need-to-see-hdr)
- [Why isn't Lumaflip in the Mac App Store?](#why-isnt-lumaflip-in-the-mac-app-store)
- [Why does macOS ask for access to my folders?](#why-does-macos-ask-for-access-to-my-folders)
- [What about privacy? Does Lumaflip connect to the internet?](#what-about-privacy-does-lumaflip-connect-to-the-internet)
- [Does Lumaflip change my files?](#does-lumaflip-change-my-files)
- [Can other apps read my ratings?](#can-other-apps-read-my-ratings)
- [Does it run on Intel Macs, Windows or Linux?](#does-it-run-on-intel-macs-windows-or-linux)

## Which formats are supported?

Everything below works out of the box, nothing to install.

| Group | Formats | Decoded by |
|---|---|---|
| Standard | JPEG, PNG, GIF, BMP, TIFF (multi-page), WebP, ICO/CUR, ICNS | Apple ImageIO |
| Modern | HEIC/HEIF, AVIF, JPEG XL | Apple ImageIO |
| RAW | CR2, CR3, CRW, NEF, NRW, ARW, SR2, RAF, ORF, RW2, DNG, PEF, IIQ, 3FR and more — whatever Apple's RAW engine supports | Apple ImageIO |
| Pro / HDR | PSD (composite), OpenEXR, Radiance HDR, JPEG 2000, TGA, DDS, KTX | Apple ImageIO |
| Animated | GIF, APNG, animated WebP, animated AVIF/HEICS, animated JPEG XL | Apple ImageIO + Lumaflip playback |
| Vector | SVG (full SVG 1.1 with CSS, filters and text) | resvg, Qt SVG as fallback — in the sandboxed helper |
| Others | XCF (GIMP, visible layers composited with the common layer modes), PSB (Photoshop Large Document), QOI, PCX, PBM/PGM/PPM/PAM/PFM, SGI, OpenRaster, Krita | Lumaflip's own decoders — in the sandboxed helper |

Format detection uses the file signature, not just the extension. Broken or unsupported files show
an error card ("Show in Finder") and you can keep flipping. RAW files show their embedded preview
first; the full development follows (or enable *RAW: Show embedded preview only* for maximum speed).

Known limits: XCF layer modes without an equivalent (HSV/LCH …) and GIMP 3 effects are simplified.
JPEG XR, DICOM and EPS may come later.

## How does "sorted like Finder" work?

macOS has no API for "the order this Finder window shows", so Lumaflip uses a best-effort cascade
and shows the result in the status bar (e.g. "As in Finder: Date Added ↓"):

1. **Your choice for this folder**, if you picked a sort order in *View → Sort By* (remembered per
   folder, or as the default for all folders).
2. **Live from Finder** *(opt-in, Settings → Navigation & Sorting)*: if the frontmost Finder window
   shows exactly this folder, Lumaflip asks Finder for its sort order. macOS asks once for
   Automation permission.
3. **The folder's `.DS_Store`**: the view settings Finder saved for the folder (list, icon and
   gallery view; sort column and direction; icon positions in "None" arrangement).
4. **Global Finder settings** (e.g. the default view options).
5. **Fallback: name ascending**, compared exactly like Finder (numbers sorted numerically,
   case-insensitive, locale-aware).

Limits: Finder writes `.DS_Store` with a delay, so a sort order you changed seconds ago may not be
saved yet (the live option helps). Network and USB volumes configured not to store `.DS_Store`
files, some cloud folders, "Use Groups", column view and search windows can't be reproduced
exactly.

## What do I need to see HDR?

- An **EDR-capable display**: e.g. a MacBook Pro with Liquid Retina XDR display, Pro Display XDR,
  or an external HDR display with HDR enabled in *System Settings → Displays*.
- An **HDR image**: gain-map HDR (iPhone HEIC, Apple HDR, ISO 21496-1, Ultra HDR JPEG), HDR
  AVIF/HEIF/JPEG XL (PQ/HLG) or OpenEXR / Radiance HDR.

HDR images get an HDR badge. *Show HDR* (⇧⌘H) switches between HDR and a tone-mapped SDR preview;
for EXR/HDR files the exposure control (⌥⌘+ / ⌥⌘− / ⌥⌘0) changes only the display, never the
file. On a regular (SDR) display HDR images are shown tone-mapped. Screenshots don't capture the
extra highlight brightness.

## Why isn't Lumaflip in the Mac App Store?

Two reasons:

1. **Sandbox.** App Store apps must run in the App Sandbox. A sandboxed viewer that you open by
   double-clicking an image only gets access to *that one file* — not its folder and not the
   `.DS_Store` that stores Finder's sort order. Flipping through the folder, which is the whole point
   of Lumaflip, would need an extra "grant access to this folder" dialog, and the Finder sort order
   would be largely unavailable. This is exactly the folder-permission hassle of many App Store
   viewers that Lumaflip set out to avoid.
2. **Licence.** Lumaflip is GPL-3.0-or-later; the App Store terms of use are generally considered
   incompatible with the GPL.

Instead, releases are signed with a Developer ID and **notarized by Apple**, and distributed on
GitHub Releases (and later Homebrew). The app itself is not sandboxed, but the decoders for
less common formats run in a separate, sandboxed helper process (see below).

## Why does macOS ask for access to my folders?

When you open an image in Desktop, Documents, Downloads, on a removable drive or a network volume
for the first time, macOS asks once whether Lumaflip may access that location. Lumaflip needs to
read the image's **folder** so you can flip through it. If you decline, Lumaflip shows just the
opened image and a hint with an *Allow Access…* button that opens the right System Settings page.
Lumaflip never needs **Full Disk Access**.

## What about privacy? Does Lumaflip connect to the internet?

- **No telemetry**, no analytics, no account, no cloud sync.
- The only network traffic is:
  - the **update check** (Sparkle, signed appcast on GitHub) — can be turned off;
  - **map tiles** from Apple Maps — only after you click *Show Map* in the info panel or open the
    folder map. The image coordinates are sent to Apple at that point. Consent lasts until the
    window is closed; it is never stored.
- Decoders for SVG, XCF, PSB, QOI and other less common formats run in `lumaflip-decoder`, a
  helper process with a **sandbox that denies network access and file writes**. If a malformed file
  crashes it, the helper is restarted and the app shows an error card.

## Does Lumaflip change my files?

Only when you ask it to: Move to Trash (with undo), rename, quick move/copy, *Save Rotation*
(lossless for JPEG), and ratings. Ratings go to an XMP sidecar file next to the image by default;
writing into the image (embedded XMP) or Finder tags can be enabled in *Settings → Rating*. Embedded
writes only change the metadata block, never re-encode pixels, and are done atomically. Lumaflip
never writes into the folder's `.DS_Store`.

## Can other apps read my ratings?

Yes. Ratings, picks and rejects use the standard `xmp:Rating` (reject = `-1`), so Lightroom,
Capture One, Bridge, darktable and digiKam can read the sidecars. For a RAW+JPEG pair,
`IMG_1.xmp` belongs to the RAW file (Lightroom convention) and the JPEG uses `IMG_1.JPG.xmp`.
Finder tags (e.g. "★★★", "Pick", "Reject" with colours) are searchable in Finder.

## Does it run on Intel Macs, Windows or Linux?

Releases are for **Apple Silicon** Macs with **macOS 15 Sequoia** or later. Windows and Linux builds
are planned for milestone M4 — the core is already platform-neutral and is built and tested on
Linux in CI.
