// SPDX-License-Identifier: GPL-3.0-or-later
#include "StartupWarmup.h"

#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/Settings.h"
#include "core/StartupTrace.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QThreadPool>

namespace lf::startup {

namespace {
// Fallback: the default window size (Application::placeWindow) on a Retina display.
constexpr QSize kDefaultDecodeTarget(2200, 1520);
} // namespace

QString firstImageArgument(const QStringList& arguments) {
    static const QStringList suffixes = defaultImageSuffixes(); // static list, no backends needed
    for (const QString& a : arguments) {
        if (a.isEmpty() || a.startsWith(QLatin1Char('-')))
            continue;
        const QString suffix = QFileInfo(a).suffix().toLower();
        if (!suffixes.contains(suffix))
            return {}; // the app opens the first argument first; only an image is prestarted
        // Same normalization as Collection (absolute, clean, NFC).
        return QDir::cleanPath(QFileInfo(a).absoluteFilePath()).normalized(QString::NormalizationForm_C);
    }
    return {};
}

QSize expectedDecodeTarget() {
    const QSize s = Settings::instance().value(QString::fromLatin1(key::DecodeTargetHint)).toSize();
    return s.isValid() && !s.isEmpty() ? s : kDefaultDecodeTarget;
}

void warmUpAfterApplication() {
    QThreadPool* pool = QThreadPool::globalInstance();
    // The font database is populated on first use (~30 ms); QFontDatabase is thread-safe.
    pool->start([] {
        QFontDatabase::families();
        trace::mark("warm-up: font database ready");
    });
    // Loading the Qt image plugins of the decoder registry takes a while; the first image
    // does not need them (ImageProvider builds the registry lazily).
    pool->start([] {
        DecoderRegistry::instance();
        trace::mark("warm-up: decoder registry ready");
    });
}

} // namespace lf::startup
