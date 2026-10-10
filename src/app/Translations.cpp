// SPDX-License-Identifier: GPL-3.0-or-later
#include "Translations.h"

#include "core/Language.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QTranslator>

namespace lf {

namespace {

struct Installed {
    QString code;
    QPointer<QTranslator> app;
    QPointer<QTranslator> qt;
};

Installed& installed() {
    static Installed i;
    return i;
}

QString bundleTranslationDir() {
#ifdef Q_OS_MACOS
    return QDir::cleanPath(QCoreApplication::applicationDirPath() +
                           QStringLiteral("/../Resources/translations"));
#else
    // XDG install layout (bin/lumaflip, share/lumaflip/translations; also inside an AppImage).
    return QDir::cleanPath(QCoreApplication::applicationDirPath() +
                           QStringLiteral("/../share/lumaflip/translations"));
#endif
}

// Loads the first of `names` found in `dirs` ("qtbase_fr.qm" …); nullptr if none.
QTranslator* loadCatalog(const QStringList& names, const QStringList& dirs) {
    auto* t = new QTranslator(QCoreApplication::instance());
    for (const QString& name : names)
        for (const QString& dir : dirs)
            if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".qm")) &&
                t->load(name, dir))
                return t;
    delete t;
    return nullptr;
}

void replace(QPointer<QTranslator>& slot, QTranslator* next) {
    if (slot) {
        QCoreApplication::removeTranslator(slot);
        delete slot.data();
    }
    slot = next;
    if (next)
        QCoreApplication::installTranslator(next);
}

} // namespace

QStringList qtTranslationDirs() {
    return {bundleTranslationDir(), QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
}

QStringList appTranslationDirs() {
    QStringList dirs{bundleTranslationDir()};
#ifdef LF_TRANSLATIONS_BUILD_DIR
    dirs << QStringLiteral(LF_TRANSLATIONS_BUILD_DIR);
#endif
    return dirs;
}

QStringList qtTranslationCandidates(const QString& code) {
    if (code == QLatin1String("pt_PT"))
        return {QStringLiteral("pt_PT"), QStringLiteral("pt"), QStringLiteral("pt_BR")};
    if (code == QLatin1String("nb"))
        return {QStringLiteral("nb"), QStringLiteral("no"), QStringLiteral("nn")};
    QStringList out{code};
    if (code.contains(QLatin1Char('_')))
        out << code.section(QLatin1Char('_'), 0, 0);
    return out;
}

QString resolvedUiLanguage() {
    return resolveUiLanguage(Settings::instance().value(key::Language).toString(), appLanguagePreference(),
                             QLocale::system().uiLanguages(QLocale::TagSeparator::Dash));
}

bool installTranslations(const QString& code) {
    Installed& i = installed();
    if (i.code == code && (i.app || i.qt))
        return true;
    QStringList qtNames;
    for (const QString& c : qtTranslationCandidates(code))
        qtNames << QStringLiteral("qtbase_") + c;
    QTranslator* qt = loadCatalog(qtNames, qtTranslationDirs());
    // German is the source language: its catalogue only holds the plural forms.
    QTranslator* app = loadCatalog({QStringLiteral("lumaflip_") + code}, appTranslationDirs());
    const bool first = i.code.isEmpty();
    i.code = code;
    // Formats first: widgets re-read texts on LanguageChange (sent by installTranslator).
    const QLocale format = formatLocaleFor(code, QLocale::system());
    setFormatLocale(format);
    // QLocale::setDefault() is not thread-safe against workers that construct QLocale():
    // only at startup (main(), before any worker runs). Afterwards everything that formats
    // uses formatLocale() explicitly (LocaleFormat.h).
    if (first)
        QLocale::setDefault(format);
    replace(i.qt, qt);
    replace(i.app, app);
    // After the translators: a cache rebuilt for the new generation (InputBindings::
    // inputSlots(), possibly on a worker) must not see the old catalogues.
    bumpLanguageGeneration();
    return qt && (app || code == QLatin1String("de"));
}

QString installedUiLanguage() {
    return installed().code;
}

QString installedQtCatalog() {
    return installed().qt ? installed().qt->filePath() : QString();
}

QString installedAppCatalog() {
    return installed().app ? installed().app->filePath() : QString();
}

QString installQtTranslations() {
    installTranslations(resolvedUiLanguage());
    return installedQtCatalog();
}

} // namespace lf
