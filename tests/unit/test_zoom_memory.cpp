// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-14: zoom and visible part per file for the session (core/ZoomMemory.h).

#include "core/ZoomMemory.h"

#include <gtest/gtest.h>

using namespace lf;

namespace {

ViewSyncState view(double factor, QPointF center, bool custom = true, int mode = 6) {
    ViewSyncState s;
    s.mode = mode;
    s.customZoom = custom;
    s.factor = factor;
    s.center = center;
    return s;
}

} // namespace

TEST(ZoomMemory, F_VIEW_14_RemembersPerFileAndPage) {
    ZoomMemory m;
    EXPECT_FALSE(m.recall("/a.jpg", 0));
    m.remember("/a.jpg", 0, view(2.0, {0.25, 0.75}));
    m.remember("/b.jpg", 0, view(0.5, {0.5, 0.5}));
    m.remember("/doc.pdf", 2, view(3.0, {0.1, 0.2}));
    ASSERT_TRUE(m.recall("/a.jpg", 0));
    EXPECT_EQ(*m.recall("/a.jpg", 0), view(2.0, {0.25, 0.75}));
    EXPECT_EQ(*m.recall("/b.jpg", 0), view(0.5, {0.5, 0.5}));
    EXPECT_TRUE(m.contains("/doc.pdf", 2));
    EXPECT_FALSE(m.contains("/doc.pdf", 0)); // pages are separate
    // Replacing keeps one entry.
    m.remember("/a.jpg", 0, view(4.0, {0.3, 0.3}));
    EXPECT_EQ(m.size(), 3);
    EXPECT_EQ(m.recall("/a.jpg", 0)->factor, 4.0);
    // A standard mode (not custom) is kept as such: its factor follows the window size.
    m.remember("/c.jpg", 0, view(1.0, {0.9, 0.1}, false, 2));
    EXPECT_FALSE(m.recall("/c.jpg", 0)->customZoom);
    EXPECT_EQ(m.recall("/c.jpg", 0)->mode, 2);
}

TEST(ZoomMemory, F_VIEW_14_LeastRecentlyUsedIsDropped) {
    ZoomMemory m(3);
    EXPECT_EQ(ZoomMemory().capacity(), 500);
    m.remember("/1", 0, view(1, {0.5, 0.5}));
    m.remember("/2", 0, view(2, {0.5, 0.5}));
    m.remember("/3", 0, view(3, {0.5, 0.5}));
    EXPECT_TRUE(m.recall("/1", 0)); // a use: "/2" is now the oldest
    m.remember("/4", 0, view(4, {0.5, 0.5}));
    EXPECT_EQ(m.size(), 3);
    EXPECT_TRUE(m.contains("/1", 0));
    EXPECT_FALSE(m.contains("/2", 0));
    EXPECT_TRUE(m.contains("/3", 0));
    EXPECT_TRUE(m.contains("/4", 0));
    // Updating counts as a use as well.
    m.remember("/3", 0, view(30, {0.5, 0.5}));
    m.remember("/5", 0, view(5, {0.5, 0.5}));
    EXPECT_FALSE(m.contains("/1", 0));
    EXPECT_TRUE(m.contains("/3", 0));
    m.setCapacity(1);
    EXPECT_EQ(m.size(), 1);
    EXPECT_TRUE(m.contains("/5", 0));
    // Many files: never more than the capacity.
    ZoomMemory big;
    for (int i = 0; i < 1200; ++i)
        big.remember(QStringLiteral("/f%1.jpg").arg(i), 0, view(i, {0.5, 0.5}));
    EXPECT_EQ(big.size(), 500);
    EXPECT_TRUE(big.contains("/f1199.jpg", 0));
    EXPECT_FALSE(big.contains("/f699.jpg", 0));
}

TEST(ZoomMemory, F_VIEW_14_FollowsRenameAndForgetsDeletedFiles) {
    ZoomMemory m;
    m.remember("/old.pdf", 0, view(2, {0.2, 0.2}));
    m.remember("/old.pdf", 1, view(3, {0.3, 0.3}));
    m.remember("/other.jpg", 0, view(4, {0.4, 0.4}));
    m.remember("/new.pdf", 0, view(9, {0.9, 0.9})); // replaced by the rename
    m.rename("/old.pdf", "/new.pdf");
    EXPECT_FALSE(m.contains("/old.pdf", 0));
    EXPECT_FALSE(m.contains("/old.pdf", 1));
    EXPECT_EQ(m.recall("/new.pdf", 0)->factor, 2.0);
    EXPECT_EQ(m.recall("/new.pdf", 1)->factor, 3.0);
    EXPECT_EQ(m.size(), 3);
    m.forget("/new.pdf", 1);
    EXPECT_TRUE(m.contains("/new.pdf", 0));
    EXPECT_FALSE(m.contains("/new.pdf", 1));
    m.forget("/new.pdf"); // all pages
    EXPECT_FALSE(m.contains("/new.pdf", 0));
    EXPECT_TRUE(m.contains("/other.jpg", 0));
    m.clear();
    EXPECT_EQ(m.size(), 0);
}
