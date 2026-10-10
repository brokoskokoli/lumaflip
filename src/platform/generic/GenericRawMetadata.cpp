// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-05 without ImageIO: the text keys Qt's image readers provide (PNG text chunks,
// JPEG comments …). Full EXIF/XMP trees need a metadata library on these platforms (M4).

#include "core/MetadataTree.h"

#include <QFileInfo>
#include <QImageReader>
#include <QVariantMap>

namespace lf {

MetadataNode readRawMetadata(const QString& path) {
    MetadataNode root;
    root.key = QFileInfo(path).fileName();
    QImageReader r(path);
    QVariantMap general;
    if (r.canRead()) {
        general.insert(QStringLiteral("Format"), QString::fromLatin1(r.format()));
        general.insert(QStringLiteral("Size"),
                       QStringLiteral("%1 × %2").arg(r.size().width()).arg(r.size().height()));
        for (const QString& k : r.textKeys())
            general.insert(k, r.text(k));
    }
    if (!general.isEmpty())
        root.children.append(metadatatree::fromVariant(QStringLiteral("Qt"), general));
    return root;
}

} // namespace lf
