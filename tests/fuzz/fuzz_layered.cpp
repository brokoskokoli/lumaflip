// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the own XCF (GIMP) and PSB (Photoshop Large Document) decoders (SPEC 4.4,
// run in the decoder helper). The first byte chooses the format and the target edge.
// Invariants: a result is never larger than the canvas of the file and never empty, and
// each input finishes quickly (the working canvas is capped, xcf::kMaxCanvasPixels).

#include "FuzzTarget.h"
#include "LayeredTestWriters.h"
#include "core/PsbDecoder.h"
#include "core/XcfDecoder.h"

#include <cstdio>

using namespace lf;

QList<QByteArray> lf::fuzz::seeds() {
    QList<QByteArray> out;
    for (int compression : {0, 1, 2}) {
        test::XcfSpec s;
        s.width = 70;
        s.height = 40;
        s.compression = compression;
        test::XcfLayerSpec top;
        top.width = 30;
        top.height = 20;
        top.x = 5;
        top.y = -3;
        top.mode = 30;
        top.pixels = test::solidRgba8(30, 20, 200, 100, 50, 180);
        top.mask = QByteArray(30 * 20, char(128));
        test::XcfLayerSpec group;
        group.width = 70;
        group.height = 40;
        group.group = true;
        test::XcfLayerSpec child;
        child.width = 70;
        child.height = 40;
        child.path = {1, 0};
        child.pixels = test::quadRgba8(70, 40);
        s.layers << top << group << child;
        out << QByteArray(1, char(compression)) + test::writeXcf(s);
    }
    for (int compression : {0, 1, 2, 3}) {
        test::PsbSpec p;
        p.width = 9;
        p.height = 7;
        p.depth = compression == 3 ? 16 : 8;
        p.compression = compression;
        const int bytes = p.depth / 8;
        for (int c = 0; c < 4; ++c)
            p.channels << QByteArray(9 * 7 * bytes, char(40 * c + 20));
        out << QByteArray(1, char(0x80 | compression)) + test::writePsb(p);
    }
    return out;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > (4 << 20))
        return 0;
    const QByteArray in = fuzz::bytes(data + 1, size - 1);
    const bool psb = data[0] & 0x80;
    const int maxEdge = (data[0] & 0x40) ? 0 : 64;
    QString error;
    QSize canvas;
    QImage image;
    if (psb) {
        if (const auto r = psb::decode(in, maxEdge, &error)) {
            canvas = r->fullSize;
            image = r->image;
        }
    } else if (const auto r = xcf::decode(in, maxEdge, &error)) {
        canvas = r->fullSize;
        image = r->image;
    }
    if (!canvas.isEmpty()) {
        if (image.isNull())
            fuzz::fail("decoded without an image");
        if (image.width() > canvas.width() || image.height() > canvas.height())
            fuzz::fail("result larger than the canvas");
    } else if (error.isEmpty()) {
        fuzz::fail("failed without an error message");
    }
    static const bool trace = qEnvironmentVariableIntValue("LF_FUZZ_TRACE") == 1;
    if (trace)
        std::fprintf(stderr, "%s, %zu bytes: %s\n", psb ? "PSB" : "XCF", size - 1,
                     canvas.isEmpty() ? qPrintable(error) : "ok");
    return 0;
}
