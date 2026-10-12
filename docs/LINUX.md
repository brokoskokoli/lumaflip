<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Lumaflip on Linux

Status: M4 port (SPEC 9.3, 13). The app is built and tested on Ubuntu 24.04 in CI (job
"Linux app" in `.github/workflows/checks.yml`) and packaged as an AppImage. This page lists what
works, how the Linux parts are built, and what is different from macOS.

## Install

- **AppImage** (x86_64): download `Lumaflip-<version>-x86_64.AppImage` from the CI artifacts or a
  release, `chmod +x` it and run it. Needs glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40
  and later). *File → Set as Default App…* writes a desktop entry for the AppImage to
  `~/.local/share/applications` and makes it the default viewer for the image types.
- **From source**: see below; `cmake --install` puts the app in the usual XDG layout.
- **Flatpak**: a manifest is in `packaging/linux/io.github.brokoskokoli.lumaflip.yml` (not
  published on Flathub).

## Build from source

Ubuntu 24.04 (other distributions: the same libraries under their names):

```sh
sudo apt install cmake ninja-build clang pkg-config libgtest-dev libarchive-dev \
  libheif-dev libheif-plugin-dav1d libjxl-dev libraw-dev libopenexr-dev librsvg2-bin \
  libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0
# Qt 6.11 (qtbase, qtsvg, qttools, qtimageformats, qtshadertools, optional qtpdf), e.g. with aqtinstall:
pipx run aqtinstall install-qt linux desktop 6.11.2 linux_gcc_64 -m qtimageformats qtshadertools qtpdf -O ~/Qt
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=~/Qt/6.11.2/gcc_64
cmake --build build/linux
build/linux/src/lumaflip [image]
ctest --test-dir build/linux -L unit                     # unit tests (no windows)
xvfb-run -a ctest --test-dir build/linux -L integration  # integration tests in a virtual X server
sudo cmake --install build/linux                         # /usr/local/bin/lumaflip …
tools/linux_appimage.sh build/linux dist                 # AppImage (downloads linuxdeploy)
```

Options (CMake cache, `AUTO` = on when found):

| Option | Default | Meaning |
|---|---|---|
| `LF_ENABLE_HEIF` | AUTO | HEIF/HEIC/AVIF via libheif (`pkg-config libheif`) |
| `LF_ENABLE_JXL` | AUTO | JPEG XL via libjxl |
| `LF_ENABLE_RAW` | AUTO | camera RAW via LibRaw (`libraw_r`, thread-safe build) |
| `LF_ENABLE_EXR` | AUTO | OpenEXR |
| `LF_TURBOJPEG_INCLUDE` / `LF_TURBOJPEG_LIB` | found | libjpeg-turbo **3.x** for lossless JPEG rotation/crop (F-EDIT-02/03). Ubuntu 24.04 has 2.1, which is ignored; CI builds 3.2 from source (static). Without it, JPEG rotation is saved via the EXIF orientation. |

The canvas (`QRhiWidget`) uses QRhi with **OpenGL** (Qt's default on Linux; works on every
driver incl. Mesa llvmpipe and under XWayland/Wayland). The shaders are compiled for GLSL, HLSL and
MSL by `qt_add_shaders`, so a Vulkan backend (SPIR-V is built as well) would only need the widget's
API switched; it is not enabled because OpenGL is the most compatible choice.

## Platform layer

| Piece | Linux implementation | Source |
|---|---|---|
| Name sorting (F-SORT-01) | digit runs by value + `QCollator` (ICU when Qt has it) | `platform/generic/GenericSorting.cpp` |
| Folder order (SPEC 6) | name order; `.DS_Store` files copied from a Mac are still read. File manager sort settings (Nautilus/Dolphin) are not read; the menu, settings and status bar say "Dateimanager" ("Name ↑ (Sortierung des Dateimanagers nicht bekannt)"), and the live query setting is not offered. | same |
| Dates (F-SORT-02) | birth time via `statx` (through `QFileInfo::birthTime`), where the file system records it | same |
| Folder watching (F-OPEN-07/08) | inotify, recursive for "folder and subfolders" (the tree is walked on a worker thread; folders deleted or moved out give their watches back), `*.xmp` reported as sidecar changes, queue overflow → rescan | `platform/linux/LinuxFolderWatcher.cpp` |
| Trash (F-FILE-01) | freedesktop trash via `QFile::moveToTrash`; undo moves the file back (also across file systems) and removes the `.trashinfo` | `platform/generic/GenericTrashService.cpp` |
| Reveal in file manager | D-Bus `org.freedesktop.FileManager1.ShowItems` (Nautilus, Dolphin, Nemo, Caja, Thunar …; asynchronous), on an error reply the folder is opened | `platform/generic/GenericShell.cpp` |
| Open with / editor (F-FILE-05) | installed `.desktop` files for the file's MIME type (and its parent types), `mimeapps.list` default first; started with the Exec field codes. Field codes inside a quoted Exec argument (not allowed by the spec, but common in `sh -c "… %f"` entries) are inserted shell-quoted, so a file name never becomes part of a shell command. *Other …* picks a `.desktop` file or any executable. | `platform/generic/XdgDesktop.cpp`, `app/generic/GenericOpenWith.cpp` |
| Default viewer (F-MAC-04) | writes `[Default Applications]` in `$XDG_CONFIG_HOME/mimeapps.list` (like `xdg-mime default`); the AppImage first installs its own desktop entry | `platform/generic/GenericShell.cpp` |
| Tags (F-INFO-06) | extended attribute `user.xdg.tags` (comma-separated; the format KDE Dolphin/Baloo uses). File systems without user xattrs report an error when saving. Ratings as Finder tags (F-CULL-02) stay macOS-only. | `platform/linux/LinuxTagService.cpp` |
| Display color space | sRGB: there is no portable API for the monitor profile (colord would need a configured profile). Wide-gamut images are converted to sRGB. | `app/generic/GenericWindowing.cpp` |
| Wallpaper (F-FILE-09) | GNOME/Unity/Budgie/Pantheon/Cinnamon/MATE via `gsettings`, KDE via `plasma-apply-wallpaperimage` (run on a worker thread, the result in the status line); others: message | `app/generic/GenericWallpaper.cpp` |

## Decoders

macOS decodes nearly everything with ImageIO. On Linux the formats come from:

| Formats | Backend | Notes |
|---|---|---|
| JPEG, PNG, GIF, BMP, WebP, TIFF, TGA, ICO, ICNS, WBMP … | Qt image plugins (`qtimageformats`) | |
| SVG | resvg if installed, else Qt SVG | |
| QOI, PCX, PNM/PFM, SGI, ORA, KRA, XCF, PSD/PSB | own decoders (portable) | |
| HEIF/HEIC, AVIF | libheif | AV1 through libheif's dav1d/aom plugin. **HEVC (HEIC) needs the libde265 plugin** (`libheif-plugin-libde265`), which is optional because of HEVC patents (SPEC 11). The AppImage ships only the AV1 plugin: HEIC files show "format not supported" there. Distribution packages decide themselves. |
| JPEG XL | libjxl | first frame of animations |
| Camera RAW | LibRaw | embedded preview for the fast stages, developed image (camera white balance, sRGB) for zooming; LibRaw is used under its CDDL-1.0 option |
| OpenEXR | OpenEXR | half/float, extended range (HDR images are tone-mapped: no EDR on Linux) |
| PDF pages (F-OPEN-10) | QtPdf (PDFium) | when Qt has the `qtpdf` module (CI and the AppImage do) |

All library decoders run in the decoder helper process (`lumaflip-decoder`, SPEC 8.5). Each
checks the size its file announces before it allocates pixels (at most 268 megapixels and
2 GiB per full-size buffer; float images are downsampled to the requested size before the
conversions), and the helper process itself is limited to half the physical memory, between
2 and 8 GiB (`RLIMIT_DATA`; `LF_DECODER_HELPER_MEMORY_MB` overrides it, `0` = no limit).

The list of image suffixes (folder scans, file dialogs) is derived from the backends that are
really built in: without libheif no `heic`, without LibRaw no RAW suffixes; formats with no
Linux backend at all (`hdr`, `pic`, `jp2`, `ktx`, `dcm`) are not listed.

## Decoder helper sandbox

The app starts the helper with nothing but its protocol pipes (stdin/stdout, created with
`O_CLOEXEC`) and `/dev/null` as stderr (`LF_DECODER_HELPER_STDERR=1` keeps the app's stderr for
diagnostics); the helper closes every other descriptor itself before anything else and sets
its memory limit (`RLIMIT_DATA`). Then it restricts itself before it reads any file
(`platform/linux/LinuxSandbox.h`):

1. `PR_SET_NO_NEW_PRIVS`;
2. **seccomp-bpf** (required):
   - no network or IPC endpoints: `socket()` fails (no D-Bus, no display connection);
   - no new processes (`execve`, `fork`, `clone` without `CLONE_THREAD`) and no access to other
     processes: no `ptrace`, `process_vm_*`, `process_madvise`, `kcmp`, no pidfds
     (`pidfd_open`, `pidfd_getfd`, `pidfd_send_signal`); `kill`/`tgkill`/`rt_sigqueueinfo` only
     towards the helper itself (it cannot signal the app);
   - **no change to the file system, with or without Landlock**: `open`/`openat` with
     `O_WRONLY`, `O_RDWR`, `O_CREAT` or `O_TRUNC` fail, as do `creat`, `rename*`, `unlink*`,
     `mkdir*`, `mknod*`, `link*`, `symlink*`, the `chmod`/`chown`/`truncate`/`utime` families and
     the xattr setters (`openat2` reports `ENOSYS`, so the C library falls back to `openat`);
   - no input injection into a terminal (`ioctl` `TIOCSTI`/`TIOCLINUX`);
   - no mount/namespace/keyring/BPF/`perf_event_open`/`io_uring` calls;
3. **Landlock** (kernel 5.13+, best effort): the file system becomes read-only (no write rule at
   all - not even for `/dev/shm`); with Landlock ABI 4 TCP bind/connect are denied as well.
   Landlock also stops ptrace-style access to processes outside the sandbox (for example
   reading `/proc/<app pid>/mem`).

The decoded pixels travel in a **sealed memfd** the helper creates (no named shared memory on
Linux): the app opens it through `/proc/<helper pid>/fd/<n>`, requires the seals
(`F_SEAL_WRITE`, `F_SEAL_SHRINK`, `F_SEAL_GROW`) and maps it privately, so the helper can
neither change nor shrink the pixels afterwards.

Without Landlock (kernels before 5.13) the seccomp rules still keep the file system read-only;
what remains is reading files (by design) and - only where the kernel's ptrace rules allow it
(Yama `ptrace_scope` 0) - reading other processes' memory through `/proc/<pid>/mem`. Ubuntu,
Debian and Fedora default to `ptrace_scope` 1, which refuses that for non-descendants.

Unprivileged user namespaces (`unshare(CLONE_NEWNET)`) are not used: Ubuntu 23.10 and later
restrict them through AppArmor. The helper logs which parts are active
(`lumaflip-decoder: sandbox: seccomp on, Landlock ABI 4`). If seccomp cannot be applied, the app
does not use the helper and shows "Der Decoder-Hilfsprozess läuft ohne Sandbox …" on the error
card; `LF_DECODER_ALLOW_UNSANDBOXED=1` accepts an unsandboxed helper (not recommended; Linux
only - on macOS and Windows the variable has no effect). Landlock
covers the thread that applies it and threads started afterwards (the helper starts its decoding
threads later).

## Not available on Linux

Hidden or shown as "not available": Live Text (VisionKit), Quick Look, the Services menu, the
share sheet, maps (folder map and GPS mini map: MapKit), native window tabs, EDR/HDR presentation
(HDR images are tone-mapped to SDR), the Dock menu, Sparkle updates, per-window screen profiles,
writing ratings into image files (sidecar XMP works), Finder-specific sort settings. The
borderless window with the fading title strip (F-VIEW-13/15) is a frameless window there: the
strip draws its own buttons (on the side of the GNOME/KDE setting), moves the window with
`startSystemMove()`, and the edges resize it with `startSystemResize()`.
Integration tests that depend on these are skipped on Linux.

## CI

Job "Linux app" (ubuntu-24.04, Qt 6.11.2 exactly via `jurplel/install-qt-action`):

- builds with libheif/libjxl/LibRaw/OpenEXR and a static libjpeg-turbo 3 (built once, cached);
- unit tests (`QT_QPA_PLATFORM=offscreen`), with `LF_REQUIRE_LIBRARY_DECODERS=1` and the RAW
  samples of `tests/corpus/download_raw.sh`;
- integration tests under `xvfb-run` (xcb, Mesa llvmpipe OpenGL for the QRhi canvas);
- validates the desktop entry and the AppStream metadata;
- builds the AppImage (`tools/linux_appimage.sh`: linuxdeploy, its Qt plugin and the AppImage
  runtime are pinned by SHA-256, the runtime through `LDAI_RUNTIME_FILE`), starts it under Xvfb until the first frame
  of an image is shown, and uploads it as the artifact `Lumaflip-linux-x86_64-AppImage`.

The generated format corpus (`tests/corpus/make_corpus.sh`) needs macOS (ImageIO); on Linux the
library decoder tests encode their own files with the same libraries.
