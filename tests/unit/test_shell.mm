// SPDX-License-Identifier: GPL-3.0-or-later
// Shell integration (F-MAC-04 type list, F-MAC-09 default app query). makeDefaultViewer
// and reveal are not called here because they change system state / open Finder windows.

#include "core/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

using namespace lf;

TEST(Shell, F_MAC_04_ImageDocumentTypesAreValidUTTypes) {
    auto shell = createShell();
    const QStringList types = shell->imageDocumentTypes();
    ASSERT_FALSE(types.isEmpty());
    @autoreleasepool {
        for (const QString& t : types) {
            UTType* type = [UTType typeWithIdentifier:t.toNSString()];
            ASSERT_NE(type, nil) << t.toStdString();
            EXPECT_TRUE([type conformsToType:UTTypeImage]) << t.toStdString();
        }
    }
}

// The list must match CFBundleDocumentTypes (rank Default) in Info.plist (SPEC 10.4).
TEST(Shell, F_MAC_04_TypesMatchInfoPlist) {
    QFile plist(QDir::cleanPath(QFileInfo(QStringLiteral(__FILE__)).absolutePath() +
                                "/../../packaging/macos/Info.plist.in"));
    ASSERT_TRUE(plist.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(plist.readAll());
    for (const QString& t : createShell()->imageDocumentTypes())
        EXPECT_TRUE(text.contains("<string>" + t + "</string>")) << t.toStdString();
}

// F-MAC-09: outside an app bundle (the test binary) the default app cannot be compared
// with an own bundle id: Unknown (the bar is never offered), and it is not an installed app.
// Files of types the app does not declare are Unknown as well.
TEST(Shell, F_MAC_09_DefaultViewerStatusOutsideBundle) {
    QTemporaryDir dir;
    const QString jpeg = dir.filePath("a.jpg");
    const QString text = dir.filePath("a.txt");
    {
        QFile f(jpeg);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000ffd9"));
        QFile t(text);
        ASSERT_TRUE(t.open(QIODevice::WriteOnly));
        t.write("hello");
    }
    auto shell = createShell();
    EXPECT_FALSE(shell->isInstalledApp());
    EXPECT_EQ(shell->defaultViewerStatus(text).state, DefaultViewerStatus::State::Unknown);
    const DefaultViewerStatus status = shell->defaultViewerStatus(jpeg);
    if (!NSBundle.mainBundle.bundleIdentifier) {
        EXPECT_EQ(status.state, DefaultViewerStatus::State::Unknown);
    } else { // a test runner inside a bundle: a real answer for public.jpeg
        EXPECT_NE(status.state, DefaultViewerStatus::State::Unknown);
        EXPECT_EQ(status.type, QStringLiteral("public.jpeg"));
        EXPECT_FALSE(status.typeName.isEmpty());
    }
}
