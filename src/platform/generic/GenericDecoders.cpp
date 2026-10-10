// SPDX-License-Identifier: GPL-3.0-or-later
// registerPlatformDecoders() for the generic platform (Platform.h): no native
// backend. The portable decoders (Qt image plugins, SVG) are registered by the
// core itself (registerPortableDecoders).
#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/Platform.h"

namespace lf {

void registerPlatformDecoders(DecoderRegistry& registry) {
#ifdef Q_OS_DARWIN
    // LF_FORCE_GENERIC_PLATFORM on macOS: DecoderRegistry only adds the portable
    // default suffixes on non-Darwin systems, so add them here to match Linux.
    registry.addSupportedExtensions(defaultImageSuffixes());
#else
    Q_UNUSED(registry);
#endif
}

} // namespace lf
