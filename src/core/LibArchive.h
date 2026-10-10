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

#include <optional>

namespace lf::libarchive {

bool available();
archive::Listing list(const QString& path);
// The member `name` (compared in Unicode NFC), at most `maxBytes` bytes: a member that
// produces more fails with archive::messageMemberTooLarge().
std::optional<QByteArray> read(const QString& path, const QString& name, qint64 maxBytes, QString* error);

// Tests: writes an archive in `format` ("7zip", "zip", "ustar") with these files.
bool writeArchive(const QString& path, const QString& format, const QList<QPair<QString, QByteArray>>& files,
                  QString* error);

} // namespace lf::libarchive
