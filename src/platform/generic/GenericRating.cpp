// SPDX-License-Identifier: GPL-3.0-or-later
// Portable rating storage parts (F-CULL-02): embedded ratings are only read (the first XMP
// packet in the file), never written (no lossless metadata writer here: the sidecar is
// used instead); there are no Finder tags.

#include "core/RatingStore.h"
#include "core/Xmp.h"

#include <QCoreApplication>
#include <QFile>

namespace lf::platform {

bool canEmbedRatingType(const QString&) {
    return false;
}

std::optional<Rating> readEmbeddedRating(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QString packet = xmp::findPacket(f.read(64 * 1024 * 1024));
    return packet.isEmpty() ? std::nullopt : xmp::readRating(packet);
}

bool writeEmbeddedRating(const QString&, const QString&, const Rating&, QString* error) {
    if (error)
        *error = QCoreApplication::translate(
            "lf::RatingStore", "In dieses Format kann die Bewertung nicht sicher geschrieben werden.");
    return false;
}

bool tagsSupported() {
    return false;
}

std::optional<QStringList> readRawTags(const QString&, QString*) {
    return QStringList();
}

bool writeRawTags(const QString&, const QStringList&, QString* error) {
    if (error)
        *error = QCoreApplication::translate("lf::RatingStore",
                                             "Finder-Tags werden auf diesem System nicht unterstützt.");
    return false;
}

} // namespace lf::platform
