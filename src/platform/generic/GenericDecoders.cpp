// SPDX-License-Identifier: GPL-3.0-or-later
// registerPlatformDecoders() for the generic platform (Platform.h): no native
// backend. The portable decoders (Qt image plugins, SVG) are registered by the
// core itself (registerPortableDecoders).
#include "core/Decoder.h"
#include "core/Platform.h"

namespace lf {

void registerPlatformDecoders(DecoderRegistry& registry) {
    Q_UNUSED(registry); // no native backend: the portable decoders declare their suffixes
}

std::unique_ptr<Decoder> createPrimaryDecoder() {
    return nullptr; // the portable decoders need the application object (Qt plugins)
}

} // namespace lf
