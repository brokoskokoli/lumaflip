// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): images inside archives are extracted on demand into a private cache, so
// every decoder (ImageIO in the app, the library decoders in the helper) reads them like any
// other file. PDF pages are rendered directly (PdfDocument.h) and never extracted.
//
// The cache is a directory of this process (QTemporaryDir in the user's temporary folder,
// mode 0700, removed when the app quits). A file is named after a hash of the archive path,
// its size and date and the member name, so a changed archive never serves stale members.
// Older files are dropped once the cache exceeds its budget (default 512 MiB). Extraction
// goes through archive::read() (the decoder helper when there is one).
//
// Thread-safe: worker threads materialize concurrently; one member is extracted once.

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace lf {

class ArchiveCache {
public:
    using Reader = std::function<std::optional<QByteArray>(const QString& container, const QString& member,
                                                           QString* error)>;

    static ArchiveCache& instance();
    explicit ArchiveCache(Reader reader = {}, qint64 maxBytes = qint64(512) << 20);
    ~ArchiveCache();
    ArchiveCache(const ArchiveCache&) = delete;
    ArchiveCache& operator=(const ArchiveCache&) = delete;

    // The extracted file for the member path `memberPath` (vpath), extracting it if needed.
    // Empty + `error` (user facing) if it cannot be read.
    QString materialize(const QString& memberPath, QString* error);
    // Already extracted (tests).
    bool contains(const QString& memberPath) const;
    QString directory() const;
    qint64 cachedBytes() const;
    void clear();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
