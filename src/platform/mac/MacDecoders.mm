// SPDX-License-Identifier: GPL-3.0-or-later
// registerPlatformDecoders() for macOS (Platform.h). Kept in its own translation unit
// so that using ImageIODecoder alone does not pull in the DecoderRegistry.
#include "core/Decoder.h"
#include "core/Platform.h"
#include "platform/mac/ImageIODecoder.h"

namespace lf {

void registerPlatformDecoders(DecoderRegistry& registry) {
    registry.add(std::make_shared<ImageIODecoder>());
    registry.addSupportedExtensions(ImageIODecoder::supportedExtensions());
}

std::unique_ptr<Decoder> createPrimaryDecoder() {
    return std::make_unique<ImageIODecoder>();
}

} // namespace lf
