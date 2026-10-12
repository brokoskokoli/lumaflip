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
- **winget** *(pending)*: once the package is accepted into `microsoft/winget-pkgs`,
  `winget install Brokoskokoli.Lumaflip` installs the same installer per user
  (`--scope machine` for all users). Manifests and submission: [RELEASING.md](RELEASING.md#winget).
- **Signed releases:** the installer, `Lumaflip.exe`, `lumaflip-decoder.exe` and
  `WinSparkle.dll` of a release are Authenticode-signed with Azure Artifact Signing and
  timestamped (see [Signing](#signing)). **CI artifacts are unsigned:** Windows SmartScreen shows
  "Windows protected your PC" on the first start: *More info → Run anyway*. An unsigned ZIP may
  also carry the "downloaded from the Internet" mark: *Properties → Unblock* before unpacking
  avoids the prompt for every file.
- **Updates:** installed copies update themselves (see [Automatic updates](#automatic-updates)); the
  installer of a newer version also upgrades an existing installation in place.

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

CI: job **Windows app** in `.github/workflows/checks.yml` (windows-2025; the build steps are the
composite action `.github/actions/windows-build`, shared with the release job): build, unit tests,
integration tests (real windows, QRhi on D3D11/WARP), deployment, a smoke test of the
deployed folder, ZIP and installer (`tools/windows_package.py`), then
`tools/windows_installer_check.py` installs the installer silently, updates it the way WinSparkle
does (appcast arguments, Lumaflip running, relaunch) and uninstalls it; ZIP and installer as
artifacts (`lumaflip-windows-x64`, unsigned). Supply chain: the Qt archives must match the
SHA-256 committed in `tools/fetch_qt_windows.py` (`--print-hashes` prints the entries for a new
version), WinSparkle the one in `tools/fetch_winsparkle.py`, vcpkg is pinned to a commit, Inno
Setup to a version.

## Platform layer

| Area | Windows implementation |
|---|---|
| Name order (F-SORT-01) | `CompareStringEx` with `SORT_DIGITSASNUMBERS` (Explorer's logical order), sort keys for large folders, code point order for ties |
| Folder order (SPEC 6.1) | Explorer keeps per-folder sort settings only in undocumented ShellBags: **name order**; a `.DS_Store` from a Mac still applies |
| File attributes | creation date, Explorer's type name (`SHGetFileInfo`) |
| Folder watching (F-OPEN-07/08) | `ReadDirectoryChangesW`, recursive for folder trees, overflow → rescan |
| Recycle Bin (F-FILE-01) | `IFileOperation` with `FOFX_RECYCLEONDELETE`; undo moves the `$R` item back and removes its `$I` record. A file that cannot go to the Recycle Bin (too large, a drive without one) is never deleted silently: Windows asks first (`FOF_WANTNUKEWARNING`), and if the user agrees Lumaflip reports "… wurde endgültig gelöscht" and offers no undo |
| Rename/replace | `MoveFileExW` without replace (atomic, never overwrites); `ReplaceFileW` keeps creation date, attributes, ACLs and streams; `FlushFileBuffers` |
| Reveal (F-FILE-05) | `SHOpenFolderAndSelectItems` |
| Open with | `SHAssocEnumHandlers` (recommended programs, default first), invoked like Explorer; programs chosen by path (*Other …*: any `.exe`) via `CreateProcess`; batch files, `cmd.exe`, PowerShell and `wsl.exe` are refused there (they would read the file name as a command) |
| Default app (F-MAC-04) | Windows does not allow programs to set themselves as default: Lumaflip opens *Settings → Default apps* at its own page |
| Custom actions (F-FILE-10) | program + arguments, the files appended as arguments quoted for `CommandLineToArgvW` (`core/CommandLine.h`), `CreateProcess`, never `cmd.exe`. Refused with an explanation, because they would read file names as commands: batch files (also spelled `x.bat.` or `x.cmd ` - Windows ignores trailing dots and spaces - or found through `PATH`/`PATHEXT`), `cmd.exe`, `conhost.exe`, `wsl.exe`/`bash.exe`, and PowerShell (`powershell`, `pwsh`) unless it runs a script: `powershell -File Skript.ps1` (`-Command`, `-EncodedCommand` or a bare argument are refused). Shortcuts actions are macOS only |
| Wallpaper (F-FILE-09) | `IDesktopWallpaper` per monitor |
| Recent items (F-OPEN-11) | the app's jump list (`SHAddToRecentDocs`) |
| Tags (F-INFO-06) | Windows has no Finder tags: the section is hidden; ratings use XMP sidecars / embedded XMP |
| Live Text, Quick Look, Services, Share sheet, maps | not available on Windows: hidden or replaced by a note |
| Menus | every window has its own menu bar with the shared menus (no global menu bar); in the borderless window it is collapsed into a menu button of the title strip (Alt and the shortcuts keep working) |
| Borderless window (F-VIEW-15) | the caption is removed from the frame with `WM_NCCALCSIZE` (styles unchanged: Aero Snap, Win+arrows, animations, resize borders stay); `WM_NCHITTEST` makes the strip a caption (drag, double click, system menu) and its maximize button `HTMAXBUTTON` (Snap Layouts on Windows 11) |

## Decoders

1. **WIC** (`WicDecoder`, priority 100, in the app process like ImageIO on macOS): JPEG, PNG,
   TIFF, BMP, ICO, JPEG XR, DDS built in; HEIF/HEIC, AVIF, WebP, JPEG XL and camera RAW when
   the codec extensions are installed. Orientation (EXIF/XMP), embedded ICC profiles and the
   EXIF color space, CMYK via WIC color management, metadata via
   `IWICMetadataQueryReader` (camera, exposure, date, GPS, rating), decode-time downscaling.
   GIF and animated WebP are left to Qt (animation; WIC returns the frames of an animated
   WebP as separate images without delays).
2. **Qt image plugins**, **Qt SVG** and Lumaflip's own decoders (QOI, PCX, PNM/PFM, SGI, ORA,
   KRA, XCF, PSB, PDF pages via the archive code) in the sandboxed helper process.

The list of image suffixes (folder scans, file dialogs) comes from these backends: the
installed WIC codecs (e.g. `jxr`/`wdp`/`hdp` for JPEG XR, the Store codecs' types when they are
installed) plus the portable decoders. Formats without a Windows backend (`exr`, `hdr`, `pic`,
`jp2`, `ktx`, `dcm`, `psd`) are not listed.
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
- a **memory limit** in the job (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`: half the physical memory,
  between 2 and 8 GiB; `LF_DECODER_HELPER_MEMORY_MB` overrides it) - a decode beyond it fails
  instead of exhausting the system; the decoders also check the size a file announces before
  they allocate;
- a **desktop of its own** (`lumaflip-decoder-<app pid>`, labelled low integrity): the screen it
  could capture with `GetDC`/`BitBlt` is that empty desktop, and windows it might create are
  never shown;
- only its stdin/stdout pipes and stderr inherited.

The helper reports `sandboxed` only if it runs at low integrity inside the job; the app
refuses it otherwise (as on macOS). **Not covered:**

- network access (blocking it needs an AppContainer, which cannot read arbitrary user files by
  path);
- reading files (by design);
- the user's screen, in theory: the helper still runs as the user in the user's window
  station, so a compromised helper could try to open the user's input desktop for reading
  (`OpenInputDesktop`; low integrity is not reliably refused that) and capture it there. Closing
  this would need a restricted token with the user's SID as deny-only (as browsers use for
  renderers), which in turn denies the user's own files - the helper must read them by path.
  The unit test `WindowsHelperRunsOnItsOwnDesktop` logs what the helper can reach on the CI
  runner.

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

## Automatic updates

Same behaviour as Sparkle on macOS (F-MAC-08, SPEC 10.3), with
[WinSparkle](https://winsparkle.org) (MIT, `tools/fetch_winsparkle.py`, pinned by SHA-256):

- *Ablage → Nach Updates suchen …* (File menu) checks at once; *Einstellungen → Allgemein → "Automatisch nach Updates
  suchen"* switches the daily check on or off. Nothing happens on the first start; on the second
  start WinSparkle asks whether to check automatically. The check is the app's only network traffic.
- *"Updates automatisch installieren"* is shown but disabled on Windows: WinSparkle can only
  install by quitting the app right away, so it always asks first ("Install update").
- Feed: `https://brokoskokoli.github.io/lumaflip/appcast-windows.xml`, separate from the macOS
  `appcast.xml` (Sparkle never sees Windows items). Every installer is EdDSA-signed with the same key
  as the macOS updates (public key compiled into `Lumaflip.exe` from `LF_SPARKLE_PUBLIC_KEY`);
  WinSparkle refuses an installer whose signature does not match.
- Installing: WinSparkle downloads the installer, starts it with
  `/SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1` and asks Lumaflip to
  quit (like *Quit*: the session is saved, pending file operations are finished). The installer
  upgrades in place (same `AppId`, the previous folder and "only me"/"all users" mode; an all-users
  installation asks for administrator rights), closes copies still running and starts the new
  version (`/RELAUNCH=1`).
- WinSparkle's settings (automatic check, last check, skipped version) are kept in
  `HKEY_CURRENT_USER\Software\Lumaflip\WinSparkle`; its windows follow Lumaflip's language.
- Builds without `WinSparkle.dll` (`-DLF_ENABLE_WINSPARKLE=OFF`, or the DLL missing) or without the
  public key (`-DLF_SPARKLE_PUBLIC_KEY=…`, CI builds) have no updater: the menu item and the settings
  are hidden, the reason is logged (`lumaflip.updater`). The DLL is loaded at run time only from the
  folder of `Lumaflip.exe` (`LoadLibraryEx`, no `PATH` search).

## Signing

Releases (`.github/workflows/release.yml`, job `build-windows`, tags of the main repository only)
are signed with **Azure Artifact Signing** (formerly Trusted Signing), certificate profile type
*Public Trust*:

1. `Lumaflip.exe`, `lumaflip-decoder.exe` and every other `.exe`/`.dll` of the deployed folder
   without a valid signature (`WinSparkle.dll`) are signed (SHA-256, RFC 3161 timestamp), then ZIP
   and installer are built from the signed folder and the installer is signed.
2. GitHub logs in to Azure with OpenID Connect (federated credential for the environment
   `release`, no client secret); the app registration may only sign with this one certificate
   profile. Set up with `tools/setup_windows_signing.sh` (docs/RELEASING.md, "Windows: Signatur und
   Updates").
3. The job verifies every signature with `signtool verify /pa` and fails a tag build if signing is
   not configured; a dry run without it builds unsigned with a warning.

Not signed: the uninstaller inside the installer (`unins000.exe`, Inno Setup would have to sign it
while compiling); it is never downloaded, so SmartScreen does not check it.

**SmartScreen:** reputation builds up per certificate (and per file); an Artifact Signing
certificate is trusted by Windows from the start, but a new publisher may still see the
"Windows protected your PC" prompt for the first downloads until SmartScreen has seen enough of
them. The publisher name shown is the validated identity of the certificate profile.

## Known gaps

- Updates always ask before they install (WinSparkle); "Updates automatisch installieren" is
  disabled on Windows.
- No system share sheet yet (Windows `DataTransferManager`), no Live Text, no maps.
- The Explorer sort order of a folder cannot be read; Lumaflip uses name order.
- Network access of the decoder helper is not blocked (see above).
- PDF pages (F-OPEN-10) need QtPdf, which the Qt online repository does not ship for
  `msvc2022_64` without Qt WebEngine; CBZ/CBR/7z archives work.
- The library decoders of the Linux build (libheif, libjxl, LibRaw, OpenEXR; `src/codecs`)
  are not built on Windows yet; WIC with the Store codec extensions covers these formats.
- Native keyboard event injection for the integration tests exists only on macOS; those
  tests skip on Windows (as on Linux).
