// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// UI language (SPEC 4.16): loads Lumaflip's translations (translations/lumaflip_<code>.qm;
// German is the source language, its catalogue only holds plural forms) and Qt's own
// (qtbase_<code>.qm: the macOS application menu - Dienste, Lumaflip ausblenden, Alle
// anzeigen, Lumaflip beenden -, QMessageBox buttons, Qt's dialogs) and sets the locale for
// numbers, sizes and dates (F-I18N-04, core/Language.h).
//
// Live switching (F-I18N-03): installTranslations() replaces the translators; Qt then sends
// QEvent::LanguageChange to the application and every widget. The menu bar, the
// preferences and the shortcut overview are rebuilt by the app shell (Application), the
// windows retranslate their texts in changeEvent().

#include <QString>
#include <QStringList>

namespace lf {

// Folders searched for qtbase_<code>.qm, in order: the app bundle's Resources/translations,
// then the Qt installation (QLibraryInfo::TranslationsPath).
QStringList qtTranslationDirs();
// Folders searched for lumaflip_<code>.qm: the app bundle's Resources/translations, then
// the build tree (LF_TRANSLATIONS_BUILD_DIR, for tests and unbundled builds).
QStringList appTranslationDirs();
// Qt catalogues to try for a UI language (Qt ships none for pt_PT and nb):
// "pt_PT" -> pt_PT, pt, pt_BR; "nb" -> nb, no, nn; "fr" -> fr.
QStringList qtTranslationCandidates(const QString& code);

// F-I18N-03: the language to use now: key::Language, the macOS per-app language, the
// system's preferred languages, English (resolveUiLanguage()).
QString resolvedUiLanguage();

// Installs the translators for `code` (a code of uiLanguageCodes()), replacing the ones
// installed before, and sets the formatting locale. Nothing happens if `code` is installed
// already. Returns true if Lumaflip's catalogue (or, for German, none is needed) and Qt's
// were found.
bool installTranslations(const QString& code);
QString installedUiLanguage(); // empty before the first installTranslations()
QString installedQtCatalog();  // file of the loaded qtbase catalogue, empty if none
QString installedAppCatalog(); // file of the loaded lumaflip catalogue, empty if none

// main(): installTranslations(resolvedUiLanguage()) before the menu bar exists. Returns
// the loaded qtbase catalogue (empty if none was found).
QString installQtTranslations();

} // namespace lf
