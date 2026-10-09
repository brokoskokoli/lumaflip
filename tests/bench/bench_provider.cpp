// SPDX-License-Identifier: GPL-3.0-or-later
// P-03: switching to a preloaded neighbour - the provider's part is a cache lookup.

#include "Bench.h"
#include "core/ImageProvider.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <vector>

namespace {

class BenchDecoder : public lf::Decoder {
public:
    QString name() const override { return "bench"; }
    int priority() const override { return 100; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<lf::ImageInfo> info(const QString&, QString*) override { return lf::ImageInfo{}; }
    std::optional<lf::DecodedImage> decode(const QString&, const lf::DecodeRequest& req, QString*) override {
        lf::DecodedImage d;
        d.fullSize = QSize(6000, 4000);
        const QSize s = req.maxPixelSize > 0
                            ? d.fullSize.scaled(req.maxPixelSize, req.maxPixelSize, Qt::KeepAspectRatio)
                            : d.fullSize;
        d.image = QImage(s, QImage::Format_RGB32);
        d.image.fill(Qt::gray);
        return d;
    }
    lf::Metadata metadata(const QString&) override { return {}; }
};

} // namespace

LF_BENCH(P_03_CacheHit, 1.0) {
    lf::DecoderRegistry registry;
    registry.add(std::make_shared<BenchDecoder>());
    lf::ImageProvider provider(registry);
    int client = 0;
    const QSize target(2560, 1600);
    QStringList wanted;
    for (int i = 0; i < 5; ++i)
        wanted << QStringLiteral("/bench/%1.jpg").arg(i);
    provider.setWanted(&client, wanted, target, true);

    // Wait until the whole prefetch window is decoded.
    QDeadlineTimer deadline(10000);
    auto allCached = [&] {
        for (const QString& p : wanted)
            if (!provider.request(p, lf::Quality::Screen, target, &client))
                return false;
        return true;
    };
    provider.setWanted(&client, wanted, target, true);
    while (!allCached()) {
        if (deadline.hasExpired())
            return -1;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }

    // Flip through the cached window; report the 95th percentile of request().
    std::vector<double> times;
    for (int round = 0; round < 200; ++round) {
        const QString& p = wanted[round % wanted.size()];
        QElapsedTimer t;
        t.start();
        auto img = provider.request(p, lf::Quality::Screen, target, &client);
        const double ms = double(t.nsecsElapsed()) / 1e6;
        if (!img)
            return 1e9;
        times.push_back(ms);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() * 95 / 100];
}
