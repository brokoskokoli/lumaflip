// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-WIN-07 coupled windows, the part without widgets: which image a coupled window shows
// when another window of the group flips. By index (same position in each window's
// collection) or by file name (same name without extension, so "IMG_1.CR3" in one folder
// follows "IMG_1.jpg" in another; case, diacritics and Unicode normalization ignored).

#include <QString>
#include <QStringList>

namespace lf {

enum class CoupleBy { Index, Name };

namespace coupling {

CoupleBy fromId(const QString& id); // "index" | "name" (setting key::CoupleBy), default Name
QString toId(CoupleBy by);

// Comparison key of a file name for CoupleBy::Name: name without the last extension, folded.
QString matchKey(const QString& fileName);

// The index a follower shows after the leader went to `leaderIndex` (`leaderPath`).
// Index: the same position, clamped to the follower's last image. Name: the follower's
// image with the same name (an exact file name match wins over a match without the
// extension); -1 if there is none (the follower stays where it is). -1 for an empty follower.
int followerIndex(CoupleBy by, int leaderIndex, const QString& leaderPath, const QStringList& followerPaths);

} // namespace coupling

} // namespace lf
