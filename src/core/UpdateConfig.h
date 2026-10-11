// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Configuration of the automatic updates (F-MAC-08, SPEC 10.1): Sparkle on macOS, WinSparkle
// on Windows (app/Updater.h). Plain functions, shared by both backends and unit-tested
// (tests/unit/test_update_config.cpp).

#include <QString>

namespace lf::update {

// The two appcasts on GitHub Pages (branch gh-pages, release.yml). Separate feeds: Sparkle on
// macOS never sees a Windows installer and WinSparkle never sees a DMG.
inline constexpr char kMacFeedUrl[] = "https://brokoskokoli.github.io/lumaflip/appcast.xml";
inline constexpr char kWindowsFeedUrl[] = "https://brokoskokoli.github.io/lumaflip/appcast-windows.xml";

// Empty if the feed URL and the EdDSA public key are usable, otherwise the reason (English,
// for the log). Never ship updates without a key: https feeds only (file: for tests, never
// any network), and a base64 ed25519 public key (32 bytes).
QString configurationProblem(const QString& feedUrl, const QString& publicKey);

// WinSparkle's language for Lumaflip's UI language code (Language.h: "de", "pt_BR",
// "zh_TW" ...): ISO 639 with an optional region after a hyphen ("pt-BR"). Empty for an empty
// or "system" code (WinSparkle then follows Windows).
QString winSparkleLanguage(const QString& uiLanguageCode);

} // namespace lf::update
