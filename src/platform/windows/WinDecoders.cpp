// SPDX-License-Identifier: GPL-3.0-or-later
// registerPlatformDecoders() for Windows (Platform.h): the WIC backend. The portable decoders
// (Qt image plugins, SVG, the own "Rest" formats) are registered by the core itself
// (registerPortableDecoders) and run in the decoder helper process.
#include "core/Decoder.h"
#include "core/Platform.h"
#include "platform/windows/WicDecoder.h"

namespace lf {

void registerPlatformDecoders(DecoderRegistry& registry) {
    registry.add(std::make_shared<WicDecoder>());
    registry.addSupportedExtensions(WicDecoder::supportedExtensions());
}

std::unique_ptr<Decoder> createPrimaryDecoder() {
    return std::make_unique<WicDecoder>(); // needs no QGuiApplication (P-01 prestart)
}

} // namespace lf
