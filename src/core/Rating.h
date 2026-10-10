// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Ratings and culling (SPEC 4.9, F-CULL-01..05): the value model, the filter, the mapping to
// Finder tags and the naming of XMP sidecar files. Pure logic, no file access; the storage
// itself lives in RatingStore (F-CULL-02).
//
// Storage format (F-CULL-02, interoperability):
//   - Stars go to xmp:Rating ("0".."5", namespace http://ns.adobe.com/xap/1.0/), read by
//     Lightroom, Bridge, Capture One, darktable, digiKam and Photo Mechanic.
//   - Reject is xmp:Rating="-1" (the Adobe/Bridge convention, also used by darktable and
//     digiKam). Stars given before the reject are kept in lumaflip:Stars, so that "U"
//     (reset the flag) restores them.
//   - Pick has no standard XMP property (Lightroom keeps flags in its catalog only):
//     lumaflip:Pick="1" in the Lumaflip namespace http://ns.lumaflip.app/xmp/1.0/
//     (reject additionally writes lumaflip:Pick="-1"). Other programs ignore it.
//   - xmp:Rating decides about reject when it disagrees with lumaflip:Pick (another program
//     changed the rating after Lumaflip wrote it).

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace lf {

enum class Action; // InputBindings.h

enum class PickFlag : qint8 { None = 0, Pick = 1, Reject = -1 };

struct Rating {
    int stars = 0; // 0..5
    PickFlag flag = PickFlag::None;

    bool isEmpty() const { return stars == 0 && flag == PickFlag::None; }
    bool isPick() const { return flag == PickFlag::Pick; }
    bool isReject() const { return flag == PickFlag::Reject; }
    friend bool operator==(const Rating&, const Rating&) = default;
};

// F-CULL-04: which images a window shows.
struct RatingFilter {
    enum class Kind { All, Picks, MinStars, Unrated, Rejects };
    Kind kind = Kind::All;
    int minStars = 1; // MinStars: 1..5

    bool isActive() const { return kind != Kind::All; }
    bool matches(const Rating& r) const;
    // Stable id for menus/settings: "all", "picks", "min3", "unrated", "rejects".
    QString id() const;
    static RatingFilter fromId(const QString& id);
    // German, for the status line: "≥ 3 ★", "nur Picks" …
    QString describe() const;
    friend bool operator==(const RatingFilter&, const RatingFilter&) = default;
};

// Finder tag names/colors used for ratings (F-CULL-02 c). Colors are the Finder label
// indices: 0 none, 1 gray, 2 green, 3 purple, 4 blue, 5 yellow, 6 red, 7 orange.
struct RatingTagNames {
    QString starSymbol = QStringLiteral("★"); // n stars = the symbol n times ("★★★")
    QString pick = QStringLiteral("Pick");
    QString reject = QStringLiteral("Reject");
    int starColor = 0;
    int pickColor = 2;
    int rejectColor = 6;
    friend bool operator==(const RatingTagNames&, const RatingTagNames&) = default;
};

namespace rating {

// Display: "★★★☆☆", with "Pick"/"Reject" appended; empty rating = "–".
QString describe(const Rating& r);
// Value for sorting by rating (F-SORT-02): rejects lowest, then stars, picks above
// non-picks with the same stars.
int sortValue(const Rating& r);

// ---- culling steps (F-CULL-01, F-CULL-03) ----
bool isRatingAction(Action a); // Rate0..Rate5, RatePick, RateReject, RateClear
// The rating after `a` (0..5 set the stars, P/X set the flag, U resets the flag).
Rating apply(const Rating& current, Action a);
struct Step {
    Rating rating;
    bool advance = false; // F-CULL-03: show the next image afterwards
};
Step cullingStep(const Rating& current, Action a, bool autoAdvance);

// ---- XMP sidecars (F-CULL-02 a) ----
// Adobe naming: "IMG_1.CR3" -> "IMG_1.xmp" (extension replaced).
QString sidecarPath(const QString& imagePath);
// darktable/digiKam naming: "IMG_1.CR3.xmp" (extension kept).
QString sidecarPathWithExtension(const QString& imagePath);
// Candidates in reading order (both namings; also the upper-case ".XMP").
QStringList sidecarCandidates(const QString& imagePath);

// ---- Finder tags (F-CULL-02 c) ----
// A raw tag entry is "Name" or "Name\n<color>" (as stored in _kMDItemUserTags).
QString tagName(const QString& rawTag);
int tagColor(const QString& rawTag);
QString rawTag(const QString& name, int color);
// The rating the tags describe; nullopt-like: `found` false if none of our tags is present.
Rating fromTags(const QStringList& rawTags, const RatingTagNames& names, bool* found = nullptr);
// `existing` with our rating tags replaced by the ones for `r` (other tags untouched, order
// kept, ours appended).
QStringList applyToTags(const QStringList& existing, const Rating& r, const RatingTagNames& names);

} // namespace rating

} // namespace lf

Q_DECLARE_METATYPE(lf::Rating)
