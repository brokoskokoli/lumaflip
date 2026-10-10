// SPDX-License-Identifier: GPL-3.0-or-later
// registerPortableDecoders() (declared in QtImageDecoder.h). Separate translation unit so
// that using the decoders alone does not pull in the DecoderRegistry implementation.
#include "DecoderHelper.h"
#include "PsbDecoder.h"
#include "QtImageDecoder.h"
#include "ResvgDecoder.h"
#include "SimpleDecoders.h"
#include "XcfDecoder.h"

namespace lf {

void registerPortableDecoders(DecoderRegistry& registry) {
    // SPEC 8.5 (M3): library decoders run in the helper process when it is installed
    // (the app bundle); unbundled binaries (tests) decode in process.
    if (auto resvg = createResvgDecoder()) // SVG 1.1 (M3); QtSvg stays the fallback
        registry.add(isolatedDecoder(resvg));
    registry.add(isolatedDecoder(std::make_shared<SvgDecoder>()));
    registry.add(isolatedDecoder(std::make_shared<QtImageDecoder>()));
    // M3 "Rest" formats (QOI, PCX, PNM/PFM, SGI, ORA, KRA): own parsers, also isolated.
    for (const std::shared_ptr<Decoder>& d : simpleDecoders())
        registry.add(isolatedDecoder(d));
    registry.addSupportedExtensions(simpleDecoderSuffixes());
    // XCF (GIMP) and PSB (Photoshop Large Document): own decoders, isolated (M3).
    registry.add(isolatedDecoder(std::make_shared<XcfDecoder>()));
    registry.add(isolatedDecoder(std::make_shared<PsbDecoder>()));
    registry.addSupportedExtensions({QStringLiteral("xcf"), QStringLiteral("psb")});
    registry.addSupportedExtensions({QStringLiteral("svg"), QStringLiteral("svgz")});
    registry.addSupportedExtensions(QtImageDecoder::supportedExtensions());
}

} // namespace lf
