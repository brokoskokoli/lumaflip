// SPDX-License-Identifier: GPL-3.0-or-later
// M3 views and navigation without widgets: grid size/movement/selection (F-GRID-02),
// synchronized zoom and pan (F-GRID-03, F-WIN-07), loupe geometry (F-VIEW-11), coupled
// windows (F-WIN-07), go to / shuffle (F-NAV-06), name filter (F-NAV-08) and keys during a
// slideshow (F-SLIDE-03).

#include "QtTestApp.h"
#include "core/Collection.h"
#include "core/GridSelection.h"
#include "core/InputBindings.h"
#include "core/LoupeGeometry.h"
#include "core/NameFilter.h"
#include "core/NavigationOrder.h"
#include "core/Settings.h"
#include "core/SlideshowInput.h"
#include "core/ViewSync.h"
#include "core/WindowCoupling.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cmath>

using namespace lf;

namespace {

QStringList paths(int n) {
    QStringList out;
    for (int i = 0; i < n; ++i)
        out << QStringLiteral("/f/img%1.jpg").arg(i, 2, 10, QLatin1Char('0'));
    return out;
}

bool near(QPointF a, QPointF b, double eps = 1e-9) {
    return std::abs(a.x() - b.x()) < eps && std::abs(a.y() - b.y()) < eps;
}

} // namespace

// ---------------------------------------------------------------------------------------
// F-GRID-02
// ---------------------------------------------------------------------------------------

TEST(GridLogic, F_GRID_02_SizeLadderAndColumns) {
    const auto& s = grid::sizeSteps();
    ASSERT_GE(s.size(), 4);
    EXPECT_TRUE(s.contains(grid::kDefaultSize));
    EXPECT_TRUE(std::is_sorted(s.begin(), s.end()));
    EXPECT_GT(grid::stepSize(grid::kDefaultSize, 1), grid::kDefaultSize);
    EXPECT_LT(grid::stepSize(grid::kDefaultSize, -1), grid::kDefaultSize);
    EXPECT_EQ(grid::stepSize(grid::kDefaultSize, -1), s.at(s.indexOf(grid::kDefaultSize) - 1));
    // Clamped at both ends; a size between two steps snaps in the direction of the step.
    EXPECT_EQ(grid::stepSize(s.last(), 3), s.last());
    EXPECT_EQ(grid::stepSize(s.first(), -3), s.first());
    EXPECT_EQ(grid::stepSize(s.at(2) + 1, 1), s.at(3));
    EXPECT_EQ(grid::stepSize(s.at(2) + 1, -1), s.at(2));
    EXPECT_EQ(grid::scaledSize(160, 2.0), std::min(320, s.last()));
    EXPECT_EQ(grid::scaledSize(160, 100.0), s.last());
    EXPECT_EQ(grid::scaledSize(160, 0.01), s.first());
    EXPECT_EQ(grid::columnsFor(1000, 200), 5);
    EXPECT_EQ(grid::columnsFor(100, 200), 1);
    EXPECT_EQ(grid::columnsFor(1000, 0), 1);
}

TEST(GridLogic, F_GRID_02_KeyboardMovement) {
    using grid::Move;
    // 10 items in 4 columns: rows [0..3] [4..7] [8 9]
    EXPECT_EQ(grid::moveIndex(5, Move::Right, 4, 10, 2), 6);
    EXPECT_EQ(grid::moveIndex(3, Move::Right, 4, 10, 2), 4); // wraps into the next row
    EXPECT_EQ(grid::moveIndex(9, Move::Right, 4, 10, 2), 9);
    EXPECT_EQ(grid::moveIndex(0, Move::Left, 4, 10, 2), 0);
    EXPECT_EQ(grid::moveIndex(5, Move::Up, 4, 10, 2), 1);
    EXPECT_EQ(grid::moveIndex(1, Move::Up, 4, 10, 2), 1);
    EXPECT_EQ(grid::moveIndex(1, Move::Down, 4, 10, 2), 5);
    EXPECT_EQ(grid::moveIndex(6, Move::Down, 4, 10, 2), 9); // partial last row: its last item
    EXPECT_EQ(grid::moveIndex(9, Move::Down, 4, 10, 2), 9);
    EXPECT_EQ(grid::moveIndex(9, Move::PageUp, 4, 10, 2), 1);
    EXPECT_EQ(grid::moveIndex(1, Move::PageDown, 4, 10, 2), 9);
    EXPECT_EQ(grid::moveIndex(4, Move::Home, 4, 10, 2), 0);
    EXPECT_EQ(grid::moveIndex(4, Move::End, 4, 10, 2), 9);
    EXPECT_EQ(grid::moveIndex(-1, Move::Right, 4, 10, 2), 0);
    EXPECT_EQ(grid::moveIndex(0, Move::Right, 4, 0, 2), -1);
}

TEST(GridLogic, F_GRID_02_ClickSelection) {
    GridSelection s;
    const QStringList p = paths(8);
    s.reset(p, p.at(2));
    EXPECT_EQ(s.cursor(), 2);
    EXPECT_EQ(s.selectedPaths(), QStringList{p.at(2)});
    s.click(5, false, false); // plain click: only this one
    EXPECT_EQ(s.selectedRows(), QList<int>({5}));
    s.click(1, true, false); // ⌘: toggle
    EXPECT_EQ(s.selectedRows(), QList<int>({1, 5}));
    s.click(1, true, false);
    EXPECT_EQ(s.selectedRows(), QList<int>({5}));
    s.click(3, false, false);
    s.click(6, false, true); // ⇧: range from the anchor (replaces)
    EXPECT_EQ(s.selectedRows(), QList<int>({3, 4, 5, 6}));
    s.click(4, false, true); // the anchor stays at 3
    EXPECT_EQ(s.selectedRows(), QList<int>({3, 4}));
    s.click(0, true, false);  // ⌘ adds 0 and moves the anchor
    s.click(1, true, true);   // ⌘⇧ adds the range 0..1
    EXPECT_EQ(s.selectedRows(), QList<int>({0, 1, 3, 4}));
    EXPECT_EQ(s.selectedPaths(), (QStringList{p.at(0), p.at(1), p.at(3), p.at(4)}));
    s.selectAll();
    EXPECT_EQ(s.selectedCount(), 8);
    s.clear();
    EXPECT_EQ(s.selectedCount(), 0);
    EXPECT_EQ(s.cursor(), 1);
    s.click(99, false, false); // outside: ignored
    EXPECT_EQ(s.cursor(), 1);
}

TEST(GridLogic, F_GRID_02_KeyboardSelection) {
    GridSelection s;
    const QStringList p = paths(10);
    s.reset(p, p.at(4));
    s.moveTo(5, true);
    s.moveTo(6, true);
    EXPECT_EQ(s.selectedRows(), QList<int>({4, 5, 6}));
    s.moveTo(2, true); // back over the anchor
    EXPECT_EQ(s.selectedRows(), QList<int>({2, 3, 4}));
    s.moveTo(8, false); // plain arrow: only the new item
    EXPECT_EQ(s.selectedRows(), QList<int>({8}));
    EXPECT_EQ(s.anchor(), 8);
}

TEST(GridLogic, F_GRID_02_SelectionFollowsCollectionChanges) {
    GridSelection s;
    QStringList p = paths(6);
    s.reset(p, p.at(1));
    s.click(3, true, false);
    // Re-sorted (reversed) and one file gone: selection and cursor stay on their files.
    QStringList q = p;
    std::reverse(q.begin(), q.end());
    q.removeAll(p.at(5));
    s.setPaths(q);
    EXPECT_EQ(s.selectedPaths().size(), 2);
    EXPECT_TRUE(s.selectedPaths().contains(p.at(1)));
    EXPECT_TRUE(s.selectedPaths().contains(p.at(3)));
    EXPECT_EQ(s.cursorPath(), p.at(3));
    // The cursor's file leaves: the cursor stays at its position (clamped), selection shrinks.
    q.removeAll(p.at(3));
    s.setPaths(q);
    EXPECT_EQ(s.selectedPaths(), QStringList{p.at(1)});
    EXPECT_GE(s.cursor(), 0);
    EXPECT_LT(s.cursor(), q.size());
    s.setPaths({});
    EXPECT_EQ(s.cursor(), -1);
    EXPECT_EQ(s.selectedCount(), 0);
}

// ---------------------------------------------------------------------------------------
// F-GRID-03 / F-WIN-07 synchronized zoom and pan
// ---------------------------------------------------------------------------------------

namespace {
ViewSyncState custom(double factor, QPointF center) {
    return ViewSyncState{6, true, factor, center};
}
} // namespace

TEST(ViewSyncMath, F_GRID_03_PanAndZoomFollow) {
    ViewSync sync(3);
    sync.reset(ViewSyncState{0, false, 0.25, {0.5, 0.5}});
    const auto targets = sync.changed(1, custom(2.0, {0.3, 0.6}), false);
    ASSERT_EQ(targets.size(), 2);
    EXPECT_EQ(targets.at(0).member, 0);
    EXPECT_EQ(targets.at(1).member, 2);
    for (const auto& t : targets) {
        EXPECT_TRUE(t.state.customZoom);
        EXPECT_DOUBLE_EQ(t.state.factor, 2.0);
        EXPECT_TRUE(near(t.state.center, {0.3, 0.6}));
    }
    // A zoom mode (Fit) is passed on as the mode.
    const auto fit = sync.changed(0, ViewSyncState{0, false, 0.4, {0.5, 0.5}}, false);
    ASSERT_EQ(fit.size(), 2);
    EXPECT_FALSE(fit.at(0).state.customZoom);
    EXPECT_EQ(fit.at(0).state.mode, 0);
}

TEST(ViewSyncMath, F_GRID_03_OptionMovesOnePaneIndependently) {
    ViewSync sync(2);
    sync.reset(custom(1.0, {0.5, 0.5}));
    // ⌥-drag of pane 1: nothing follows, pane 1 keeps an offset.
    EXPECT_TRUE(sync.changed(1, custom(1.0, {0.6, 0.45}), true).isEmpty());
    EXPECT_TRUE(near(sync.offset(1), {0.1, -0.05}));
    EXPECT_TRUE(near(sync.offset(0), {0, 0}));
    EXPECT_TRUE(near(sync.shared().center, {0.5, 0.5}));
    // A normal pan of pane 0 moves pane 1 with its offset.
    auto t = sync.changed(0, custom(1.0, {0.3, 0.3}), false);
    ASSERT_EQ(t.size(), 1);
    EXPECT_TRUE(near(t.at(0).state.center, {0.4, 0.25}));
    // A normal pan of pane 1 moves the shared view by pane 1's position minus its offset.
    t = sync.changed(1, custom(1.0, {0.7, 0.7}), false);
    ASSERT_EQ(t.size(), 1);
    EXPECT_EQ(t.at(0).member, 0);
    EXPECT_TRUE(near(t.at(0).state.center, {0.6, 0.75}));
    // Centers are clamped to the image.
    t = sync.changed(1, custom(1.0, {1.0, 0.0}), false);
    EXPECT_TRUE(near(t.at(0).state.center, {0.9, 0.05}));
    EXPECT_TRUE(near(ViewSync::clampCenter({-1, 2}), {0, 1}));
}

TEST(ViewSyncMath, F_GRID_03_OptionZoomKeepsRatio) {
    ViewSync sync(2);
    sync.reset(custom(1.0, {0.5, 0.5}));
    sync.changed(1, custom(2.0, {0.5, 0.5}), true); // ⌥: pane 1 zooms on its own
    EXPECT_DOUBLE_EQ(sync.zoomRatio(1), 2.0);
    auto t = sync.changed(0, custom(0.5, {0.5, 0.5}), false);
    ASSERT_EQ(t.size(), 1);
    EXPECT_DOUBLE_EQ(t.at(0).state.factor, 1.0); // keeps twice the shared factor
    t = sync.changed(1, custom(4.0, {0.5, 0.5}), false);
    EXPECT_DOUBLE_EQ(t.at(0).state.factor, 2.0);
    // A zoom mode ends the own zoom of every pane.
    t = sync.changed(0, ViewSyncState{0, false, 0.3, {0.5, 0.5}}, false);
    EXPECT_FALSE(t.at(0).state.customZoom);
    EXPECT_DOUBLE_EQ(sync.zoomRatio(1), 1.0);
}

TEST(ViewSyncMath, F_WIN_07_MembersComeAndGo) {
    ViewSync sync(2);
    sync.reset(custom(1.0, {0.5, 0.5}));
    sync.changed(1, custom(1.0, {0.7, 0.5}), true);
    sync.setMemberCount(3);
    EXPECT_TRUE(near(sync.offset(2), {0, 0}));
    sync.removeMember(0);
    EXPECT_EQ(sync.memberCount(), 2);
    EXPECT_TRUE(near(sync.offset(0), {0.2, 0}));
    EXPECT_TRUE(sync.changed(5, custom(1, {0, 0}), false).isEmpty()); // unknown member
}

// ---------------------------------------------------------------------------------------
// F-VIEW-11 loupe
// ---------------------------------------------------------------------------------------

TEST(LoupeMath, F_VIEW_11_MagnifiesAroundCursor) {
    // View: image 4000×3000 fitted at 0.25 (factor), widget = device px (dpr 1).
    QTransform view;
    view.translate(100, 50);
    view.scale(0.25, 0.25);
    const QPointF cursor(300, 250); // image (800, 800)
    const auto g = loupe::compute(view, {4000, 3000}, cursor, 200, 0.25, 1.0, 1.0);
    EXPECT_TRUE(g.overImage);
    EXPECT_EQ(g.target, QRectF(200, 150, 200, 200));
    EXPECT_NEAR(g.deviceScale, 1.0, 1e-9); // 100 %: one image px per device px
    // The cursor still points at the same image pixel.
    EXPECT_TRUE(near(g.transform.map(QPointF(800, 800)), cursor, 1e-6));
    // 200 px of loupe = 200 image px around (800, 800).
    EXPECT_TRUE(near(g.source.topLeft(), {700, 700}, 1e-6));
    EXPECT_TRUE(near(g.source.bottomRight(), {900, 900}, 1e-6));

    // 200 % on a Retina display (dpr 2): device scale 2, half the source in logical px.
    QTransform retina;
    retina.scale(0.125, 0.125); // logical px; factor 0.25 in device px
    const auto r = loupe::compute(retina, {4000, 3000}, QPointF(100, 100), 200, 0.25, 2.0, 2.0);
    EXPECT_NEAR(r.deviceScale, 2.0, 1e-9);
    EXPECT_NEAR(r.source.width(), 200, 1e-6); // 200 logical = 400 device px = 200 image px
}

TEST(LoupeMath, F_VIEW_11_KeepsRotationAndClipsToImage) {
    QTransform view; // 90° rotated view
    view.translate(500, 0);
    view.rotate(90);
    view.scale(0.5, 0.5);
    const auto g = loupe::compute(view, {1000, 800}, QPointF(500, 0), 100, 0.5, 1.0, 1.0);
    // Still rotated: image x axis maps to widget y axis.
    const QPointF a = g.transform.map(QPointF(0, 0));
    const QPointF b = g.transform.map(QPointF(10, 0));
    EXPECT_NEAR(a.x(), b.x(), 1e-6);
    EXPECT_NEAR(b.y() - a.y(), 10.0, 1e-6);
    // The cursor at the image corner: the source is clipped to the image.
    EXPECT_GE(g.source.left(), 0);
    EXPECT_GE(g.source.top(), 0);
    // Outside the image.
    const auto out = loupe::compute(view, {1000, 800}, QPointF(-50, -50), 100, 0.5, 1.0, 1.0);
    EXPECT_FALSE(out.overImage);
    // Degenerate inputs.
    EXPECT_FALSE(loupe::compute(view, {}, QPointF(1, 1), 100, 0.5, 1.0, 1.0).overImage);
}

TEST(LoupeMath, F_VIEW_11_FullResolutionAndSettings) {
    EXPECT_TRUE(loupe::needsFullResolution({1000, 750}, {4000, 3000}, 1.0));
    EXPECT_FALSE(loupe::needsFullResolution({4000, 3000}, {4000, 3000}, 2.0));
    EXPECT_FALSE(loupe::needsFullResolution({1000, 750}, {4000, 3000}, 0.25));
    EXPECT_FALSE(loupe::needsFullResolution({}, {4000, 3000}, 1.0));
    EXPECT_EQ(loupe::diameterFor("small"), 160);
    EXPECT_EQ(loupe::diameterFor("medium"), 240);
    EXPECT_EQ(loupe::diameterFor("large"), 360);
    EXPECT_EQ(loupe::diameterFor("bogus"), 240);
    EXPECT_DOUBLE_EQ(loupe::magnificationFor("100"), 1.0);
    EXPECT_DOUBLE_EQ(loupe::magnificationFor("200"), 2.0);
}

// ---------------------------------------------------------------------------------------
// F-WIN-07 coupled windows
// ---------------------------------------------------------------------------------------

TEST(CouplingLogic, F_WIN_07_ByIndex) {
    const QStringList follower = paths(5);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Index, 2, "/a/x.jpg", follower), 2);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Index, 9, "/a/x.jpg", follower), 4); // clamped
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Index, 0, "/a/x.jpg", {}), -1);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Index, -1, "/a/x.jpg", follower), -1);
}

TEST(CouplingLogic, F_WIN_07_ByName) {
    const QStringList follower = {"/b/IMG_0001.jpg", "/b/IMG_0002.CR3", "/b/IMG_0002.jpg",
                                  QString::fromUtf8("/b/Mu\xCC\x88nchen.heic")}; // NFD "Mü"
    // Exact file name first, then the name without extension.
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Name, 0, "/a/IMG_0002.jpg", follower), 2);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Name, 0, "/a/IMG_0002.tif", follower), 1);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Name, 0, "/a/img_0001.PNG", follower), 0);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Name, 0, QString::fromUtf8("/a/M\xC3\xBCnchen.jpg"), follower), 3);
    EXPECT_EQ(coupling::followerIndex(CoupleBy::Name, 3, "/a/other.jpg", follower), -1);
    EXPECT_EQ(coupling::matchKey("/x/Photo.final.JPG"), coupling::matchKey("photo.final.png"));
    EXPECT_EQ(coupling::fromId("index"), CoupleBy::Index);
    EXPECT_EQ(coupling::fromId("name"), CoupleBy::Name);
    EXPECT_EQ(coupling::fromId("?"), CoupleBy::Name);
    EXPECT_EQ(coupling::toId(CoupleBy::Index), "index");
}

// ---------------------------------------------------------------------------------------
// F-NAV-06 go to / shuffle
// ---------------------------------------------------------------------------------------

TEST(NavOrder, F_NAV_06_GoToParsing) {
    EXPECT_EQ(nav::parseGoTo("1", 5, 100), 0);
    EXPECT_EQ(nav::parseGoTo(" 23 ", 5, 100), 22);
    EXPECT_EQ(nav::parseGoTo("1 000", 5, 2000), 999);
    EXPECT_EQ(nav::parseGoTo("500", 5, 100), 99); // clamped
    EXPECT_EQ(nav::parseGoTo("0", 5, 100), 0);
    EXPECT_EQ(nav::parseGoTo("+10", 5, 100), 15);
    EXPECT_EQ(nav::parseGoTo("-10", 5, 100), 0);
    EXPECT_EQ(nav::parseGoTo(QString::fromUtf8("\xE2\x88\x92" "2"), 5, 100), 3); // U+2212 minus
    EXPECT_FALSE(nav::parseGoTo("", 5, 100));
    EXPECT_FALSE(nav::parseGoTo("abc", 5, 100));
    EXPECT_FALSE(nav::parseGoTo("3", 0, 0));
}

TEST(NavOrder, F_NAV_06_ShuffleIsDeterministicPerSeed) {
    const QStringList p = paths(40);
    ShuffleOrder a(42), b(42), c(43);
    a.setPaths(p);
    b.setPaths(p);
    c.setPaths(p);
    EXPECT_EQ(a.order(), b.order());
    EXPECT_NE(a.order(), c.order());
    EXPECT_NE(a.order(), p); // actually shuffled
    QStringList sorted = a.order();
    sorted.sort();
    EXPECT_EQ(sorted, p); // a permutation
    // Same order whatever the input order is.
    QStringList reversed = p;
    std::reverse(reversed.begin(), reversed.end());
    b.setPaths(reversed);
    EXPECT_EQ(a.order(), b.order());
    // One seed for the whole session.
    EXPECT_EQ(ShuffleOrder::sessionSeed(), ShuffleOrder::sessionSeed());
    EXPECT_EQ(ShuffleOrder().seed(), ShuffleOrder::sessionSeed());
}

TEST(NavOrder, F_NAV_06_ShuffleStableUnderChangesAndStepping) {
    const QStringList p = paths(20);
    ShuffleOrder s(7);
    s.setPaths(p);
    const QStringList before = s.order();
    QStringList more = p;
    more << "/f/new.jpg";
    more.removeAll(p.at(3));
    s.setPaths(more);
    QStringList expected = before;
    expected.removeAll(p.at(3));
    QStringList now = s.order();
    now.removeAll("/f/new.jpg");
    EXPECT_EQ(now, expected); // the others keep their relative order

    const QString first = s.first();
    bool end = false;
    EXPECT_EQ(s.step(first, 1, false, &end), s.order().at(1));
    EXPECT_FALSE(end);
    EXPECT_EQ(s.step(first, -1, false, &end), first);
    EXPECT_TRUE(end);
    EXPECT_EQ(s.step(first, -1, true, &end), s.last());
    EXPECT_FALSE(end);
    EXPECT_EQ(s.step(s.last(), 1, true), first);
    EXPECT_EQ(s.step("/unknown", 1, false), first);
    EXPECT_EQ(s.step("/unknown", -1, false), s.last());
    EXPECT_EQ(s.positionOf(first), 0);
    EXPECT_EQ(s.positionOf("/unknown"), -1);
}

// ---------------------------------------------------------------------------------------
// F-NAV-08 name filter
// ---------------------------------------------------------------------------------------

TEST(NameFilterMatch, F_NAV_08_CaseDiacriticsAndNormalization) {
    const QString nfd = QString::fromUtf8("Mu\xCC\x88nchen 2024.JPG"); // as HFS+/APFS lists it
    const QString nfc = QString::fromUtf8("M\xC3\xBCnchen 2024.jpg");
    for (const QString& name : {nfd, nfc}) {
        SCOPED_TRACE(name.toStdString());
        EXPECT_TRUE(namefilter::matches(name, QString::fromUtf8("m\xC3\xBCnchen")));        // typed NFC
        EXPECT_TRUE(namefilter::matches(name, QString::fromUtf8("MU\xCC\x88NCHEN")));       // NFD, upper case
        EXPECT_TRUE(namefilter::matches(name, QStringLiteral("munchen")));                  // no umlaut
        EXPECT_TRUE(namefilter::matches(name, QStringLiteral("2024 mun")));                 // all words
        EXPECT_FALSE(namefilter::matches(name, QStringLiteral("2024 berlin")));
        EXPECT_TRUE(namefilter::matches(name, QStringLiteral("  ")));                       // empty
    }
    EXPECT_TRUE(namefilter::matches(QString::fromUtf8("Stra\xC3\x9F" "e.png"), QStringLiteral("STRASSE")));
    EXPECT_TRUE(namefilter::matches(QStringLiteral("Café.heic"), QStringLiteral("CAFE")));
    EXPECT_EQ(namefilter::fold(QString::fromUtf8("\xC3\x84")), QStringLiteral("a"));
    EXPECT_EQ(namefilter::terms(QStringLiteral(" a  B ")), (QStringList{"a", "b"}));
}

TEST(NameFilterMatch, F_NAV_08_CollectionFiltersLiveAndCombinesWithRatings) {
    test::ensureQtApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QStringList names = {"alpha.jpg", "beta.jpg", QString::fromUtf8("Mu\xCC\x88nchen.jpg"), "gamma.jpg"};
    for (const QString& n : names) {
        QFile f(dir.filePath(n));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    Collection c;
    c.openFolder(dir.path());
    ASSERT_TRUE(test::waitUntil([&] { return !c.isLoading() && c.count() == 4; }, 5000));
    c.setCurrentIndex(1); // beta
    int changed = 0;
    QObject::connect(&c, &Collection::entriesChanged, [&changed] { ++changed; });
    c.setNameFilter(QString::fromUtf8("m\xC3\xBCn"));
    EXPECT_FALSE(c.isLoading()); // applied at once
    ASSERT_EQ(c.count(), 1);
    EXPECT_TRUE(c.at(0).name.contains(QStringLiteral("nchen")));
    EXPECT_EQ(c.currentIndex(), 0); // the current image left: the first match is current
    EXPECT_EQ(c.totalCount(), 4);
    EXPECT_GE(changed, 1);
    c.setNameFilter(QStringLiteral("a"));
    EXPECT_EQ(c.count(), 3); // alpha, beta, gamma (sorted again)
    EXPECT_EQ(c.at(0).name, QStringLiteral("alpha.jpg"));
    c.setNameFilter(QStringLiteral("zzz"));
    EXPECT_EQ(c.count(), 0);
    EXPECT_EQ(c.currentIndex(), -1);
    c.setNameFilter(QString());
    EXPECT_EQ(c.count(), 4);
    EXPECT_EQ(c.nameFilter(), QString());

    // With a rating filter: both must match (the reader says: only "gamma" has 3 stars).
    c.setRatingReader([](const QString& p) {
        Rating r;
        r.stars = p.endsWith(QLatin1String("gamma.jpg")) ? 3 : 0;
        return r;
    });
    RatingFilter min3;
    min3.kind = RatingFilter::Kind::MinStars;
    min3.minStars = 3;
    c.setNameFilter(QStringLiteral("a"));
    c.setRatingFilter(min3);
    ASSERT_TRUE(test::waitUntil([&] { return !c.isLoading(); }, 5000));
    ASSERT_EQ(c.count(), 1);
    EXPECT_EQ(c.at(0).name, QStringLiteral("gamma.jpg"));
    c.setNameFilter(QStringLiteral("beta")); // matches the name, not the rating
    EXPECT_EQ(c.count(), 0);
    c.setNameFilter(QString());
    EXPECT_EQ(c.count(), 1);
}

// ---------------------------------------------------------------------------------------
// F-SLIDE-03 rating during the slideshow
// ---------------------------------------------------------------------------------------

namespace {
InputEvent keyEvent(int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    InputEvent e;
    e.kind = InputKind::Key;
    e.key = k;
    e.modifiers = mods;
    return e;
}
} // namespace

TEST(SlideshowKeys, F_SLIDE_03_RatingDispatch) {
    QTemporaryDir dir;
    Settings::useTestingBackend(dir.filePath("s.ini"));
    InputBindings b;
    using slideshowinput::Kind;
    // Space pauses, 1–9 set the interval (F-SLIDE-01) while digits are no rating keys.
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_Space), false).kind, Kind::TogglePause);
    auto r = slideshowinput::dispatch(b, keyEvent(Qt::Key_3), false);
    EXPECT_EQ(r.kind, Kind::SetInterval);
    EXPECT_EQ(r.seconds, 3);
    // P / X / U always rate (no conflict with the interval digits).
    r = slideshowinput::dispatch(b, keyEvent(Qt::Key_P), false);
    EXPECT_EQ(r.kind, Kind::Rate);
    EXPECT_EQ(r.rating, Action::RatePick);
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_X), false).rating, Action::RateReject);
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_U), false).rating, Action::RateClear);
    // Rating keys active (culling mode / "immer"): 0–5 rate, 6–9 still set the interval.
    r = slideshowinput::dispatch(b, keyEvent(Qt::Key_4), true);
    EXPECT_EQ(r.kind, Kind::Rate);
    EXPECT_EQ(r.rating, Action::Rate4);
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_0), true).rating, Action::Rate0);
    r = slideshowinput::dispatch(b, keyEvent(Qt::Key_7), true);
    EXPECT_EQ(r.kind, Kind::SetInterval);
    EXPECT_EQ(r.seconds, 7);
    // Other keys are left to the normal handling.
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_Right), true).kind, Kind::None);
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_3, Qt::ControlModifier), false).kind, Kind::None);
    EXPECT_EQ(slideshowinput::dispatch(b, keyEvent(Qt::Key_0), false).kind, Kind::None);
    InputEvent wheel;
    wheel.kind = InputKind::Wheel;
    EXPECT_EQ(slideshowinput::dispatch(b, wheel, true).kind, Kind::None);
    // A user shortcut for a rating action works too.
    b.setShortcut(Action::Rate5, QKeySequence(QStringLiteral("Shift+5")));
    r = slideshowinput::dispatch(b, keyEvent(Qt::Key_5, Qt::ShiftModifier), false);
    EXPECT_EQ(r.kind, Kind::Rate);
    EXPECT_EQ(r.rating, Action::Rate5);
}
