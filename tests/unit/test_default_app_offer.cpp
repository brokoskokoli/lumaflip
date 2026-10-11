// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-09: decision logic of the "not the default app" bar (core/DefaultAppOffer.h):
// skip conditions, the answers remembered per type, the global switch, the installed-app
// helpers. The bar itself: tst_default_app_offer.

#include "core/DefaultAppOffer.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace lf;
using defaultapp::Decision;
using State = DefaultViewerStatus::State;

namespace {

void ensureOfferTestApp() {
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lf_unit_tests";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv); // lives for the whole test process
}

class DefaultAppOfferTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureOfferTestApp();
        static QTemporaryDir dir;
        static int counter = 0;
        Settings::useTestingBackend(dir.filePath(QStringLiteral("offer-%1.ini").arg(++counter)));
        m_hadEnv = qEnvironmentVariableIsSet(defaultapp::kDisableEnv);
        m_env = qgetenv(defaultapp::kDisableEnv);
        qunsetenv(defaultapp::kDisableEnv);
    }
    void TearDown() override {
        if (m_hadEnv)
            qputenv(defaultapp::kDisableEnv, m_env);
        else
            qunsetenv(defaultapp::kDisableEnv);
    }
    static DefaultViewerStatus notDefault(const QString& type = QStringLiteral("public.jpeg")) {
        DefaultViewerStatus s;
        s.state = State::NotDefault;
        s.type = type;
        s.typeName = QStringLiteral("JPEG-Bild");
        return s;
    }

private:
    bool m_hadEnv = false;
    QByteArray m_env;
};

} // namespace

TEST_F(DefaultAppOfferTest, F_MAC_09_ShowsForAnotherAppsTypeWhenInstalled) {
    const defaultapp::Situation s;
    EXPECT_EQ(defaultapp::beforeQuery(s), Decision::Show);
    EXPECT_EQ(defaultapp::afterQuery(s, true, notDefault(), {}), Decision::Show);
}

TEST_F(DefaultAppOfferTest, F_MAC_09_SkipConditions) {
    defaultapp::Situation s;
    // Not the installed app (build tree, disk image, outside /Applications).
    EXPECT_EQ(defaultapp::afterQuery(s, false, notDefault(), {}), Decision::NotInstalled);
    // The query failed or the type is not one of the app's types: never shown.
    DefaultViewerStatus unknown = notDefault();
    unknown.state = State::Unknown;
    EXPECT_EQ(defaultapp::afterQuery(s, true, unknown, {}), Decision::Unknown);
    DefaultViewerStatus noType = notDefault(QString());
    EXPECT_EQ(defaultapp::afterQuery(s, true, noType, {}), Decision::Unknown);
    EXPECT_EQ(defaultapp::afterQuery(s, true, DefaultViewerStatus{}, {}), Decision::Unknown);
    // Already the default.
    DefaultViewerStatus isDefault = notDefault();
    isDefault.state = State::IsDefault;
    EXPECT_EQ(defaultapp::afterQuery(s, true, isDefault, {}), Decision::IsDefault);

    // Archive members and PDF pages, full screen/slideshow, the test switch: not even asked.
    s.containerMember = true;
    EXPECT_EQ(defaultapp::beforeQuery(s), Decision::ContainerMember);
    EXPECT_EQ(defaultapp::afterQuery(s, true, notDefault(), {}), Decision::ContainerMember);
    s = {};
    s.fullScreen = true;
    EXPECT_EQ(defaultapp::beforeQuery(s), Decision::FullScreen);
    EXPECT_EQ(defaultapp::afterQuery(s, true, notDefault(), {}), Decision::FullScreen);
    s = {};
    s.environmentOff = true;
    EXPECT_EQ(defaultapp::beforeQuery(s), Decision::Environment);
    s = {};
    s.enabled = false;
    EXPECT_EQ(defaultapp::beforeQuery(s), Decision::Disabled);
    EXPECT_EQ(defaultapp::afterQuery(s, true, notDefault(), {}), Decision::Disabled);
}

TEST_F(DefaultAppOfferTest, F_MAC_09_SituationFromPathSettingsAndEnvironment) {
    const QString archive = QDir::temp().filePath(QStringLiteral("Band 1.cbz"));
    EXPECT_TRUE(
        defaultapp::situationFor(vpath::join(archive, QStringLiteral("001.jpg")), false).containerMember);
    EXPECT_TRUE(
        defaultapp::situationFor(vpath::join(QStringLiteral("/tmp/a.pdf"), vpath::pdfPageMember(2)), false)
            .containerMember);
    const defaultapp::Situation plain = defaultapp::situationFor(QStringLiteral("/tmp/a.jpg"), false);
    EXPECT_FALSE(plain.containerMember);
    EXPECT_FALSE(plain.fullScreen);
    EXPECT_TRUE(plain.enabled); // default on (F-SET-04)
    EXPECT_FALSE(plain.environmentOff);
    EXPECT_TRUE(defaultapp::situationFor(QStringLiteral("/tmp/a.jpg"), true).fullScreen);

    qputenv(defaultapp::kDisableEnv, "1");
    EXPECT_TRUE(defaultapp::disabledByEnvironment());
    EXPECT_EQ(defaultapp::beforeQuery(defaultapp::situationFor(QStringLiteral("/tmp/a.jpg"), false)),
              Decision::Environment);
    qunsetenv(defaultapp::kDisableEnv);
    EXPECT_FALSE(defaultapp::disabledByEnvironment());
}

TEST_F(DefaultAppOfferTest, F_MAC_09_AskedTypesAreRememberedPerType) {
    EXPECT_TRUE(defaultapp::askedTypes().isEmpty());
    defaultapp::markAsked(QStringLiteral("public.jpeg"));
    defaultapp::markAsked(QStringLiteral("public.jpeg")); // once
    defaultapp::markAsked(QString());                     // ignored
    EXPECT_EQ(defaultapp::askedTypes(), QStringList{QStringLiteral("public.jpeg")});
    const defaultapp::Situation s;
    EXPECT_EQ(defaultapp::afterQuery(s, true, notDefault(), defaultapp::askedTypes()),
              Decision::AlreadyAsked);
    // Another type is still offered.
    EXPECT_EQ(
        defaultapp::afterQuery(s, true, notDefault(QStringLiteral("public.png")), defaultapp::askedTypes()),
        Decision::Show);
    defaultapp::markAsked(QStringLiteral("public.png"));
    EXPECT_EQ(defaultapp::askedTypes(),
              (QStringList{QStringLiteral("public.jpeg"), QStringLiteral("public.png")}));
    // Persistent: stored in the settings.
    EXPECT_EQ(Settings::instance().value(QString::fromLatin1(defaultapp::kAskedTypesKey)).toStringList(),
              defaultapp::askedTypes());
}

TEST_F(DefaultAppOfferTest, F_MAC_09_GlobalSwitch) {
    EXPECT_TRUE(defaultapp::isEnabled());
    EXPECT_TRUE(Settings::instance().defaultValue(QString::fromLatin1(key::OfferDefaultApp)).toBool());
    defaultapp::setEnabled(false); // "Nicht mehr fragen"
    EXPECT_FALSE(defaultapp::isEnabled());
    EXPECT_FALSE(Settings::instance().value(QString::fromLatin1(key::OfferDefaultApp)).toBool());
    EXPECT_EQ(defaultapp::beforeQuery(defaultapp::situationFor(QStringLiteral("/tmp/a.jpg"), false)),
              Decision::Disabled);
    defaultapp::setEnabled(true); // the preference checkbox
    EXPECT_EQ(defaultapp::beforeQuery(defaultapp::situationFor(QStringLiteral("/tmp/a.jpg"), false)),
              Decision::Show);
}

TEST_F(DefaultAppOfferTest, F_MAC_09_InstalledLocation) {
    const QStringList apps{QStringLiteral("/Applications"), QStringLiteral("/Users/u/Applications")};
    EXPECT_TRUE(defaultapp::isInsideAny(QStringLiteral("/Applications/Lumaflip.app"), apps));
    EXPECT_TRUE(defaultapp::isInsideAny(QStringLiteral("/Applications/Fotos/Lumaflip.app"), apps));
    EXPECT_TRUE(defaultapp::isInsideAny(QStringLiteral("/Users/u/Applications/Lumaflip.app"), apps));
    EXPECT_FALSE(defaultapp::isInsideAny(QStringLiteral("/Applications"), apps));
    EXPECT_FALSE(defaultapp::isInsideAny(QStringLiteral("/ApplicationsOld/Lumaflip.app"), apps));
    EXPECT_FALSE(defaultapp::isInsideAny(QStringLiteral("/Volumes/Lumaflip/Lumaflip.app"), apps));
    EXPECT_FALSE(
        defaultapp::isInsideAny(QStringLiteral("/Users/u/src/lumaflip/build/check/src/Lumaflip.app"), apps));
    EXPECT_FALSE(defaultapp::isInsideAny(
        QStringLiteral("/private/var/folders/x/AppTranslocation/1234/d/Lumaflip.app"), apps));
    EXPECT_FALSE(defaultapp::isInsideAny(QString(), apps));
    EXPECT_TRUE(defaultapp::isInsideAny(QStringLiteral("/Applications/../Applications/Lumaflip.app"), apps));
}

TEST_F(DefaultAppOfferTest, F_MAC_09_BuildTreeIsNotInstalled) {
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("build/src/Lumaflip.app/Contents/MacOS")));
    const QString exeDir = dir.filePath(QStringLiteral("build/src/Lumaflip.app/Contents/MacOS"));
    EXPECT_FALSE(defaultapp::isInBuildTree(exeDir));
    QFile cache(dir.filePath(QStringLiteral("build/CMakeCache.txt")));
    ASSERT_TRUE(cache.open(QIODevice::WriteOnly));
    cache.close();
    EXPECT_TRUE(defaultapp::isInBuildTree(exeDir));
    EXPECT_FALSE(defaultapp::isInBuildTree(exeDir, 2)); // too far up
    // The unit test binary itself runs from the build tree.
    EXPECT_TRUE(defaultapp::isInBuildTree(QCoreApplication::applicationDirPath()));
}
