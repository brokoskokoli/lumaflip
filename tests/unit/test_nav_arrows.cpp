// SPDX-License-Identifier: GPL-3.0-or-later
// F-NAV-09: when the arrow buttons at the image edges may appear and what they do.

#include "core/NavArrows.h"

#include <gtest/gtest.h>

using namespace lf;
using namespace lf::navarrows;

namespace {

Context viewing() {
    Context c;
    c.enabled = true;
    c.hasImage = true;
    c.viewShown = true;
    return c;
}

Position at(int index, int count, EndBehavior end = EndBehavior::Stop) {
    Position p;
    p.index = index;
    p.count = count;
    p.end = end;
    return p;
}

} // namespace

TEST(NavArrows, F_NAV_09_OnlyInTheSingleImageView) {
    EXPECT_TRUE(allowed(viewing()));
    Context c = viewing();
    c.enabled = false; // setting off
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.hasImage = false; // start screen
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.viewShown = false; // grid, compare
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.cropping = true;
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.loupe = true;
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.slideshow = true;
    EXPECT_FALSE(allowed(c));
    c = viewing();
    c.liveText = true;
    EXPECT_FALSE(allowed(c));
}

TEST(NavArrows, F_NAV_09_HiddenAtTheEndsWithoutWrap) {
    Buttons b = buttons(at(0, 3));
    EXPECT_EQ(b.left, Action::PreviousImage);
    EXPECT_EQ(b.right, Action::NextImage);
    EXPECT_FALSE(b.leftShown);
    EXPECT_TRUE(b.rightShown);
    b = buttons(at(1, 3));
    EXPECT_TRUE(b.leftShown);
    EXPECT_TRUE(b.rightShown);
    b = buttons(at(2, 3));
    EXPECT_TRUE(b.leftShown);
    EXPECT_FALSE(b.rightShown);
    b = buttons(at(0, 1)); // a single image: nothing to go to
    EXPECT_FALSE(b.leftShown);
    EXPECT_FALSE(b.rightShown);
    b = buttons(at(-1, 0));
    EXPECT_FALSE(b.leftShown);
    EXPECT_FALSE(b.rightShown);
}

TEST(NavArrows, F_NAV_09_WrapAndNextFolderAlwaysHaveATarget) {
    Buttons b = buttons(at(0, 3, EndBehavior::Wrap));
    EXPECT_TRUE(b.leftShown);
    EXPECT_TRUE(b.rightShown);
    b = buttons(at(0, 1, EndBehavior::Wrap)); // wrapping onto itself is no target
    EXPECT_FALSE(b.leftShown);
    EXPECT_FALSE(b.rightShown);
    // "Zum nächsten Ordner weitergehen": only where a sibling folder/archive can follow.
    Position p = at(2, 3, EndBehavior::NextFolder);
    p.canSwitchFolder = true;
    b = buttons(p);
    EXPECT_TRUE(b.leftShown);
    EXPECT_TRUE(b.rightShown);
    p = at(0, 1, EndBehavior::NextFolder);
    p.canSwitchFolder = true;
    EXPECT_TRUE(buttons(p).rightShown);
    p.canSwitchFolder = false; // several files opened: stops at the ends
    p.index = 0;
    p.count = 2;
    b = buttons(p);
    EXPECT_FALSE(b.leftShown);
    EXPECT_TRUE(b.rightShown);
    // Shuffled order: both while there is more than one image.
    p = at(0, 3);
    p.shuffle = true;
    b = buttons(p);
    EXPECT_TRUE(b.leftShown && b.rightShown);
}

TEST(NavArrows, F_NAV_09_ComicsRightToLeftSwapTheButtons) {
    Position p = at(0, 3);
    p.rightToLeft = true;
    const Buttons b = buttons(p);
    EXPECT_EQ(b.left, Action::NextImage); // like ←
    EXPECT_EQ(b.right, Action::PreviousImage);
    EXPECT_TRUE(b.leftShown);   // the first page has a next one
    EXPECT_FALSE(b.rightShown); // but no previous one
}

TEST(NavArrows, F_NAV_09_EndBehaviorFromSetting) {
    EXPECT_EQ(endBehaviorFromId(QStringLiteral("stop")), EndBehavior::Stop);
    EXPECT_EQ(endBehaviorFromId(QStringLiteral("wrap")), EndBehavior::Wrap);
    EXPECT_EQ(endBehaviorFromId(QStringLiteral("nextFolder")), EndBehavior::NextFolder);
    EXPECT_EQ(endBehaviorFromId(QStringLiteral("bogus")), EndBehavior::Stop);
}
