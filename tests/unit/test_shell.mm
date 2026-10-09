// SPDX-License-Identifier: GPL-3.0-or-later
// Shell integration (F-MAC-04 type list). makeDefaultViewer and reveal are not
// called here because they change system state / open Finder windows.

#include "core/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <gtest/gtest.h>

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
