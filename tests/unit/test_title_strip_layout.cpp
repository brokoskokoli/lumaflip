// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-15: geometry of the title strip, side of the window buttons from the desktop
// settings and the macOS double-click preference (core/TitleStripLayout).
#include "core/TitleStripLayout.h"

#include <gtest/gtest.h>

using namespace lf;

TEST(TitleStripLayout, F_VIEW_15_WindowsButtonsRightMenuLeft) {
    TitleStripSpec spec;
    spec.width = 800;
    spec.height = 32;
    spec.ownButtons = true;
    spec.side = TitleButtonSide::Right;
    spec.menuButton = true;
    spec.buttonWidth = 46;
    const TitleStripLayout l = layoutTitleStrip(spec);
    EXPECT_EQ(l.close, QRect(754, 0, 46, 32)); // close at the right edge
    EXPECT_EQ(l.maximize, QRect(708, 0, 46, 32));
    EXPECT_EQ(l.minimize, QRect(662, 0, 46, 32));
    EXPECT_EQ(l.menu, QRect(0, 0, 46, 32));
    EXPECT_EQ(l.titleArea.left(), 46 + 8);
    EXPECT_EQ(l.titleArea.right(), 662 - 8 - 1);
    // Centered: the same inset on both sides (the larger button group).
    EXPECT_EQ(l.centeredTitle.left(), 800 - (662 - 8));
    EXPECT_EQ(l.centeredTitle.left() + l.centeredTitle.width(), 662 - 8);
    EXPECT_EQ(titleButtonAt(l, QPoint(799, 10)), TitleButton::Close);
    EXPECT_EQ(titleButtonAt(l, QPoint(730, 31)), TitleButton::Maximize);
    EXPECT_EQ(titleButtonAt(l, QPoint(662, 0)), TitleButton::Minimize);
    EXPECT_EQ(titleButtonAt(l, QPoint(5, 5)), TitleButton::Menu);
    EXPECT_EQ(titleButtonAt(l, QPoint(400, 16)), TitleButton::None); // caption: moves the window
}

TEST(TitleStripLayout, F_VIEW_15_LinuxButtonsLeft) {
    TitleStripSpec spec;
    spec.width = 600;
    spec.height = 32;
    spec.ownButtons = true;
    spec.side = TitleButtonSide::Left;
    spec.buttonWidth = 40;
    const TitleStripLayout l = layoutTitleStrip(spec);
    EXPECT_EQ(l.close, QRect(0, 0, 40, 32)); // like macOS: close first
    EXPECT_EQ(l.minimize, QRect(40, 0, 40, 32));
    EXPECT_EQ(l.maximize, QRect(80, 0, 40, 32));
    EXPECT_TRUE(l.menu.isEmpty());
    EXPECT_EQ(l.titleArea.left(), 128);
    EXPECT_EQ(l.titleArea.right(), 600 - 8 - 1);
    EXPECT_EQ(l.centeredTitle, QRect(128, 0, 600 - 2 * 128, 32));
}

TEST(TitleStripLayout, F_VIEW_15_MacSystemButtonsKeepTheirPlace) {
    TitleStripSpec spec;
    spec.width = 500;
    spec.height = 28;
    spec.ownButtons = false; // the system's buttons stay
    spec.reservedLeft = 78;
    const TitleStripLayout l = layoutTitleStrip(spec);
    EXPECT_TRUE(l.close.isEmpty() && l.minimize.isEmpty() && l.maximize.isEmpty() && l.menu.isEmpty());
    EXPECT_EQ(l.titleArea.left(), 86);
    EXPECT_EQ(l.centeredTitle, QRect(86, 0, 500 - 2 * 86, 28));
    EXPECT_EQ(titleButtonAt(l, QPoint(10, 10)), TitleButton::None);
}

TEST(TitleStripLayout, F_VIEW_15_NarrowWindowHasNoCenteredTitle) {
    TitleStripSpec spec;
    spec.width = 250;
    spec.height = 32;
    spec.ownButtons = true;
    spec.side = TitleButtonSide::Right;
    spec.menuButton = true;
    spec.buttonWidth = 46;
    const TitleStripLayout l = layoutTitleStrip(spec);
    EXPECT_TRUE(l.centeredTitle.isEmpty());
    EXPECT_GE(l.titleArea.width(), 0);
    // Degenerate sizes do not crash and give empty areas.
    spec.width = 0;
    const TitleStripLayout empty = layoutTitleStrip(spec);
    EXPECT_EQ(empty.titleArea.width(), 0);
}

TEST(TitleStripLayout, F_VIEW_15_ButtonSideFromDesktopSettings) {
    constexpr auto R = TitleButtonSide::Right;
    constexpr auto L = TitleButtonSide::Left;
    // GNOME (gsettings prints the value quoted).
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("'appmenu:minimize,maximize,close'\n"), L), R);
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("'close,minimize,maximize:'"), R), L);
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("close:appmenu"), R), L);
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("'appmenu:close'"), L), R);
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("'appmenu:'"), R), R); // no buttons: fallback
    EXPECT_EQ(buttonSideFromGnomeLayout(QStringLiteral("garbage"), L), L);
    // KDE kwinrc.
    EXPECT_EQ(buttonSideFromKdeButtons(QStringLiteral("MS"), QStringLiteral("HIAX"), L), R);
    EXPECT_EQ(buttonSideFromKdeButtons(QStringLiteral("XIA"), QStringLiteral("MS"), R), L);
    EXPECT_EQ(buttonSideFromKdeButtons(QString(), QString(), L), L);
}

TEST(TitleStripLayout, F_VIEW_15_DoubleClickFollowsMacPreference) {
    EXPECT_EQ(titleDoubleClickAction(QStringLiteral("Maximize"), std::nullopt), TitleDoubleClick::Zoom);
    EXPECT_EQ(titleDoubleClickAction(QStringLiteral("Fill"), std::nullopt), TitleDoubleClick::Zoom);
    EXPECT_EQ(titleDoubleClickAction(QStringLiteral("Minimize"), std::nullopt), TitleDoubleClick::Minimize);
    EXPECT_EQ(titleDoubleClickAction(QStringLiteral("None"), true), TitleDoubleClick::Nothing);
    // Not set: the default zoom, or the older bool preference.
    EXPECT_EQ(titleDoubleClickAction(QString(), std::nullopt), TitleDoubleClick::Zoom);
    EXPECT_EQ(titleDoubleClickAction(QString(), true), TitleDoubleClick::Minimize);
    EXPECT_EQ(titleDoubleClickAction(QString(), false), TitleDoubleClick::Zoom);
}
