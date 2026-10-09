// SPDX-License-Identifier: GPL-3.0-or-later
#include "SortSpec.h"

#include <QCoreApplication>

namespace lf {

QString sortKeyLabel(SortKey key) {
    const char* text = "Name";
    switch (key) {
    case SortKey::Name:
        text = QT_TRANSLATE_NOOP("SortSpec", "Name");
        break;
    case SortKey::DateModified:
        text = QT_TRANSLATE_NOOP("SortSpec", "Änderungsdatum");
        break;
    case SortKey::DateCreated:
        text = QT_TRANSLATE_NOOP("SortSpec", "Erstellungsdatum");
        break;
    case SortKey::DateAdded:
        text = QT_TRANSLATE_NOOP("SortSpec", "Hinzugefügt am");
        break;
    case SortKey::DateCaptured:
        text = QT_TRANSLATE_NOOP("SortSpec", "Aufnahmedatum");
        break;
    case SortKey::Size:
        text = QT_TRANSLATE_NOOP("SortSpec", "Größe");
        break;
    case SortKey::Kind:
        text = QT_TRANSLATE_NOOP("SortSpec", "Art");
        break;
    case SortKey::PixelCount:
        text = QT_TRANSLATE_NOOP("SortSpec", "Pixelzahl");
        break;
    case SortKey::Rating:
        text = QT_TRANSLATE_NOOP("SortSpec", "Bewertung");
        break;
    case SortKey::FinderIconPosition:
        text = QT_TRANSLATE_NOOP("SortSpec", "Symbolposition");
        break;
    }
    return QCoreApplication::translate("SortSpec", text);
}

QString describe(const SortSpec& spec) {
    // Icon positions have no direction of their own.
    QString keyPart = sortKeyLabel(spec.key);
    if (spec.key != SortKey::FinderIconPosition)
        keyPart += spec.ascending ? QStringLiteral(" ↑") : QStringLiteral(" ↓");
    switch (spec.source) {
    case SortSource::FinderLive:
    case SortSource::FinderDsStore:
    case SortSource::FinderGlobal:
        return QCoreApplication::translate("SortSpec", "Wie Finder: %1").arg(keyPart);
    case SortSource::Fallback:
        return QCoreApplication::translate("SortSpec", "%1 (Finder-Info nicht gefunden)").arg(keyPart);
    case SortSource::Manual:
        break;
    }
    return keyPart;
}

} // namespace lf
