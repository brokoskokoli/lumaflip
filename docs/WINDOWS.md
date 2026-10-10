<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Lumaflip on Windows

Status of the Windows 10/11 port (M4, SPEC 9.3 and 13), how to build it, and what differs
from macOS. The platform code lives in `src/platform/windows/` (core layer) and
`src/app/*_win.cpp` (UI counterparts of the `*_mac.mm` files); CMake parts in
`src/platform/windows/*.cmake` and `if(WIN32)` blocks.

## Install

- **Installer:** `Lumaflip-<version>-windows-x64-setup.exe` (Inno Setup). Installs for the
  current user without administrator rights (or for all users on request), adds a Start menu
  entry and registers Lumaflip for the image types in **Open with** and in
  **Settings → Apps → Default apps**. It never takes over a file type by itself.
- **Portable:** `Lumaflip-<version>-windows-x64.zip`, unpack anywhere and start
  `Lumaflip.exe` (no file type registration).
- **Unsigned builds:** the CI artifacts are not code-signed yet. Windows SmartScreen shows
  "Windows protected your PC" on the first start: *More info → Run anyway*. The ZIP may also
  carry the "downloaded from the Internet" mark: *Properties → Unblock* before unpacking
  avoids the prompt for every file.

Requirements: Windows 10 1809 or later, or Windows 11; x64. Optional codec extensions from the
Microsoft Store add formats to the system decoder: *HEIF Image Extensions* (+ *HEVC Video
Extensions*), *AV1 Video Extension* (AVIF), *WebP Image Extensions*, *JPEG XL Image
Extension*, *Raw Image Extension*.

## Build

Requirements: Visual Studio 2022 (Desktop C++ workload) or Build Tools, CMake ≥ 3.28, Ninja,
Qt 6.11 for `msvc2022_64` (with *Qt Image Formats* and *Qt Shader Tools*), Python 3,
[vcpkg](https://github.com/microsoft/vcpkg) (manifest mode, `vcpkg.json`: zlib,
libjpeg-turbo, libarchive, GoogleTest).

In an *x64 Native Tools Command Prompt for VS 2022*:

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
set CMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release -L unit
build\windows-release\src\Lumaflip.exe
python tools\windows_deploy.py build\windows-release dist\Lumaflip --vcpkg-installed build\windows-release\vcpkg_installed\x64-windows-static-md
```

Presets: `windows-release` (MSVC, RelWithDebInfo), `windows-debug`, `windows-clang-cl`. Own
sources compile with `/W4 /WX` (the warnings clang reports with `-Wall -Wextra`; conversion
and shadowing warnings are off on both compilers). The installer is built with
[Inno Setup 6](https://jrsoftware.org/isinfo.php): `iscc /DAppVersion=<v>
/DSourceDir=<deployed folder> /DOutputDir=dist packaging\windows\Lumaflip.iss`.

CI: job **Windows app** in `.github/workflows/checks.yml` (windows-2025): build, unit tests,
integration tests (real windows, QRhi on D3D11/WARP), deployment, a smoke test of the
deployed folder, ZIP and installer as artifacts (`lumaflip-windows-x64`).

## Platform layer

| Area | Windows implementation |
|---|---|
| Name order (F-SORT-01) | `CompareStringEx` with `SORT_DIGITSASNUMBERS` (Explorer's logical order), sort keys for large folders, code point order for ties |
| Folder order (SPEC 6.1) | Explorer keeps per-folder sort settings only in undocumented ShellBags: **name order**; a `.DS_Store` from a Mac still applies |
| File attributes | creation date, Explorer's type name (`SHGetFileInfo`) |
| Folder watching (F-OPEN-07/08) | `ReadDirectoryChangesW`, recursive for folder trees, overflow → rescan |
| Recycle Bin (F-FILE-01) | `IFileOperation` with `FOFX_RECYCLEONDELETE`; undo moves the `$R` item back and removes its `$I` record |
| Rename/replace | `MoveFileExW` without replace (atomic, never overwrites); `ReplaceFileW` keeps creation date, attributes, ACLs and streams; `FlushFileBuffers` |
| Reveal (F-FILE-05) | `SHOpenFolderAndSelectItems` |
| Open with | `SHAssocEnumHandlers` (recommended programs, default first), invoked like Explorer; programs chosen by path via `CreateProcess` |
| Default app (F-MAC-04) | Windows does not allow programs to set themselves as default: Lumaflip opens *Settings → Default apps* at its own page |
| Custom actions (F-FILE-10) | program + arguments, the files appended as arguments quoted for `CommandLineToArgvW` (`core/CommandLine.h`), `CreateProcess`, never `cmd.exe`; batch files are refused; Shortcuts actions are macOS only |
| Wallpaper (F-FILE-09) | `IDesktopWallpaper` per monitor |
| Recent items (F-OPEN-11) | the app's jump list (`SHAddToRecentDocs`) |
| Tags (F-INFO-06) | Windows has no Finder tags: the section is hidden; ratings use XMP sidecars / embedded XMP |
| Live Text, Quick Look, Services, Share sheet, maps | not available on Windows: hidden or replaced by a note |
| Menus | every window has its own menu bar with the shared menus (no global menu bar) |

## Decoders

1. **WIC** (`WicDecoder`, priority 100, in the app process like ImageIO on macOS): JPEG, PNG,
   TIFF, BMP, ICO, JPEG XR, DDS built in; HEIF/HEIC, AVIF, WebP, JPEG XL and camera RAW when
   the codec extensions are installed. Orientation (EXIF/XMP), embedded ICC profiles and the
   EXIF color space, CMYK via WIC color management, metadata via
   `IWICMetadataQueryReader` (camera, exposure, date, GPS, rating), decode-time downscaling.
   GIF is left to Qt (animation).
2. **Qt image plugins**, **Qt SVG** and Lumaflip's own decoders (QOI, PCX, PNM/PFM, SGI, ORA,
   KRA, XCF, PSB, PDF pages via the archive code) in the sandboxed helper process.
3. Lossless JPEG rotation with libjpeg-turbo 3 (vcpkg, static); RAR/CBR and 7z/CB7 via
   libarchive (vcpkg, static).

Optional libraries (libheif, libjxl, LibRaw, OpenEXR) are not built yet; the codec extensions
cover these formats through WIC.

## Decoder helper sandbox (SPEC 8.5)

`lumaflip-decoder.exe` is started by the app with:

- a **low integrity** token (restricted copy of the user's token without privileges): it cannot
  write to the user's files, folders or registry ("no write up"), cannot send window
  messages to other processes (UIPI), cannot open the app's process;
- a **job object**: dies with the app (`KILL_ON_JOB_CLOSE`), no child processes (active process
  limit 1 plus the child process creation policy), no clipboard, desktop switching, global
  atoms or system settings;
- **process mitigations**: no images from remote shares or with a low label, no extension point
  DLLs, strict handle checks, heap termination on corruption; after start-up the helper also
  forbids dynamic code;
- only its stdin/stdout pipes and stderr inherited.

The helper reports `sandboxed` only if it runs at low integrity inside the job; the app
refuses it otherwise (as on macOS). **Not covered:** network access (blocking it needs an
AppContainer, which cannot read arbitrary user files by path) and reading files (by design).
Pixels travel through a named file mapping labelled low integrity, created by the app.

## HDR and color

- SDR: the canvas encodes its pixels in the monitor's ICC profile (`GetICMProfileW`), since
  Qt's D3D swapchain is not color managed. With *advanced color* (HDR or Auto Color
  Management) Windows composites in scRGB and expects sRGB, so no profile is applied there.
- HDR (F-COL-03): on an HDR monitor (`IDXGIOutput6`: BT.2100 PQ) HDR images are shown through
  an extended range surface with an **scRGB** swapchain (`QRhiSwapChain::HDRExtendedSrgbLinear`,
  RGBA16F, 1.0 = SDR white). Headroom = peak luminance / SDR white level (Settings → Display →
  HDR → SDR content brightness). Changes of the display configuration are followed.
- Gain map HDR (ISO 21496-1) is decoded by ImageIO on macOS only; on Windows such files show
  their SDR base image.

## Signing (later)

The build is prepared for Authenticode signing but not signed yet:

1. Obtain a code signing certificate (OV/EV, or Azure Trusted Signing).
2. Sign `Lumaflip.exe` and `lumaflip-decoder.exe` before packaging
   (`signtool sign /fd SHA256 /tr <timestamp URL> /td SHA256 ...`), then the installer
   (Inno Setup: `SignTool=` in `[Setup]`).
3. Only in release builds from tags of the main repository, with the certificate as an
   encrypted secret (like the macOS Developer ID, docs/RELEASING.md).

SmartScreen reputation builds up per certificate; EV certificates start with reputation.

## Known gaps

- No automatic updates on Windows (Sparkle is macOS only); the update menu item is hidden.
- No system share sheet yet (Windows `DataTransferManager`), no Live Text, no maps.
- The Explorer sort order of a folder cannot be read; Lumaflip uses name order.
- Network access of the decoder helper is not blocked (see above).
