// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-08: configuration of the automatic updates shared by Sparkle (macOS) and WinSparkle
// (Windows): feed URLs, key validation, WinSparkle's language tag (core/UpdateConfig.h).

#include "core/UpdateConfig.h"

#include <gtest/gtest.h>

using namespace lf;

namespace {

// 32 bytes, base64: a syntactically valid ed25519 public key (never used to verify).
const QString kKey = QStringLiteral("dGVzdC1rZXktbHVtYWZsaXAtdXBkYXRlci10ZXN0cyE=");

} // namespace

TEST(UpdateConfig, F_MAC_08_SeparateFeedsForMacAndWindows) {
    const QString mac = QLatin1String(update::kMacFeedUrl);
    const QString win = QLatin1String(update::kWindowsFeedUrl);
    EXPECT_EQ(mac, QStringLiteral("https://brokoskokoli.github.io/lumaflip/appcast.xml"));
    EXPECT_EQ(win, QStringLiteral("https://brokoskokoli.github.io/lumaflip/appcast-windows.xml"));
    EXPECT_NE(mac, win);
    EXPECT_TRUE(update::configurationProblem(mac, kKey).isEmpty());
    EXPECT_TRUE(update::configurationProblem(win, kKey).isEmpty());
}

TEST(UpdateConfig, F_MAC_08_NeverWithoutKeyOrHttps) {
    const QString https = QLatin1String(update::kWindowsFeedUrl);
    EXPECT_TRUE(update::configurationProblem(QStringLiteral("file:///tmp/appcast.xml"), kKey).isEmpty());
    EXPECT_FALSE(update::configurationProblem(https, QString()).isEmpty());
    EXPECT_FALSE(update::configurationProblem(https, QStringLiteral("not base64!")).isEmpty());
    EXPECT_FALSE(update::configurationProblem(https, QStringLiteral("c2hvcnQ=")).isEmpty()); // 5 bytes
    EXPECT_FALSE(
        update::configurationProblem(QStringLiteral("http://example.org/appcast.xml"), kKey).isEmpty());
    EXPECT_FALSE(
        update::configurationProblem(QStringLiteral("ftp://example.org/appcast.xml"), kKey).isEmpty());
    EXPECT_FALSE(update::configurationProblem(QStringLiteral("https:///appcast.xml"), kKey).isEmpty());
    EXPECT_FALSE(update::configurationProblem(QString(), kKey).isEmpty());
}

TEST(UpdateConfig, F_MAC_08_WinSparkleLanguageTags) {
    EXPECT_EQ(update::winSparkleLanguage(QStringLiteral("de")), QStringLiteral("de"));
    EXPECT_EQ(update::winSparkleLanguage(QStringLiteral("pt_BR")), QStringLiteral("pt-BR"));
    EXPECT_EQ(update::winSparkleLanguage(QStringLiteral("zh_TW")), QStringLiteral("zh-TW"));
    EXPECT_TRUE(update::winSparkleLanguage(QString()).isEmpty());
    EXPECT_TRUE(update::winSparkleLanguage(QStringLiteral("system")).isEmpty());
}
