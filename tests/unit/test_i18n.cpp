// SPDX-License-Identifier: GPL-3.0-or-later
// Multilingual UI (SPEC 4.16): untranslated texts (F-I18N-01), shipped catalogues
// (F-I18N-02), language resolution (F-I18N-03), locale formats (F-I18N-04), catalogue
// consistency (F-I18N-05) and the text checks of F-I18N-06 that need no window.

#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/Language.h"
#include "core/LocaleFormat.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTranslator>
#include <QXmlStreamReader>

#include <memory>

using namespace lf;

namespace {

// ---- .ts parsing ----------------------------------------------------------------------

struct TsMessage {
    QString context;
    QString source;
    QString comment; // disambiguation
    bool numerus = false;
    bool finished = false;
    QStringList forms; // translation (one entry) or the plural forms
};

QList<TsMessage> readTs(const QString& path) {
    QList<TsMessage> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader xml(&f);
    QString context;
    TsMessage cur;
    bool inMessage = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == u"name" && !inMessage) {
                context = xml.readElementText();
            } else if (name == u"message") {
                inMessage = true;
                cur = TsMessage();
                cur.context = context;
                cur.numerus = xml.attributes().value(u"numerus") == u"yes";
            } else if (name == u"source") {
                cur.source = xml.readElementText();
            } else if (name == u"comment") {
                cur.comment = xml.readElementText();
            } else if (name == u"translation") {
                const auto type = xml.attributes().value(u"type");
                cur.finished = type.isEmpty();
                if (type == u"obsolete" || type == u"vanished") {
                    cur.finished = false;
                    cur.context.clear(); // not part of the catalogue any more: skipped below
                }
                if (!cur.numerus)
                    cur.forms << xml.readElementText();
            } else if (name == u"numerusform") {
                cur.forms << xml.readElementText();
            }
        } else if (xml.isEndElement() && xml.name() == u"message") {
            inMessage = false;
            if (!cur.context.isEmpty())
                out << cur;
        }
    }
    return out;
}

QStringList languages() {
    return QString::fromLatin1(LF_UI_LANGUAGES).split(QLatin1Char(','));
}

QStringList translatedLanguages() {
    QStringList l = languages();
    l.removeAll(QStringLiteral("de")); // source language
    return l;
}

QString tsPath(const QString& lang) {
    return QStringLiteral(LF_TS_DIR "/lumaflip_%1.ts").arg(lang);
}

int pluralForms(const QString& lang) {
    if (lang == u"cs" || lang == u"pl")
        return 3;
    if (lang == u"ja" || lang == u"ko" || lang.startsWith(u"zh"))
        return 1;
    return 2;
}

QStringList placeholders(const QString& s) {
    static const QRegularExpression re(QStringLiteral("%L?(\\d+|n)"));
    QStringList out;
    for (auto it = re.globalMatch(s); it.hasNext();)
        out << it.next().captured(1);
    out.sort();
    out.removeDuplicates();
    return out;
}

QStringList htmlTags(const QString& s) {
    static const QRegularExpression re(QStringLiteral("</?([a-zA-Z]+)[^>]*>"));
    QStringList out;
    for (auto it = re.globalMatch(s); it.hasNext();)
        out << it.next().captured(0).section(QLatin1Char(' '), 0, 0);
    return out;
}

bool requireTranslations() {
    return qEnvironmentVariableIntValue("LF_REQUIRE_TRANSLATIONS") == 1;
}

#define LF_SKIP_WITHOUT_TRANSLATIONS()                                                                       \
    do {                                                                                                     \
        if (!kHaveTranslations && !requireTranslations())                                                    \
            GTEST_SKIP() << "built without Qt LinguistTools (no .qm files)";                                 \
    } while (false)

#ifdef LF_HAVE_TRANSLATIONS
constexpr bool kHaveTranslations = true;
#else
constexpr bool kHaveTranslations = false;
#endif

// Installs a catalogue for the lifetime of the object (QCoreApplication needed).
class ScopedCatalog {
public:
    explicit ScopedCatalog(const QString& lang) {
        test::ensureQtApp();
        m_ok = m_t.load(QStringLiteral("lumaflip_") + lang, QStringLiteral(LF_QM_DIR));
        if (m_ok)
            QCoreApplication::installTranslator(&m_t);
    }
    ~ScopedCatalog() {
        if (m_ok)
            QCoreApplication::removeTranslator(&m_t);
    }
    bool ok() const { return m_ok; }

private:
    QTranslator m_t;
    bool m_ok = false;
};

} // namespace

// ---- F-I18N-01: every user-visible text goes through tr() ---------------------------------

TEST(I18n, F_I18N_01_NoUntranslatedLiteralsInSources) {
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        GTEST_SKIP() << "python3 not found";
    QProcess p;
    p.start(python, {QStringLiteral(LF_SRC_DIR "/tools/i18n_check.py")});
    ASSERT_TRUE(p.waitForFinished(60000));
    EXPECT_EQ(p.exitCode(), 0) << p.readAllStandardOutput().toStdString()
                               << p.readAllStandardError().toStdString();
}

TEST(I18n, F_I18N_01_CheckerFindsUnwrappedTexts) {
    // The checker itself: a literal sentence outside tr() is reported, wrapped ones and
    // logging/keys are not.
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        GTEST_SKIP() << "python3 not found";
    const QString dir =
        QDir::tempPath() + QStringLiteral("/lf_i18n_check_%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/sample.cpp"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"(
void a() { label->setText(QStringLiteral("Bild wird geladen")); }
void b() { label->setText(tr("Bild wird geladen")); }
void c() { qCWarning(lc) << "Shader not found:" << name; }
void d() { settings.value(QStringLiteral("window/geometry")); }
void e() { box->setText(QCoreApplication::translate("lf::X", "Datei löschen?")); }
)");
    f.close();
    QProcess p;
    p.start(python, {QStringLiteral(LF_SRC_DIR "/tools/i18n_check.py"), QStringLiteral("--root"), dir,
                     QStringLiteral("--allowlist"), dir + QStringLiteral("/none.txt")});
    ASSERT_TRUE(p.waitForFinished(60000));
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    EXPECT_EQ(p.exitCode(), 1) << out.toStdString();
    EXPECT_EQ(out.count(QStringLiteral("untranslated")), 1) << out.toStdString();
    EXPECT_TRUE(out.contains(QStringLiteral("sample.cpp:2"))) << out.toStdString();
    QDir(dir).removeRecursively();
}

// ---- F-I18N-02: shipped languages, catalogues load and are complete ----------------------

TEST(I18n, F_I18N_02_ShippedLanguages) {
    const QStringList expected = {"de", "en", "fr", "es", "it", "pt_BR", "pt_PT", "nl",    "pl",
                                  "sv", "da", "nb", "fi", "cs", "ja",    "zh_CN", "zh_TW", "ko"};
    EXPECT_EQ(uiLanguageCodes(), expected);
    EXPECT_EQ(languages(), expected); // CMake list (src/CMakeLists.txt) matches Language.cpp
    for (const QString& code : expected)
        EXPECT_FALSE(nativeLanguageName(code).isEmpty()) << code.toStdString();
    EXPECT_EQ(nativeLanguageName(QStringLiteral("fr")), QStringLiteral("Français"));
    EXPECT_EQ(nativeLanguageName(QStringLiteral("ja")), QStringLiteral("日本語"));
    EXPECT_TRUE(nativeLanguageName(QStringLiteral("xx")).isEmpty());
}

TEST(I18n, F_I18N_02_AllCataloguesExistAndLoad) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    for (const QString& lang : languages()) {
        EXPECT_TRUE(QFile::exists(tsPath(lang))) << lang.toStdString();
        QTranslator t;
        EXPECT_TRUE(t.load(QStringLiteral("lumaflip_") + lang, QStringLiteral(LF_QM_DIR)))
            << "lumaflip_" << lang.toStdString() << ".qm in " << LF_QM_DIR;
        EXPECT_FALSE(t.isEmpty()) << lang.toStdString();
    }
}

TEST(I18n, F_I18N_02_CataloguesCoverAtLeast99PercentOfSourceMessages) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    const QList<TsMessage> reference = readTs(tsPath(QStringLiteral("en")));
    ASSERT_GT(reference.size(), 500);
    for (const QString& lang : translatedLanguages()) {
        QTranslator t;
        ASSERT_TRUE(t.load(QStringLiteral("lumaflip_") + lang, QStringLiteral(LF_QM_DIR)));
        int translated = 0;
        QStringList missing;
        for (const TsMessage& m : reference) {
            const QByteArray ctx = m.context.toUtf8(), src = m.source.toUtf8(), cmt = m.comment.toUtf8();
            const QString text =
                t.translate(ctx.constData(), src.constData(), m.comment.isEmpty() ? nullptr : cmt.constData(),
                            m.numerus ? 2 : -1);
            if (!text.isEmpty())
                ++translated;
            else if (missing.size() < 5)
                missing << m.context + QStringLiteral(": ") + m.source;
        }
        const double coverage = double(translated) / double(reference.size());
        EXPECT_GE(coverage, 0.99) << lang.toStdString() << ": " << translated << "/" << reference.size()
                                  << ", e.g. " << missing.join(QStringLiteral(" | ")).toStdString();
    }
}

TEST(I18n, F_I18N_02_GermanCatalogueHoldsThePluralForms) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    ScopedCatalog de(QStringLiteral("de"));
    ASSERT_TRUE(de.ok());
    EXPECT_EQ(QCoreApplication::translate("lf::ViewerWindow", "Intervall: %n Sekunde(n)", nullptr, 1),
              QStringLiteral("Intervall: 1 Sekunde"));
    EXPECT_EQ(QCoreApplication::translate("lf::ViewerWindow", "Intervall: %n Sekunde(n)", nullptr, 5),
              QStringLiteral("Intervall: 5 Sekunden"));
}

TEST(I18n, F_I18N_02_TranslatedTextsAreUsed) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    ScopedCatalog en(QStringLiteral("en"));
    ASSERT_TRUE(en.ok());
    EXPECT_EQ(QCoreApplication::translate("lf::MainMenu", "Ablage"), QStringLiteral("File"));
    EXPECT_EQ(QCoreApplication::translate("lf::MainMenu", "Hilfe"), QStringLiteral("Help"));
    // Disambiguation: Redo vs. the slideshow's Repeat.
    EXPECT_NE(QCoreApplication::translate("lf::MainMenu", "Wiederholen"),
              QCoreApplication::translate("lf::MainMenu", "Wiederholen",
                                          "slideshow option: start again after the last image"));
    EXPECT_EQ(metadataLabel(QStringLiteral("gps.latitude")), QStringLiteral("Latitude"));
}

// ---- F-I18N-03: language resolution order --------------------------------------------------

TEST(I18n, F_I18N_03_ExplicitSettingWins) {
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("fr"), {QStringLiteral("ja")}, {QStringLiteral("de-DE")}),
              QStringLiteral("fr"));
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("de"), {}, {QStringLiteral("en-US")}), QStringLiteral("de"));
    // Not a shipped language (an old or edited settings file): falls through.
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("xx"), {}, {QStringLiteral("it-IT")}), QStringLiteral("it"));
}

TEST(I18n, F_I18N_03_PerAppLanguageBeforeSystemLanguages) {
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("system"), {QStringLiteral("ja")},
                                {QStringLiteral("de-DE"), QStringLiteral("en-US")}),
              QStringLiteral("ja"));
    // Per-app language not shipped: the system list decides.
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("system"), {QStringLiteral("ru")},
                                {QStringLiteral("de-DE"), QStringLiteral("en-US")}),
              QStringLiteral("de"));
}

TEST(I18n, F_I18N_03_SystemLanguagesInOrderThenEnglish) {
    EXPECT_EQ(resolveUiLanguage(QStringLiteral("system"), {},
                                {QStringLiteral("ru-RU"), QStringLiteral("sv-SE"), QStringLiteral("de-DE")}),
              QStringLiteral("sv"));
    EXPECT_EQ(
        resolveUiLanguage(QStringLiteral("system"), {}, {QStringLiteral("ru-RU"), QStringLiteral("tr")}),
        QStringLiteral("en"));
    EXPECT_EQ(resolveUiLanguage(QString(), {}, {}), QStringLiteral("en"));
}

TEST(I18n, F_I18N_03_MatchesRegionalAndScriptVariants) {
    EXPECT_EQ(matchUiLanguage(QStringLiteral("de-CH")), QStringLiteral("de"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("en_GB")), QStringLiteral("en"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("fr-CA")), QStringLiteral("fr"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("es-419")), QStringLiteral("es"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("pt-BR")), QStringLiteral("pt_BR"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("pt")), QStringLiteral("pt_BR"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("pt-PT")), QStringLiteral("pt_PT"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("pt-AO")), QStringLiteral("pt_PT"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("zh-Hans-CN")), QStringLiteral("zh_CN"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("zh-Hant-TW")), QStringLiteral("zh_TW"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("zh-HK")), QStringLiteral("zh_TW"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("zh-Hant-HK")), QStringLiteral("zh_TW"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("zh")), QStringLiteral("zh_CN"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("nb-NO")), QStringLiteral("nb"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("no")), QStringLiteral("nb"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("nn-NO")), QStringLiteral("nb"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("ja-JP")), QStringLiteral("ja"));
    EXPECT_EQ(matchUiLanguage(QStringLiteral("de_DE.UTF-8")), QStringLiteral("de"));
    EXPECT_TRUE(matchUiLanguage(QStringLiteral("ru-RU")).isEmpty());
    EXPECT_TRUE(matchUiLanguage(QString()).isEmpty());
    // Only what is available counts.
    EXPECT_TRUE(
        matchUiLanguage(QStringLiteral("fr"), {QStringLiteral("de"), QStringLiteral("en")}).isEmpty());
}

// ---- F-I18N-04: locale formats -------------------------------------------------------------

TEST(I18n, F_I18N_04_SizesFollowTheLocale) {
    const QLocale de(QLocale::German, QLocale::Germany);
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    EXPECT_EQ(formatByteSize(2'812'345, de), QStringLiteral("2,8 MB"));
    EXPECT_EQ(formatByteSize(2'812'345, en), QStringLiteral("2.8 MB"));
    EXPECT_EQ(formatByteSize(680, de), QStringLiteral("680 Byte"));
    EXPECT_EQ(formatByteSize(1'500'000'000, de), QStringLiteral("1,5 GB"));
    EXPECT_EQ(formatByteSizeExact(2'812'345, de), QStringLiteral("2,8 MB (2.812.345 Byte)"));
    EXPECT_EQ(formatByteSizeExact(2'812'345, en), QStringLiteral("2.8 MB (2,812,345 Byte)"));
    // German is the default until the app sets the UI language's locale.
    EXPECT_EQ(formatByteSize(2'812'345), QStringLiteral("2,8 MB"));
}

TEST(I18n, F_I18N_04_TranslatedUnits) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    {
        ScopedCatalog en(QStringLiteral("en"));
        ASSERT_TRUE(en.ok());
        const QLocale us(QLocale::English, QLocale::UnitedStates);
        EXPECT_EQ(formatByteSize(1, us), QStringLiteral("1 byte"));
        EXPECT_EQ(formatByteSize(680, us), QStringLiteral("680 bytes"));
        EXPECT_EQ(formatByteSize(2'812'345, us), QStringLiteral("2.8 MB"));
        EXPECT_EQ(formatLongitude(11.5755, us), QStringLiteral("11.5755° E"));
    }
    {
        ScopedCatalog fr(QStringLiteral("fr"));
        ASSERT_TRUE(fr.ok());
        EXPECT_EQ(formatByteSize(2'812'345, QLocale(QLocale::French, QLocale::France)),
                  QStringLiteral("2,8 Mo"));
    }
}

TEST(I18n, F_I18N_04_DatesFollowTheLocale) {
    const QDateTime dt(QDate(2026, 10, 10), QTime(14, 3, 9), QTimeZone::UTC);
    EXPECT_EQ(formatDateTime(dt, QLocale(QLocale::German, QLocale::Germany)),
              QStringLiteral("10.10.2026 14:03:09"));
    const QString us = formatDateTime(dt, QLocale(QLocale::English, QLocale::UnitedStates));
    EXPECT_TRUE(us.startsWith(QStringLiteral("10/10/2026"))) << us.toStdString();
    EXPECT_TRUE(us.contains(QStringLiteral("2:03:09"))) << us.toStdString();
    const QString ja = formatDateTime(dt, QLocale(QLocale::Japanese, QLocale::Japan));
    EXPECT_TRUE(ja.startsWith(QStringLiteral("2026/10/10"))) << ja.toStdString();
    EXPECT_TRUE(ja.contains(QStringLiteral("14:03:09"))) << ja.toStdString();
    EXPECT_TRUE(formatDateTime(QDateTime(), QLocale::c()).isEmpty());
}

TEST(I18n, F_I18N_04_ExposureAndGpsFollowTheLocale) {
    const QLocale de(QLocale::German, QLocale::Germany);
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    EXPECT_EQ(formatExposureTime(1.0 / 250, de), QStringLiteral("1/250 s"));
    EXPECT_EQ(formatExposureTime(2.5, de), QStringLiteral("2,5 s"));
    EXPECT_EQ(formatExposureTime(2.5, en), QStringLiteral("2.5 s"));
    EXPECT_EQ(formatExposureTime(1.0 / 3, en), QStringLiteral("1/3 s"));
    EXPECT_EQ(formatAperture(2.8, de), QStringLiteral("f/2,8"));
    EXPECT_EQ(formatAperture(8.0, en), QStringLiteral("f/8"));
    EXPECT_EQ(formatExposureBias(0.7, de), QStringLiteral("+0,7 EV"));
    EXPECT_EQ(formatExposureBias(-1.3, en), QStringLiteral("-1.3 EV"));
    EXPECT_EQ(formatFocalLength(35, 52, de), QStringLiteral("35 mm (KB: 52 mm)"));
    EXPECT_EQ(formatFocalLength(50, 50, en), QStringLiteral("50 mm"));
    EXPECT_EQ(formatLatitude(48.137154, de), QStringLiteral("48,137154° N"));
    EXPECT_EQ(formatLatitude(-33.8688, en), QStringLiteral("33.8688° S"));
    EXPECT_EQ(formatLongitude(11.5755, de), QStringLiteral("11,5755° O"));
    EXPECT_EQ(formatLongitude(-0.1276, en), QStringLiteral("0.1276° W"));
    EXPECT_EQ(formatAltitude(519.4, de), QStringLiteral("519,4 m"));
    EXPECT_EQ(formatDecimal(2.0, 1, de), QStringLiteral("2"));
}

TEST(I18n, F_I18N_04_FormatLocaleForUiLanguage) {
    // Same language as the region settings: the user's own locale (e.g. Swiss formats).
    const QLocale ch(QLocale::German, QLocale::Switzerland);
    EXPECT_EQ(formatLocaleFor(QStringLiteral("de"), ch), ch);
    // Another UI language: that language in the user's region if it exists.
    const QLocale enDe = formatLocaleFor(QStringLiteral("en"), QLocale(QLocale::German, QLocale::Germany));
    EXPECT_EQ(enDe.language(), QLocale::English);
    EXPECT_EQ(enDe.territory(), QLocale::Germany);
    EXPECT_EQ(formatByteSize(2'812'345, enDe), QStringLiteral("2,8 MB"));
    // ... else the language's default.
    const QLocale ja = formatLocaleFor(QStringLiteral("ja"), QLocale(QLocale::German, QLocale::Germany));
    EXPECT_EQ(ja.language(), QLocale::Japanese);
    EXPECT_EQ(
        formatLocaleFor(QStringLiteral("fr"), QLocale(QLocale::English, QLocale::UnitedStates)).language(),
        QLocale::French);
    // Variants keep their region: European Portuguese on a Brazilian system.
    const QLocale pt =
        formatLocaleFor(QStringLiteral("pt_PT"), QLocale(QLocale::Portuguese, QLocale::Brazil));
    EXPECT_EQ(pt.territory(), QLocale::Portugal);
    const QLocale tw = formatLocaleFor(QStringLiteral("zh_TW"), QLocale(QLocale::Chinese, QLocale::China));
    EXPECT_EQ(tw.script(), QLocale::TraditionalChineseScript);
}

TEST(I18n, F_I18N_04_FormatLocaleIsGlobalAndSettable) {
    const QLocale before = formatLocale();
    EXPECT_EQ(before.language(), QLocale::German);
    const int generation = languageGeneration();
    setFormatLocale(QLocale(QLocale::English, QLocale::UnitedStates));
    bumpLanguageGeneration();
    EXPECT_EQ(formatByteSize(2'812'345), QStringLiteral("2.8 MB"));
    EXPECT_GT(languageGeneration(), generation);
    setFormatLocale(before);
}

// ---- F-I18N-05: catalogues are consistent -------------------------------------------------

TEST(I18n, F_I18N_05_PlaceholdersMatchTheSource) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    for (const QString& lang : languages()) {
        const QList<TsMessage> messages = readTs(tsPath(lang));
        ASSERT_FALSE(messages.isEmpty()) << lang.toStdString();
        for (const TsMessage& m : messages) {
            if (!m.finished)
                continue;
            for (const QString& form : m.forms) {
                EXPECT_EQ(placeholders(form), placeholders(m.source))
                    << lang.toStdString() << " " << m.context.toStdString() << ": " << m.source.toStdString()
                    << " -> " << form.toStdString();
                EXPECT_EQ(htmlTags(form), htmlTags(m.source))
                    << lang.toStdString() << ": " << form.toStdString();
                EXPECT_EQ(form.count(QLatin1Char('\n')), m.source.count(QLatin1Char('\n')))
                    << lang.toStdString() << ": " << form.toStdString();
            }
        }
    }
}

TEST(I18n, F_I18N_05_PluralFormsPerLanguage) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    for (const QString& lang : languages()) {
        int plurals = 0;
        for (const TsMessage& m : readTs(tsPath(lang))) {
            if (!m.numerus)
                continue;
            ++plurals;
            EXPECT_EQ(m.forms.size(), pluralForms(lang))
                << lang.toStdString() << ": " << m.source.toStdString();
            for (const QString& form : m.forms)
                EXPECT_EQ(form.contains(QStringLiteral("%n")), m.source.contains(QStringLiteral("%n")))
                    << lang.toStdString() << ": " << form.toStdString();
        }
        EXPECT_GE(plurals, 3) << lang.toStdString(); // %n Byte, Sekunde(n), Aktion(en)
    }
}

TEST(I18n, F_I18N_05_PluralsUseTheLanguageRules) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    ScopedCatalog pl(QStringLiteral("pl"));
    ASSERT_TRUE(pl.ok());
    const QString one = formatByteSize(1, QLocale(QLocale::Polish));
    const QString few = formatByteSize(3, QLocale(QLocale::Polish));
    const QString many = formatByteSize(5, QLocale(QLocale::Polish));
    EXPECT_NE(one.section(QLatin1Char(' '), 1), few.section(QLatin1Char(' '), 1));
    EXPECT_NE(few.section(QLatin1Char(' '), 1), many.section(QLatin1Char(' '), 1));
}

// ---- F-I18N-06 (unit part): texts fit their place ------------------------------------------

TEST(I18n, F_I18N_06_NoEmptyTranslations) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    for (const QString& lang : translatedLanguages())
        for (const TsMessage& m : readTs(tsPath(lang)))
            for (const QString& form : m.forms)
                if (m.finished)
                    EXPECT_FALSE(form.trimmed().isEmpty())
                        << lang.toStdString() << " " << m.context.toStdString() << ": "
                        << m.source.toStdString();
}

TEST(I18n, F_I18N_06_ShortcutTextsAndSpacingKept) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    // Shortcut hints (⌘I, ⇧⌘F …), file dialog filters and the leading space of spin box
    // suffixes (" ms") must survive translation, or labels read wrong next to the value.
    for (const QString& lang : translatedLanguages()) {
        for (const TsMessage& m : readTs(tsPath(lang))) {
            if (!m.finished)
                continue;
            for (const QString& form : m.forms) {
                for (const QChar sym : {u'⌘', u'⌥', u'⇧', u'⌫'})
                    EXPECT_EQ(form.contains(sym), m.source.contains(sym))
                        << lang.toStdString() << ": " << m.source.toStdString() << " -> "
                        << form.toStdString();
                EXPECT_EQ(form.startsWith(QLatin1Char(' ')), m.source.startsWith(QLatin1Char(' ')))
                    << lang.toStdString() << ": '" << form.toStdString() << "'";
                EXPECT_EQ(form.contains(QStringLiteral(";;")), m.source.contains(QStringLiteral(";;")))
                    << lang.toStdString() << ": " << form.toStdString();
            }
        }
    }
}

TEST(I18n, F_I18N_06_MenuTextsStayCompact) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    // Menus and toolbar sections have no room for prose: a translation may be longer than
    // the (already long) German text, but not by an unreasonable factor.
    const QStringList compact = {QStringLiteral("lf::MainMenu"), QStringLiteral("lf::MenuExtras"),
                                 QStringLiteral("SortSpec")};
    for (const QString& lang : translatedLanguages())
        for (const TsMessage& m : readTs(tsPath(lang)))
            if (m.finished && compact.contains(m.context))
                for (const QString& form : m.forms)
                    EXPECT_LE(form.size(), std::max<qsizetype>(2 * m.source.size() + 12, 32))
                        << lang.toStdString() << ": " << m.source.toStdString() << " -> "
                        << form.toStdString();
}

TEST(I18n, F_I18N_06_EllipsisFollowsTheLanguageStyle) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    // Apple's per-language style: German puts a space before "…" ("Öffnen …"); English,
    // French, Japanese … write it right after the word ("Open…", "Ouvrir…", "開く…").
    // Never three dots.
    static const QRegularExpression spaceBefore(QStringLiteral("\\S[\\s\\x{00A0}\\x{202F}]+…"));
    static const QRegularExpression glued(QStringLiteral("[\\p{L}\\p{N}]…"));
    for (const TsMessage& m : readTs(tsPath(QStringLiteral("en")))) {
        EXPECT_FALSE(m.source.contains(QStringLiteral("..."))) << m.source.toStdString();
        EXPECT_FALSE(glued.match(m.source).hasMatch())
            << "German style is \"Wort …\": " << m.source.toStdString();
    }
    for (const QString& lang : translatedLanguages()) {
        for (const TsMessage& m : readTs(tsPath(lang))) {
            if (!m.finished)
                continue;
            for (const QString& form : m.forms) {
                EXPECT_FALSE(form.contains(QStringLiteral("...")))
                    << lang.toStdString() << ": " << form.toStdString();
                EXPECT_FALSE(spaceBefore.match(form).hasMatch())
                    << lang.toStdString() << ": no space before \"…\": " << form.toStdString();
                EXPECT_EQ(form.contains(u'…'), m.source.contains(u'…'))
                    << lang.toStdString() << ": " << m.source.toStdString() << " -> " << form.toStdString();
            }
        }
    }
}

TEST(I18n, F_I18N_04_GpsDirectionOrderPerLanguage) {
    LF_SKIP_WITHOUT_TRANSLATIONS();
    // East Asian languages put the direction word first ("北緯 35° 39′ 29.4″").
    const auto dms = [](const char* lang) {
        ScopedCatalog c(QString::fromLatin1(lang));
        EXPECT_TRUE(c.ok()) << lang;
        return QCoreApplication::translate("lf::GeoLocation", "%1° %2′ %3″ %4")
            .arg(35)
            .arg(39)
            .arg(QStringLiteral("29.4"), QCoreApplication::translate("lf::Format", "N", "north"));
    };
    EXPECT_EQ(dms("ja"), QStringLiteral("北緯 35° 39′ 29.4″"));
    EXPECT_EQ(dms("zh_CN"), QStringLiteral("北纬 35°39′29.4″"));
    EXPECT_EQ(dms("zh_TW"), QStringLiteral("北緯 35°39′29.4″"));
    EXPECT_EQ(dms("ko"), QStringLiteral("북위 35° 39′ 29.4″"));
    EXPECT_EQ(dms("en"), QStringLiteral("35° 39′ 29.4″ N"));
}
