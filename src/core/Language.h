// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// UI languages (SPEC 4.16, F-I18N-02/03/04): the shipped languages, matching the user's
// preferred languages against them, and the locale numbers, sizes and dates are formatted
// with. Platform independent and side-effect free (unit-tested); the app installs the
// translators (app/Translations.h).

#include <QList>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace lf {

struct UiLanguage {
    const char* code;       // suffix of translations/lumaflip_<code>.ts; "de" = source language
    const char* nativeName; // name in the language itself ("Deutsch", "日本語")
};

// The source language first ("de"), then the translations in the order of F-I18N-02.
const QList<UiLanguage>& uiLanguages();
QStringList uiLanguageCodes();
// "Français" for "fr"; empty for unknown codes.
QString nativeLanguageName(const QString& code);

inline constexpr auto kSystemLanguage = "system"; // setting value: follow macOS
inline constexpr auto kFallbackLanguage = "en";   // none of the preferred languages is shipped

// Maps one language tag of the system ("de-CH", "zh-Hant-TW", "pt", "nb-NO", "en_GB") to a
// shipped language code, or "" if there is none. Chinese goes by script/region (Hant, TW,
// HK, MO -> zh_TW, else zh_CN), Portuguese by region (BR or none -> pt_BR, else pt_PT),
// Norwegian "no"/"nn"/"nb" -> nb, everything else by its language part.
QString matchUiLanguage(const QString& tag, const QStringList& available = uiLanguageCodes());

// F-I18N-03: the UI language, in this order: the explicit setting (key::Language, unless
// "system" or not shipped), the macOS per-app language (AppleLanguages in the app's own
// defaults domain, System Settings -> General -> Language & Region -> Applications), the
// system's preferred languages in order (QLocale::uiLanguages()), English.
QString resolveUiLanguage(const QString& setting, const QStringList& perAppLanguages,
                          const QStringList& systemLanguages,
                          const QStringList& available = uiLanguageCodes());

// The language tag macOS uses for a UI language code in AppleLanguages and .lproj folders
// ("pt_BR" -> "pt-BR", "zh_CN" -> "zh-Hans", "zh_TW" -> "zh-Hant", "fr" -> "fr").
QString appleLanguageTag(const QString& code);

// F-I18N-04: the locale texts in UI language `code` are formatted with. The system locale
// (macOS region settings) if it is of the same language ("de" + de_CH keeps the Swiss
// formats); else the UI language in the system's region if that combination exists
// (English UI in Germany: en_DE, "2,8 MB"); else the UI language's own default locale.
QLocale formatLocaleFor(const QString& code, const QLocale& system);

// The locale of the formatting helpers (LocaleFormat.h), any thread. German (the source
// language) until the app sets the one of the UI language.
QLocale formatLocale();
void setFormatLocale(const QLocale& locale);

// Increases whenever the app switches the UI language; caches of translated texts (e.g.
// InputBindings::inputSlots()) compare it to rebuild themselves.
int languageGeneration();
void bumpLanguageGeneration();

} // namespace lf
