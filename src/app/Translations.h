// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// German UI throughout (CLAUDE.md, SPEC 5): our own strings are German already; this
// loads Qt's German translations (qtbase_de.qm) so that texts Qt generates are German
// too - the macOS application menu (Dienste, Lumaflip ausblenden, Alle anzeigen,
// Lumaflip beenden), QMessageBox buttons and Qt's dialogs.

#include <QString>
#include <QStringList>

namespace lf {

// Folders searched for qtbase_de.qm, in order: the app bundle's Resources/translations,
// then the Qt installation (QLibraryInfo::TranslationsPath).
QStringList qtTranslationDirs();
// Installs the translator (once) and returns the loaded file, empty if none was found.
// Number/date formats follow the system locale.
QString installQtTranslations();

} // namespace lf
