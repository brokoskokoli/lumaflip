// SPDX-License-Identifier: GPL-3.0-or-later
#include "LanguageChooser.h"

#include "core/Language.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QLocale>

namespace lf::prefs {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PreferencesDialog)
};

// The language "system" stands for right now (setting ignored).
QString systemChoice() {
    return resolveUiLanguage(QString::fromLatin1(kSystemLanguage), appLanguagePreference(),
                             QLocale::system().uiLanguages(QLocale::TagSeparator::Dash));
}

void fill(QComboBox* c) {
    QSignalBlocker block(c);
    c->clear();
    //: first entry of the language list; %1 is the language macOS asks for, in that language
    c->addItem(Tr::tr("Systemsprache (%1)").arg(nativeLanguageName(systemChoice())),
               QString::fromLatin1(kSystemLanguage));
    c->insertSeparator(1);
    for (const UiLanguage& l : uiLanguages())
        c->addItem(QString::fromUtf8(l.nativeName), QString::fromLatin1(l.code));
    c->setCurrentIndex(std::max(0, c->findData(Settings::instance().value(key::Language).toString())));
}

} // namespace

QComboBox* createLanguageChooser(QWidget* parent) {
    auto* c = new QComboBox(parent);
    c->setObjectName(QStringLiteral("setting:") + key::Language);
    fill(c);
    QObject::connect(c, &QComboBox::activated, c, [c](int i) {
        const QString value = c->itemData(i).toString();
        if (!value.isEmpty())
            Settings::instance().setValue(key::Language, value);
    });
    QObject::connect(&Settings::instance(), &Settings::changed, c, [c](const QString& k) {
        if (k == QLatin1String(key::Language))
            fill(c);
    });
    return c;
}

} // namespace lf::prefs
