// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): images inside archives are extracted on demand into a private cache, so
// every decoder (ImageIO in the app, the library decoders in the helper) reads them like any
// other file. PDF pages are rendered directly (PdfDocument.h) and never extracted.
//
// The cache is a directory of this process (QTemporaryDir in the user's temporary folder,
// mode 0700, removed when the app quits; directories a crashed run left behind are swept,
// sweepStaleDirectories()). A member is stored as "<hash>/<its file name>": the hash covers
// the archive path, its size and date and the member name, so a changed archive never serves
// stale members, and the file name is the member's own (other apps show it, "Öffnen mit").
// Older files are dropped once the cache exceeds its budget (default 512 MiB); a file that is
// being read (Pin) is never dropped. Extraction goes through archive::readRun() (the decoder
// helper when there is one): RAR and 7z archives are read front to back, so the members
// after the wanted one come along in the same pass (solid archives decompress them anyway)
// and browsing on does not start from the beginning of the archive for every image.
//
// Thread-safe: worker threads materialize concurrently; one member is extracted once, and
// one archive read front to back is read by one thread at a time.

#include "Archive.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace lf {

class ArchiveCache {
public:
    using Reader = std::function<std::optional<QByteArray>(const QString& container, const QString& member,
                                                           QString* error)>;
    using RunReader = std::function<std::optional<QVector<archive::Extracted>>(
        const QString& container, const QString& member, QString* error)>;

    static ArchiveCache& instance();
    // `reader` (tests): one member per call; else `runReader`, else archive::readRun().
    explicit ArchiveCache(Reader reader = {}, qint64 maxBytes = qint64(512) << 20, RunReader runReader = {});
    ~ArchiveCache();
    ArchiveCache(const ArchiveCache&) = delete;
    ArchiveCache& operator=(const ArchiveCache&) = delete;

    // An extracted file that stays while the Pin lives (eviction skips it).
    class Pin {
    public:
        Pin() = default;
        Pin(Pin&& other) noexcept;
        Pin& operator=(Pin&& other) noexcept;
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        ~Pin();
        QString file() const { return m_file; }
        bool isNull() const { return m_file.isEmpty(); }

    private:
        friend class ArchiveCache;
        void release();
        ArchiveCache* m_cache = nullptr;
        QString m_key;
        QString m_file;
    };

    // The extracted file for the member path `memberPath` (vpath), extracting it if needed,
    // pinned. Null + `error` (user facing) if it cannot be read.
    Pin acquire(const QString& memberPath, QString* error);
    // Like acquire(), unpinned (the file may go once the cache needs room).
    QString materialize(const QString& memberPath, QString* error);
    // The extracted file if the member is in the cache already, else empty. Never extracts
    // (cheap: GUI thread, e.g. "Öffnen mit" for the shown image).
    QString cachedFile(const QString& memberPath) const;
    // Already extracted (tests).
    bool contains(const QString& memberPath) const;
    QString directory() const;
    qint64 cachedBytes() const;
    void clear();

    // Removes cache directories ("lumaflip-archive-*" in the temporary folder) of earlier
    // runs that did not end cleanly (untouched for `minAgeSecs`). Blocking: a worker thread.
    // Returns the number removed.
    static int sweepStaleDirectories(qint64 minAgeSecs = 12 * 3600, const QString& tempDir = {});

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
