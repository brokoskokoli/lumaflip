// SPDX-License-Identifier: GPL-3.0-or-later
// F-GRID-01 / P-04: the first 20 filmstrip thumbnails of a folder of 24 MP JPEGs appear
// quickly with an empty disk cache (embedded previews, low-priority worker threads).
// Uses copies of tests/corpus/generated/big_24mp.jpg.

#include "Bench.h"
#include "core/Decoder.h"
#include "core/ThumbnailCache.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

namespace {

constexpr int kFiles = 24;
constexpr int kVisible = 20;
constexpr int kThumbPx = 192; // filmstrip size on a Retina display

// Runs one cold pass: new disk folder, 20 requests, until all are ready. -1 on timeout.
double coldPass(const QStringList& files, const QString& cacheDir) {
    lf::ThumbnailCacheOptions o;
    o.directory = cacheDir;
    o.threads = std::clamp(QThread::idealThreadCount() / 4, 1, 2); // as ThumbnailCache::instance()
    lf::ThumbnailCache cache(lf::DecoderRegistry::instance(), o);
    int client = 0;
    int ready = 0;
    QObject::connect(&cache, &lf::ThumbnailCache::thumbnailReady, [&](const QString&, int) { ++ready; });
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < kVisible; ++i)
        cache.request(files[i], kThumbPx, &client);
    QDeadlineTimer deadline(20000);
    while (ready < kVisible) {
        if (deadline.hasExpired())
            return -1;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::usleep(200);
    }
    return double(t.nsecsElapsed()) / 1e6;
}

} // namespace

LF_BENCH(P_04_FilmstripFirstThumbs, 300.0) {
    const QString src = QStringLiteral(LF_CORPUS_DIR "/generated/big_24mp.jpg");
    if (!QFileInfo::exists(src))
        return -1;
    QTemporaryDir dir;
    if (!dir.isValid())
        return -1;
    QStringList files;
    for (int i = 0; i < kFiles; ++i) {
        const QString p = dir.filePath(QStringLiteral("IMG_%1.jpg").arg(i, 4, 10, QLatin1Char('0')));
        if (!QFile::copy(src, p))
            return -1;
        files << p;
    }
    (void)lf::DecoderRegistry::instance(); // backends registered outside the measurement
    // Median of three cold runs (each with an empty disk cache).
    double runs[3];
    for (double& r : runs) {
        QTemporaryDir cacheDir;
        r = coldPass(files, cacheDir.path());
        if (r < 0)
            return 1e9;
    }
    std::sort(std::begin(runs), std::end(runs));
    return runs[1];
}
