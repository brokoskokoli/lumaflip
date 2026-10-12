// SPDX-License-Identifier: GPL-3.0-or-later
#include "SortSpec.h"

#include "Platform.h"

#include <QCoreApplication>

namespace lf {

QString sortKeyLabel(SortKey key) {
    const char* text = QT_TRANSLATE_NOOP("SortSpec", "Name");
    switch (key) {
    case SortKey::Name:
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

QString describe(const SortSpec& spec, FileManager fm) {
    // Icon positions have no direction of their own.
    QString keyPart = sortKeyLabel(spec.key);
    if (spec.key != SortKey::FinderIconPosition)
        keyPart += spec.ascending ? QStringLiteral(" ↑") : QStringLiteral(" ↓");
    switch (spec.source) {
    case SortSource::FinderLive: // F-SORT-04: asked the front Finder window
        return QCoreApplication::translate("SortSpec", "Wie im Finder (live): %1").arg(keyPart);
    case SortSource::FinderDsStore: // also on Windows/Linux: a .DS_Store a Mac left there
    case SortSource::FinderGlobal:
        return QCoreApplication::translate("SortSpec", "Wie im Finder: %1").arg(keyPart);
    case SortSource::ExplorerLive: // read from the Explorer window showing the folder
        return QCoreApplication::translate("SortSpec", "Wie im Explorer: %1").arg(keyPart);
    case SortSource::ExplorerUnsupported:
        return QCoreApplication::translate("SortSpec", "%1 (Sortierung des Explorers nicht unterstützt)")
            .arg(keyPart);
    case SortSource::Fallback:
        switch (fm) {
        case FileManager::Finder:
            return QCoreApplication::translate("SortSpec", "%1 (Finder-Informationen nicht gefunden)")
                .arg(keyPart);
        case FileManager::Explorer:
            return QCoreApplication::translate("SortSpec", "%1 (Explorer-Ansicht nicht gefunden)")
                .arg(keyPart);
        case FileManager::Other:
            return QCoreApplication::translate("SortSpec", "%1 (Sortierung des Dateimanagers nicht bekannt)")
                .arg(keyPart);
        }
        break;
    case SortSource::Manual:
        break;
    }
    return keyPart;
}

QString describe(const SortSpec& spec) {
    return describe(spec, platformFileManager());
}

} // namespace lf
