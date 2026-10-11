// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// libarchive access for Archive.h (RAR/CBR, 7z/CB7, ZIP variants). Internal to the core.
// Built only with LF_HAVE_LIBARCHIVE (macOS: the system's /usr/lib/libarchive.2.dylib);
// otherwise every call fails with archive::messageUnsupportedArchive().

#include "Archive.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

#include <QVector>

#include <functional>
#include <optional>

namespace lf::libarchive {

bool available();
// The members; skipping their data counts against `scanLimit` (solid RAR archives decompress
// it): the listing ends where the budget is used up.
archive::Listing list(const QString& path, qint64 scanLimit = archive::kMaxScanBytes);
// The member `name` (compared in Unicode NFC), at most `maxBytes` bytes: a member that
// produces more fails with archive::messageMemberTooLarge(). Passing over the members before
// it counts against `scanLimit` (the stated size of each, or what it really produces when no
// size is stated): beyond that it fails with archive::messageArchiveTooLarge().
std::optional<QByteArray> read(const QString& path, const QString& name, qint64 maxBytes, QString* error,
                               qint64 scanLimit = archive::kMaxScanBytes);
// Like read(), in the same pass also the members after `name` (archive order) that `follow`
// accepts, as long as their data together stays within `maxRunBytes` (each at most
// `maxBytes`); result[0] is `name`. In a solid archive these were decompressed anyway.
using RunFilter = std::function<bool(const QString& member)>;
std::optional<QVector<archive::Extracted>> readRun(const QString& path, const QString& name, qint64 maxBytes,
                                                   qint64 maxRunBytes, const RunFilter& follow,
                                                   QString* error, qint64 scanLimit = archive::kMaxScanBytes);

// Tests: writes an archive in `format` ("7zip", "zip", "ustar") with these files.
bool writeArchive(const QString& path, const QString& format, const QList<QPair<QString, QByteArray>>& files,
                  QString* error);

} // namespace lf::libarchive
