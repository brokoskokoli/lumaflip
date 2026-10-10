// SPDX-License-Identifier: GPL-3.0-or-later
#include "Language.h"

#include <QMutex>

#include <atomic>

namespace lf {

namespace {

// Native names as macOS shows them in Language & Region.
const QList<UiLanguage> kLanguages = {
    {"de", "Deutsch"},
    {"en", "English"},
    {"fr", "Français"},
    {"es", "Español"},
    {"it", "Italiano"},
    {"pt_BR", "Português (Brasil)"},
    {"pt_PT", "Português (Portugal)"},
    {"nl", "Nederlands"},
    {"pl", "Polski"},
    {"sv", "Svenska"},
    {"da", "Dansk"},
    {"nb", "Norsk bokmål"},
    {"fi", "Suomi"},
    {"cs", "Čeština"},
    {"ja", "日本語"},
    {"zh_CN", "简体中文"},
    {"zh_TW", "繁體中文"},
    {"ko", "한국어"},
};

QMutex g_localeMutex;
QLocale g_formatLocale(QLocale::German, QLocale::Germany);
std::atomic<int> g_generation{0};

QString pick(const QString& code, const QStringList& available) {
    return available.contains(code) ? code : QString();
}

} // namespace

const QList<UiLanguage>& uiLanguages() {
    return kLanguages;
}

QStringList uiLanguageCodes() {
    QStringList out;
    for (const UiLanguage& l : kLanguages)
        out << QString::fromLatin1(l.code);
    return out;
}

QString nativeLanguageName(const QString& code) {
    for (const UiLanguage& l : kLanguages)
        if (code == QLatin1StringView(l.code))
            return QString::fromUtf8(l.nativeName);
    return {};
}

QString matchUiLanguage(const QString& tag, const QStringList& available) {
    QString t = tag.trimmed();
    t.replace(QLatin1Char('_'), QLatin1Char('-'));
    t = t.section(QLatin1Char('@'), 0, 0); // POSIX modifiers ("de_DE@euro")
    t = t.section(QLatin1Char('.'), 0, 0); // POSIX charsets ("de_DE.UTF-8")
    const QStringList parts = t.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    const QString lang = parts.first().toLower();
    // Script (4 letters) and region (2 letters or 3 digits) subtags.
    QString script, region;
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const QString& p = parts[i];
        if (p.size() == 4 && script.isEmpty() && region.isEmpty())
            script = p.toLower();
        else if ((p.size() == 2 || (p.size() == 3 && p[0].isDigit())) && region.isEmpty())
            region = p.toUpper();
    }
    if (lang == u"zh") {
        const bool traditional =
            script == u"hant" ||
            (script.isEmpty() && (region == u"TW" || region == u"HK" || region == u"MO"));
        return pick(traditional ? QStringLiteral("zh_TW") : QStringLiteral("zh_CN"), available);
    }
    if (lang == u"pt")
        return pick(region.isEmpty() || region == u"BR" ? QStringLiteral("pt_BR") : QStringLiteral("pt_PT"),
                    available);
    if (lang == u"nb" || lang == u"no" || lang == u"nn")
        return pick(QStringLiteral("nb"), available);
    if (available.contains(lang))
        return lang;
    // Codes with a region in the list ("xx_YY") are only reached through the cases above.
    return {};
}

QString resolveUiLanguage(const QString& setting, const QStringList& perAppLanguages,
                          const QStringList& systemLanguages, const QStringList& available) {
    if (!setting.isEmpty() && setting != QLatin1String(kSystemLanguage) && available.contains(setting))
        return setting;
    for (const QStringList* list : {&perAppLanguages, &systemLanguages})
        for (const QString& tag : *list)
            if (const QString m = matchUiLanguage(tag, available); !m.isEmpty())
                return m;
    return available.contains(QLatin1String(kFallbackLanguage)) ? QString::fromLatin1(kFallbackLanguage)
                                                                : available.value(0);
}

QString appleLanguageTag(const QString& code) {
    if (code == QLatin1String("zh_CN"))
        return QStringLiteral("zh-Hans");
    if (code == QLatin1String("zh_TW"))
        return QStringLiteral("zh-Hant");
    QString tag = code;
    return tag.replace(QLatin1Char('_'), QLatin1Char('-'));
}

QLocale formatLocaleFor(const QString& code, const QLocale& system) {
    const QLocale ui(code);
    const bool sameScript = ui.script() == system.script() || ui.language() != QLocale::Chinese;
    if (system.language() == ui.language() && sameScript) {
        // Portuguese/Chinese variants: the region decides the variant, keep the user's
        // region formats only for the matching one.
        if (!code.contains(QLatin1Char('_')) || system.territory() == ui.territory() ||
            ui.language() == QLocale::Chinese)
            return system;
    }
    const QLocale regional(ui.language(), ui.script(), system.territory());
    if (regional.language() == ui.language() && regional.territory() == system.territory() &&
        !code.contains(QLatin1Char('_')))
        return regional;
    return ui;
}

QLocale formatLocale() {
    QMutexLocker lock(&g_localeMutex);
    return g_formatLocale;
}

void setFormatLocale(const QLocale& locale) {
    QMutexLocker lock(&g_localeMutex);
    g_formatLocale = locale;
}

int languageGeneration() {
    return g_generation.load();
}

void bumpLanguageGeneration() {
    ++g_generation;
}

} // namespace lf
