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

// Quality (the pipeline stages) is declared in Decoder.h.

using DecodedImagePtr = std::shared_ptr<const DecodedImage>;

class ImageProvider : public QObject {
    Q_OBJECT
public:
    explicit ImageProvider(DecoderRegistry& registry, QObject* parent = nullptr);
    // Uses DecoderRegistry::instance(), built on the first decode (not when constructed).
    explicit ImageProvider(QObject* parent = nullptr);
    ~ImageProvider() override;

    static ImageProvider& instance(); // shared by all windows

    // P-01: starts decoding `path` (normalized absolute path, as collections use it) at
    // Screen quality for a view of `target` device pixels on a background thread, with
    // the platform's primary decoder only. May be called before the application object
    // exists. The provider that first requests `path` adopts the result (or the running
    // decode) instead of decoding again; a failure is ignored (the regular decode reports).
    static void prestart(const QString& path, QSize target);

    // Returns a cached image of at least the requested quality immediately if
    // available (may be nullptr), and schedules loading otherwise; `imageReady`
    // is emitted for every stage that becomes available.
    // `client` identifies the requesting window for prioritisation.
    // F-FMT-04: `page` selects a page of a multi-page file; pages are cached separately and
    // DecodedImage::page tells which page an imageReady() result belongs to.
    DecodedImagePtr request(const QString& path, Quality quality, QSize target, const void* client,
                            int priority = 0, int page = 0);

    // Tell the provider which files a window will likely need next (prefetch
    // window, SPEC 7.1). `ordered` = paths by priority (current first).
    // Requests not in any client's set are cancelled/deprioritised.
    void setWanted(const void* client, const QStringList& ordered, QSize target, bool clientActive);
    void removeClient(const void* client);

    // Best cached image for `path` of at least `minQuality` (Preview = any stage)
    // without scheduling anything or changing a client's state; nullptr if none.
    // Windows use it to hand decoded neighbours to the view (P-03) and to show a
    // cached stage instantly when flipping.
    DecodedImagePtr cached(const QString& path, Quality minQuality = Quality::Preview, QSize target = {},
                           int page = 0);

    // Invalidate (file changed/renamed/deleted). Drops every page of the file.
    void invalidate(const QString& path);

    void setMemoryBudget(qint64 bytes); // P-09
    qint64 memoryBudget() const;
    static qint64 defaultMemoryBudget(); // 25 % of the RAM, at most 2 GB
    // P-08: no decoded image is larger than this (half the budget, at most 1 GB, at
    // least 16 MB); bigger images are downsampled while decoding.
    qint64 maxDecodeBytes() const;
    // Setting "RAW: nur eingebettete Vorschau" (F-FMT-05): every stage of a camera RAW
    // file is taken from its embedded preview instead of a full development.
    void setRawEmbeddedOnly(bool embeddedOnly);
    bool rawEmbeddedOnly() const;
    qint64 memoryUsage() const;

    // Synchronous metadata/info (cached), for overlays and the info panel.
    std::optional<ImageInfo> info(const QString& path);

    // Number of entries in the per-path bookkeeping tables (info cache, failures,
    // generations). Diagnostics/tests: stays bounded however many files are visited.
    int bookkeepingEntries() const;

signals:
    // `quality`: the stage the image is cached as; `image->quality` is the same, or Full
    // if the image holds the full resolution anyway (small files).
    void imageReady(const QString& path, lf::Quality quality, lf::DecodedImagePtr image);
    // Nothing of page 0 of `path` can be shown (every stage failed and none is cached).
    // Follows pageFailed(path, 0, error).
    void imageFailed(const QString& path, const QString& error);
    // F-FMT-04: nothing of `page` of `path` can be shown. Other pages are not affected.
    void pageFailed(const QString& path, int page, const QString& error);
    // One stage failed while another stage of the same page of `path` is (or may still
    // become) available,
    // e.g. the full-resolution decode of a huge image while the screen stage is shown.
    // The failed stage is not retried until invalidate(path).
    void stageFailed(const QString& path, lf::Quality quality, const QString& error);
    // Everything cached of `path` was dropped (file changed, RAW setting changed). What a
    // window shows of it is outdated; the stages it wants are decoded again.
    void invalidated(const QString& path);

private:
    void init();
    struct Private;
    Private* d;
};

} // namespace lf

Q_DECLARE_METATYPE(lf::DecodedImagePtr)
