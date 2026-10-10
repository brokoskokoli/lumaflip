// SPDX-License-Identifier: GPL-3.0-or-later
// registerPortableDecoders() (declared in QtImageDecoder.h). Separate translation unit so
// that using the decoders alone does not pull in the DecoderRegistry implementation.
#include "DecoderHelper.h"
#include "QtImageDecoder.h"

namespace lf {

void registerPortableDecoders(DecoderRegistry& registry) {
    // SPEC 8.5 (M3): library decoders run in the helper process when it is installed
    // (the app bundle); unbundled binaries (tests) decode in process.
    registry.add(isolatedDecoder(std::make_shared<SvgDecoder>()));
    registry.add(isolatedDecoder(std::make_shared<QtImageDecoder>()));
    registry.addSupportedExtensions({QStringLiteral("svg"), QStringLiteral("svgz")});
    registry.addSupportedExtensions(QtImageDecoder::supportedExtensions());
}

} // namespace lf
