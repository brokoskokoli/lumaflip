// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder backends built on third-party libraries for platforms without ImageIO (SPEC 8.1,
// 8.3; Linux from M4). Each one is optional at build time (CMake LF_ENABLE_HEIF/JXL/RAW/EXR:
// AUTO = on when the library is found, only with the generic platform layer); the sources
// live in src/codecs/:
//   - libheif  (LibHeifDecoder.cpp): HEIF/HEIC and AVIF. HEVC needs libheif's libde265
//     plugin (HEVC patent risk outside macOS, SPEC 11): the backend only uses the decoder
//     plugins that the system's libheif has; without one HEIC files get the "unsupported"
//     error card while AVIF (dav1d/aom) keeps working.
//   - libjxl   (LibJxlDecoder.cpp): JPEG XL (first frame of animations).
//   - LibRaw   (LibRawDecoder.cpp): camera RAW, embedded preview for the fast stages.
//   - OpenEXR  (OpenExrDecoder.cpp): EXR (float, extended range like PFM).
// Like the other library decoders they run in the decoder helper process (SPEC 8.5): the
// app registers them through isolatedDecoder() (PortableDecoders.cpp), the helper serves
// them by name (src/helper/main.cpp). All backends are stateless and thread-safe.

#include "Decoder.h"

#include <QList>
#include <QStringList>

#include <memory>

namespace lf {

// The backends of this build (empty without any library).
QList<std::shared_ptr<Decoder>> libraryDecoders();
// File suffixes the backends add to folder scans (lower case).
QStringList libraryDecoderSuffixes();
// Names of the libraries built in ("libheif 1.17.6", "libjxl 0.7.0", …) for diagnostics.
QStringList libraryDecoderVersions();

// Loads what the libraries load lazily (libheif's decoder plugins). The decoder helper calls
// it before its sandbox closes in.
void prepareLibraryDecoders();

// Individual factories (nullptr when the library is not built in); tests use them.
std::shared_ptr<Decoder> createLibHeifDecoder();
std::shared_ptr<Decoder> createLibJxlDecoder();
std::shared_ptr<Decoder> createLibRawDecoder();
std::shared_ptr<Decoder> createOpenExrDecoder();

} // namespace lf
