// SPDX-License-Identifier: GPL-3.0-or-later
// M3 benchmarks: the HDR decode request must not slow down SDR files (P-01/P-06 decode with
// the provider's HDR request, F-COL-03) and the decoder helper process (SPEC 8.5) must add
// little to a decode (warm helper, shared memory transfer). No windows.

#include "Bench.h"

#if defined(__APPLE__)
#include "core/DecoderHelper.h"
#include "core/ImageProvider.h"
#include "core/QtImageDecoder.h"
#include "platform/mac/ImageIODecoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <vector>

namespace {

QString corpus(const char* name) {
    return QStringLiteral(LF_CORPUS_DIR "/generated/") + QString::fromLatin1(name);
}

template <typename Fn> double medianMs(int runs, Fn&& fn) {
    std::vector<double> times;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer t;
        t.start();
        if (!fn())
            return 1e9; // failure = benchmark failure
        times.push_back(double(t.nsecsElapsed()) / 1e6);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

// P-01 / P-06: the Screen stage of a 24 MP JPEG with the request the provider sends on an
// EDR Mac (hdr = true: one extra gain-map probe, ~0.3 ms) - same limit as P_01_DecodeScreen24MP.
LF_BENCH(P_01_DecodeScreen24MPWithHdrRequest, 250.0) {
    if (!QFileInfo::exists(corpus("big_24mp.jpg")))
        return -1.0;
    lf::ImageIODecoder dec;
    const lf::DecodeRequest req = lf::ImageProvider::requestFor(lf::Quality::Screen, 2560, true);
    return medianMs(5, [&] {
        QString err;
        auto img = dec.decode(corpus("big_24mp.jpg"), req, &err);
        return img && img->image.format() == QImage::Format_RGBA8888_Premultiplied;
    });
}

// P-04: a library-decoded file (SVG at screen size) through the warm decoder helper:
// request, decode, shared memory, mapping. The first-use process start (~50 ms) is excluded.
LF_BENCH(P_04_DecoderHelperSvgScreen, 100.0) {
    const QString helper =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../src/lumaflip-decoder"));
    QString exe = qEnvironmentVariable("LF_DECODER_HELPER");
    if (exe.isEmpty())
        exe = helper;
    if (!QFileInfo(exe).isExecutable() || !QFileInfo::exists(corpus("vector.svg")))
        return -1.0;
    lf::DecoderHelperPool::Options o;
    o.executable = exe;
    o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    auto pool = std::make_shared<lf::DecoderHelperPool>(o);
    lf::RemoteDecoder remote(std::make_shared<lf::SvgDecoder>(), pool);
    lf::DecodeRequest req;
    req.maxPixelSize = 2560;
    QString err;
    if (!remote.decode(corpus("vector.svg"), req, &err)) // starts the helper
        return 1e9;
    return medianMs(7, [&] {
        auto img = remote.decode(corpus("vector.svg"), req, &err);
        return img && !img->image.isNull();
    });
}

#endif
