// SPDX-License-Identifier: GPL-3.0-or-later
// Ratings in the Collection: sorting by the RatingStore's ratings (F-SORT-02) and the
// rating filter (F-CULL-04). Real sidecar files in a temporary folder.

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/RatingStore.h"
#include "core/Xmp.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace lf;

namespace {

Rating stars(int n, PickFlag flag = PickFlag::None) {
    Rating r;
    r.stars = n;
    r.flag = flag;
    return r;
}

QStringList names(const Collection& c) {
    QStringList out;
    for (const FileEntry& e : c.entries())
        out << e.name;
    return out;
}

class CullingCollectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureApp();
        ASSERT_TRUE(dir.isValid());
        collator = createNameCollator();
        // a: 3 stars, b: reject (4 stars kept), c: unrated, d: pick 1 star, e: 5 stars
        add("a.jpg", stars(3));
        add("b.jpg", stars(4, PickFlag::Reject));
        add("c.jpg", Rating());
        add("d.jpg", stars(1, PickFlag::Pick));
        add("e.jpg", stars(5));
    }
    QString path(const QString& name) const { return QDir::cleanPath(dir.path()) + "/" + name; }
    void add(const QString& name, const Rating& r) {
        QFile f(path(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        if (!r.isEmpty())
            ASSERT_TRUE(ratingio::writeSidecar(path(name), r).isEmpty());
    }
    std::unique_ptr<Collection> makeCollection() {
        auto c = std::make_unique<Collection>();
        c->setProviders(collator.get(), &order, &attributes);
        c->setRatingReader([o = RatingOptions()](const QString& p) { return ratingio::read(p, o); });
        QObject::connect(c.get(), &Collection::loadingFinished, [this] { ++finished; });
        return c;
    }
    bool waitLoaded(Collection& c, int expected) {
        return test::waitUntil([&] { return finished >= expected && !c.isLoading(); });
    }

    QTemporaryDir dir;
    std::unique_ptr<NameCollator> collator;
    test::FakeOrderProvider order;
    test::FakeAttributeProvider attributes;
    int finished = 0;
};

} // namespace

TEST_F(CullingCollectionTest, F_SORT_02_SortByRatingFromStore) {
    auto c = makeCollection();
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    c->setSortSpec(SortSpec{SortKey::Rating, false, SortSource::Manual});
    ASSERT_TRUE(waitLoaded(*c, 2));
    // Descending: 5 stars, 3 stars, 1 star + pick, unrated, reject last.
    EXPECT_EQ(names(*c), (QStringList{"e.jpg", "a.jpg", "d.jpg", "c.jpg", "b.jpg"}));
    c->setSortSpec(SortSpec{SortKey::Rating, true, SortSource::Manual});
    ASSERT_TRUE(waitLoaded(*c, 3));
    EXPECT_EQ(names(*c), (QStringList{"b.jpg", "c.jpg", "d.jpg", "a.jpg", "e.jpg"}));
    for (const FileEntry& e : c->entries())
        EXPECT_TRUE(e.ratingRead);
    EXPECT_EQ(collection::ratingOf(c->at(c->indexOf(path("b.jpg")))), stars(4, PickFlag::Reject));
}

TEST_F(CullingCollectionTest, F_CULL_04_FilterShowsMatchingAndCounts) {
    auto c = makeCollection();
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(c->count(), 5);
    c->setRatingFilter(RatingFilter{RatingFilter::Kind::MinStars, 3});
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "e.jpg"})); // b is rejected
    EXPECT_EQ(c->totalCount(), 5);
    EXPECT_EQ(c->allPaths().size(), 5);
    c->setRatingFilter(RatingFilter{RatingFilter::Kind::Picks, 1});
    ASSERT_TRUE(waitLoaded(*c, 3));
    EXPECT_EQ(names(*c), QStringList{"d.jpg"});
    c->setRatingFilter(RatingFilter{RatingFilter::Kind::Unrated, 1});
    ASSERT_TRUE(waitLoaded(*c, 4));
    EXPECT_EQ(names(*c), QStringList{"c.jpg"});
    c->setRatingFilter(RatingFilter{RatingFilter::Kind::Rejects, 1});
    ASSERT_TRUE(waitLoaded(*c, 5));
    EXPECT_EQ(names(*c), QStringList{"b.jpg"});
    c->setRatingFilter(RatingFilter());
    ASSERT_TRUE(waitLoaded(*c, 6));
    EXPECT_EQ(c->count(), 5);
}

TEST_F(CullingCollectionTest, F_CULL_04_RatedOutImageLeavesWhenNavigatingAway) {
    auto c = makeCollection();
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    c->setRatingFilter(RatingFilter{RatingFilter::Kind::MinStars, 1});
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "d.jpg", "e.jpg"}));
    ASSERT_TRUE(c->setCurrentPath(path("a.jpg")));
    // The current image is rated out: it stays while shown ...
    c->updateRating(path("a.jpg"), stars(0, PickFlag::Reject));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "d.jpg", "e.jpg"}));
    EXPECT_EQ(c->currentPath(), path("a.jpg"));
    // ... and disappears when the next image is shown.
    ASSERT_TRUE(c->next(WrapMode::Stop));
    EXPECT_EQ(c->currentPath(), path("d.jpg"));
    EXPECT_EQ(names(*c), (QStringList{"d.jpg", "e.jpg"}));
    EXPECT_EQ(c->currentIndex(), 0);
    EXPECT_EQ(c->totalCount(), 5);
    // Another image (not current) rated out disappears at once.
    c->updateRating(path("e.jpg"), Rating());
    EXPECT_EQ(names(*c), QStringList{"d.jpg"});
    EXPECT_EQ(c->currentPath(), path("d.jpg"));
    // A hidden image that matches again comes back at its sorted place.
    c->updateRating(path("c.jpg"), stars(2));
    EXPECT_EQ(names(*c), (QStringList{"c.jpg", "d.jpg"}));
    EXPECT_EQ(c->currentPath(), path("d.jpg"));
    // Removed files leave the hidden set too.
    c->removePath(path("b.jpg"));
    EXPECT_EQ(c->totalCount(), 4);
}
