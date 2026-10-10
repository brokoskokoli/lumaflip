// SPDX-License-Identifier: GPL-3.0-or-later
#include "Translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace lf {

QStringList qtTranslationDirs() {
    return {QDir::cleanPath(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/../Resources/translations")),
            QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
}

QString installQtTranslations() {
    static QTranslator* translator = nullptr;
    if (translator)
        return translator->filePath();
    // The UI is German only for now, so Qt's texts are German regardless of the system
    // language; formats (numbers, dates) keep following the system locale.
    if (QLocale::system().language() == QLocale::German)
        QLocale::setDefault(QLocale::system());
    auto* t = new QTranslator(QCoreApplication::instance());
    for (const QString& dir : qtTranslationDirs()) {
        if (t->load(QLocale(QLocale::German, QLocale::Germany), QStringLiteral("qtbase"), QStringLiteral("_"),
                    dir)) {
            QCoreApplication::installTranslator(t);
            translator = t;
            return t->filePath();
        }
    }
    delete t;
    return {};
}

} // namespace lf
