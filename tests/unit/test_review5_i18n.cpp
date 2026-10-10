// SPDX-License-Identifier: GPL-3.0-or-later
// Review findings (i18n, SPEC 4.16): time separators per locale, numbers without group
// separators in exposure values, AppleLanguages for an explicit language, the input slot
// table across language switches, and the stricter i18n checker (receiver-qualified code
// calls, labels before ':'/'%', placeholders in .ts, InfoPlist.strings completeness).
// Requirements: F-I18N-01, F-I18N-03, F-I18N-04, F-I18N-05.

#include "core/InputBindings.h"
#include "core/Language.h"
#include "core/LocaleFormat.h"
#include "core/Platform.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

#include <atomic>
#include <thread>

using namespace lf;

namespace {

QString runPython(const QStringList& args, int* exitCode) {
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        *exitCode = -1;
        return {};
    }
    QProcess p;
    p.start(python, args);
    if (!p.waitForFinished(60000)) {
        *exitCode = -2;
        return {};
    }
    *exitCode = p.exitCode();
    return QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

// ---- F-I18N-04: dates and exposure values ----

TEST(I18nReview5, F_I18N_04_SecondsUseTheLocalesTimeSeparator) {
    const QDateTime dt(QDate(2026, 10, 10), QTime(14, 3, 9), QTimeZone::UTC);
    const QString da = formatDateTime(dt, QLocale(QLocale::Danish, QLocale::Denmark));
    EXPECT_TRUE(da.endsWith(QStringLiteral("14.03.09"))) << da.toStdString();
    const QString fi = formatDateTime(dt, QLocale(QLocale::Finnish, QLocale::Finland));
    EXPECT_TRUE(fi.endsWith(QStringLiteral("14.03.09"))) << fi.toStdString();
    EXPECT_EQ(formatDateTime(dt, QLocale(QLocale::German, QLocale::Germany)),
              QStringLiteral("10.10.2026 14:03:09"));
    const QString en = formatDateTime(dt, QLocale(QLocale::English, QLocale::UnitedStates));
    EXPECT_TRUE(en.contains(QStringLiteral("2:03:09"))) << en.toStdString();
    const QString ja = formatDateTime(dt, QLocale(QLocale::Japanese, QLocale::Japan));
    EXPECT_TRUE(ja.contains(QStringLiteral("14:03:09"))) << ja.toStdString();
    // Never a mixed "14.03:09".
    for (const QString& s : {da, fi})
        EXPECT_FALSE(s.contains(QStringLiteral(".03:09"))) << s.toStdString();
}

TEST(I18nReview5, F_I18N_04_ExposureNumbersWithoutGroupSeparators) {
    const QLocale de(QLocale::German, QLocale::Germany);
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    EXPECT_EQ(formatExposureTime(1.0 / 4000, de), QStringLiteral("1/4000 s"));
    EXPECT_EQ(formatExposureTime(1.0 / 8000, en), QStringLiteral("1/8000 s"));
    EXPECT_EQ(formatExposureTime(1.0 / 250, de), QStringLiteral("1/250 s"));
    EXPECT_EQ(formatExposureTime(1500, de), QStringLiteral("1500 s"));
    EXPECT_EQ(formatFocalLength(1200, 1200, de), QStringLiteral("1200 mm"));
    EXPECT_EQ(formatFocalLength(800, 1200, en), QStringLiteral("800 mm (KB: 1200 mm)"));
    EXPECT_EQ(formatAperture(2.8, de), QStringLiteral("f/2,8"));
    // Altitudes keep the locale's grouping (a large number, not a token).
    EXPECT_EQ(formatAltitude(8848, de), QStringLiteral("8.848 m"));
    // The caller's locale is not changed.
    EXPECT_FALSE(de.numberOptions() & QLocale::OmitGroupSeparator);
}

// ---- F-I18N-03: AppleLanguages for AppKit's own texts ----

TEST(I18nReview5, F_I18N_03_AppleLanguageTags) {
    EXPECT_EQ(appleLanguageTag(QStringLiteral("fr")), QStringLiteral("fr"));
    EXPECT_EQ(appleLanguageTag(QStringLiteral("pt_BR")), QStringLiteral("pt-BR"));
    EXPECT_EQ(appleLanguageTag(QStringLiteral("pt_PT")), QStringLiteral("pt-PT"));
    EXPECT_EQ(appleLanguageTag(QStringLiteral("zh_CN")), QStringLiteral("zh-Hans"));
    EXPECT_EQ(appleLanguageTag(QStringLiteral("zh_TW")), QStringLiteral("zh-Hant"));
    EXPECT_EQ(appleLanguageTag(QStringLiteral("nb")), QStringLiteral("nb"));
    // Every tag maps back to its UI language (per-app language of macOS).
    for (const QString& code : uiLanguageCodes())
        EXPECT_EQ(matchUiLanguage(appleLanguageTag(code)), code) << code.toStdString();
}

#ifdef Q_OS_MACOS
TEST(I18nReview5, F_I18N_03_ExplicitLanguageWritesAndRemovesAppleLanguages) {
    // A defaults domain of its own (never the test binary's or the app's), removed at the end.
    const QString domain = QStringLiteral("org.lumaflip.tests.applelanguages");
    const QString plist = QDir::homePath() + QStringLiteral("/Library/Preferences/") + domain + ".plist";
    QProcess reset;
    reset.start(QStringLiteral("/usr/bin/defaults"), {QStringLiteral("delete"), domain});
    ASSERT_TRUE(reset.waitForFinished(10000)); // a previous run that stopped early
    EXPECT_TRUE(appLanguagePreference(domain).isEmpty());
    ASSERT_TRUE(setAppKitLanguage(QStringLiteral("fr"), domain));
    // Written, but Lumaflip's own choice is not reported as a per-app language of macOS.
    EXPECT_TRUE(appLanguagePreference(domain).isEmpty());
    QProcess read;
    read.start(QStringLiteral("/usr/bin/defaults"),
               {QStringLiteral("read"), domain, QStringLiteral("AppleLanguages")});
    ASSERT_TRUE(read.waitForFinished(10000));
    EXPECT_TRUE(QString::fromUtf8(read.readAllStandardOutput()).contains(QStringLiteral("fr")));
    ASSERT_TRUE(setAppKitLanguage(QStringLiteral("zh_TW"), domain));
    read.start(QStringLiteral("/usr/bin/defaults"),
               {QStringLiteral("read"), domain, QStringLiteral("AppleLanguages")});
    ASSERT_TRUE(read.waitForFinished(10000));
    EXPECT_TRUE(QString::fromUtf8(read.readAllStandardOutput()).contains(QStringLiteral("zh-Hant")));
    // "system" removes it again.
    ASSERT_TRUE(setAppKitLanguage(QString::fromLatin1(kSystemLanguage), domain));
    read.start(QStringLiteral("/usr/bin/defaults"),
               {QStringLiteral("read"), domain, QStringLiteral("AppleLanguages")});
    ASSERT_TRUE(read.waitForFinished(10000));
    EXPECT_NE(read.exitCode(), 0) << "AppleLanguages still set";
    // A per-app language set in System Settings (not by Lumaflip) is reported and survives
    // "system".
    QProcess write;
    write.start(QStringLiteral("/usr/bin/defaults"),
                {QStringLiteral("write"), domain, QStringLiteral("AppleLanguages"), QStringLiteral("-array"),
                 QStringLiteral("ja")});
    ASSERT_TRUE(write.waitForFinished(10000));
    EXPECT_EQ(appLanguagePreference(domain), QStringList{QStringLiteral("ja")});
    ASSERT_TRUE(setAppKitLanguage(QString::fromLatin1(kSystemLanguage), domain));
    EXPECT_EQ(appLanguagePreference(domain), QStringList{QStringLiteral("ja")});
    // Clean up the test domain.
    QProcess del;
    del.start(QStringLiteral("/usr/bin/defaults"), {QStringLiteral("delete"), domain});
    ASSERT_TRUE(del.waitForFinished(10000));
    EXPECT_TRUE(appLanguagePreference(domain).isEmpty());
    QFile::remove(plist); // the empty file cfprefsd leaves behind
}
#endif

// ---- F-I18N-03: the input slot table survives a language switch on another thread ----

TEST(I18nReview5, F_I18N_03_InputSlotsAreCopiesSafeAcrossRebuilds) {
    const QVector<InputSlot> before = InputBindings::inputSlots();
    ASSERT_FALSE(before.isEmpty());
    std::atomic<bool> stop{false};
    std::thread switcher([&] {
        for (int i = 0; i < 50 && !stop; ++i) {
            bumpLanguageGeneration(); // like installTranslations() after replacing translators
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 50; ++i) {
        const QVector<InputSlot> now = InputBindings::inputSlots();
        ASSERT_EQ(now.size(), before.size());
        EXPECT_EQ(now.first().id, before.first().id);
        EXPECT_FALSE(now.first().label.isEmpty());
    }
    stop = true;
    switcher.join();
    // The copy taken before is untouched by the rebuilds.
    EXPECT_EQ(before.first().id, QStringLiteral("wheel"));
    EXPECT_FALSE(before.first().label.isEmpty());
}

// ---- F-I18N-01/05: the checker ----

TEST(I18nReview5, F_I18N_01_CheckerReceiverQualifiedCallsAndLabels) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("sample.cpp")), R"(
void a() { insert(QStringLiteral("Bild wird geladen")); }
void b() { m_set.insert(QStringLiteral("Bild wird geladen")); }
void c() { once("Fenster ist offen"); }
void d() { trace::once("Fenster ist offen"); }
void e() { label->setText(QStringLiteral("Zoom: %1").arg(z)); }
void f() { label->setText(tr("Zoom: %1").arg(z)); }
void g() { st.value(QStringLiteral("Fenster ist offen")); }
)"));
    int rc = 0;
    const QString out =
        runPython({QStringLiteral(LF_SRC_DIR "/tools/i18n_check.py"), QStringLiteral("--root"), dir.path(),
                   QStringLiteral("--allowlist"), dir.filePath(QStringLiteral("none.txt"))},
                  &rc);
    if (rc == -1)
        GTEST_SKIP() << "python3 not found";
    EXPECT_EQ(rc, 1) << out.toStdString();
    EXPECT_EQ(out.count(QStringLiteral("untranslated literal")), 3) << out.toStdString();
    for (const char* line : {"sample.cpp:2:", "sample.cpp:4:", "sample.cpp:6:"})
        EXPECT_TRUE(out.contains(QString::fromLatin1(line))) << line << "\n" << out.toStdString();
}

TEST(I18nReview5, F_I18N_05_CheckerPlaceholdersAndInfoPlistStrings) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(
        writeFile(dir.filePath(QStringLiteral("Info.plist.in")), R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>CFBundleLocalizations</key><array><string>de</string><string>en</string><string>fr</string></array>
  <key>NSDesktopFolderUsageDescription</key><string>Lumaflip liest den Ordner.</string>
  <key>CFBundleName</key><string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
  <key>UTImportedTypeDeclarations</key><array><dict>
    <key>UTTypeDescription</key><string>WBMP-Bild</string></dict></array>
</dict></plist>
)"));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("lproj/de.lproj/InfoPlist.strings")),
                          "\"NSDesktopFolderUsageDescription\" = \"Lumaflip liest den Ordner.\";\n"
                          "\"WBMP-Bild\" = \"WBMP-Bild\";\n"));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("lproj/en.lproj/InfoPlist.strings")),
                          "\"NSDesktopFolderUsageDescription\" = \"Lumaflip reads the folder.\";\n"
                          "\"Bild\" = \"Image\";\n"));
    // fr.lproj missing.
    const QString script = QStringLiteral(R"(
import pathlib, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, sys.argv[1])
import i18n_check as c
d = pathlib.Path(sys.argv[2])
c.INFO_PLIST = d / "Info.plist.in"
c.LPROJ_DIR = d / "lproj"
print("plist-rc", c.check_info_plist_strings())
ts = d / "x.ts"
def msg(xml):
    return ET.fromstring(xml)
ok = msg('<message><source>%1 von %2</source><translation>%2 of %1</translation></message>')
bad = msg('<message><source>%1 von %2</source><translation>%1 of %1</translation></message>')
plural_ok = msg('<message numerus="yes"><source>%n Bilder in %1</source><translation>'
                '<numerusform>one image in %1</numerusform><numerusform>%n images in %1</numerusform>'
                '</translation></message>')
plural_bad = msg('<message numerus="yes"><source>%n Bilder in %1</source><translation>'
                 '<numerusform>one image</numerusform><numerusform>many images in %1</numerusform>'
                 '</translation></message>')
for name, m in (("ok", ok), ("bad", bad), ("plural_ok", plural_ok), ("plural_bad", plural_bad)):
    print(name, len(c.check_placeholders(ts, m, ("ctx", m.findtext("source"), ""))))
)");
    int rc = 0;
    const QString out =
        runPython({QStringLiteral("-c"), script, QStringLiteral(LF_SRC_DIR "/tools"), dir.path()}, &rc);
    if (rc == -1)
        GTEST_SKIP() << "python3 not found";
    ASSERT_EQ(rc, 0) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("plist-rc 1"))) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("no fr.lproj"))) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("en.lproj/InfoPlist.strings: missing \"WBMP-Bild\"")))
        << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("en.lproj/InfoPlist.strings: stale entry \"Bild\"")))
        << out.toStdString();
    EXPECT_FALSE(out.contains(QStringLiteral("de.lproj/InfoPlist.strings"))) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("\nok 0"))) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("\nbad 1"))) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("\nplural_ok 0"))) << out.toStdString();
    // Both forms: the first lost %1, and no form keeps %n.
    EXPECT_TRUE(out.contains(QStringLiteral("\nplural_bad 2"))) << out.toStdString();
}
