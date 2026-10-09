// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Asynchronous, cached, prioritized image loading shared by all windows
// (SPEC 7.1, F-WIN-09). GUI-thread object; decoding happens on a thread pool.

#include "Decoder.h"

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <memory>

namespace lf {

class DecoderRegistry;

enum class Quality {
    Preview,  // embedded preview / small thumbnail, fast (stage 1)
    Screen,   // downsampled to the target size (stage 2)
    Full,     // full resolution (stage 3, when zooming beyond fit)
};

using DecodedImagePtr = std::shared_ptr<const DecodedImage>;

class ImageProvider : public QObject {
    Q_OBJECT
public:
    explicit ImageProvider(DecoderRegistry& registry, QObject* parent = nullptr);
    ~ImageProvider() override;

    static ImageProvider& instance();  // shared by all windows

    // Returns a cached image of at least the requested quality immediately if
    // available (may be nullptr), and schedules loading otherwise; `imageReady`
    // is emitted for every stage that becomes available.
    // `client` identifies the requesting window for prioritisation.
    DecodedImagePtr request(const QString& path, Quality quality, QSize target,
                            const void* client, int priority = 0);

    // Tell the provider which files a window will likely need next (prefetch
    // window, SPEC 7.1). `ordered` = paths by priority (current first).
    // Requests not in any client's set are cancelled/deprioritised.
    void setWanted(const void* client, const QStringList& ordered, QSize target, bool clientActive);
    void removeClient(const void* client);

    // Invalidate (file changed/renamed/deleted).
    void invalidate(const QString& path);

    void setMemoryBudget(qint64 bytes);  // P-09
    qint64 memoryUsage() const;

    // Synchronous metadata/info (cached), for overlays and the info panel.
    std::optional<ImageInfo> info(const QString& path);

signals:
    void imageReady(const QString& path, lf::Quality quality, lf::DecodedImagePtr image);
    void imageFailed(const QString& path, const QString& error);

private:
    struct Private;
    Private* d;
};

} // namespace lf

Q_DECLARE_METATYPE(lf::DecodedImagePtr)
