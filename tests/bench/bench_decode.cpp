// SPDX-License-Identifier: GPL-3.0-or-later
// Decode benchmarks (SPEC 7): P-01 screen-sized decode of a 24 MP JPEG and P-04 first
// preview of an image that is not prefetched. Uses tests/corpus/generated/big_24mp.jpg
// (tests/corpus/make_corpus.sh); skipped if the corpus is missing.

#include "Bench.h"

#if defined(__APPLE__)
#include "platform/mac/ImageIODecoder.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <vector>

namespace {

QString bigJpeg() {
    return QStringLiteral(LF_CORPUS_DIR "/generated/big_24mp.jpg");
}

// Median of several runs (the first run also warms the file cache like a real prefetch would not,
// so the median is a fair "typical" value).
double medianDecodeMs(const lf::DecodeRequest& req, int runs = 5) {
    if (!QFileInfo::exists(bigJpeg()))
        return -1.0;
    lf::ImageIODecoder dec;
    std::vector<double> times;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer t;
        t.start();
        QString err;
        auto img = dec.decode(bigJpeg(), req, &err);
        const double ms = double(t.nsecsElapsed()) / 1e6;
        if (!img)
            return 1e9; // decode failure = benchmark failure
        times.push_back(ms);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

// P-01 (decode part): 24 MP JPEG decoded to a 2560 px screen size.
LF_BENCH(P_01_DecodeScreen24MP, 250.0) {
    lf::DecodeRequest req;
    req.maxPixelSize = 2560;
    return medianDecodeMs(req);
}

// P-04: first display of a non-prefetched image via the embedded preview / thumbnail.
LF_BENCH(P_04_FirstPreview, 100.0) {
    lf::DecodeRequest req;
    req.maxPixelSize = 512;
    req.preferEmbeddedPreview = true;
    return medianDecodeMs(req);
}

#endif // __APPLE__
