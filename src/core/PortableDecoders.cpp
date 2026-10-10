// SPDX-License-Identifier: GPL-3.0-or-later
// registerPortableDecoders() (declared in QtImageDecoder.h). Separate translation unit so
// that using the decoders alone does not pull in the DecoderRegistry implementation.
#include "QtImageDecoder.h"
#include "SimpleDecoders.h"

namespace lf {

void registerPortableDecoders(DecoderRegistry& registry) {
    registry.add(std::make_shared<SvgDecoder>());
    registry.add(std::make_shared<QtImageDecoder>());
    registerSimpleDecoders(registry); // M3 "Rest" formats: QOI, PCX, PNM/PFM, SGI, ORA, KRA
    registry.addSupportedExtensions({QStringLiteral("svg"), QStringLiteral("svgz")});
    registry.addSupportedExtensions(QtImageDecoder::supportedExtensions());
}

} // namespace lf
