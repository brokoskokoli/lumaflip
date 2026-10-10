// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-CULL-05 "Alle verworfenen in den Papierkorb / nach _Rejected/ verschieben": which files
// the bulk action works on. Pure logic (no file access).
//
// - Only the images the window shows under its current filters (name filter F-NAV-08,
//   rating filter F-CULL-04) count; the confirmation names that count.
// - A shown image that is rejected takes the other file of its RAW+JPEG pair along
//   (F-FMT-06: one unit, FileBatch).
// - Moving into "_Rejected/": images that already are inside a "_Rejected" folder (a
//   recursive collection shows them, F-OPEN-08) are left out, so "_Rejected/_Rejected" is
//   never created.

#include "FileBatch.h"
#include "Rating.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

namespace lf::bulkrejects {

inline constexpr auto kRejectedFolderName = "_Rejected";

// "<folder>/_Rejected" for an image in <folder>; empty if the image already lies inside a
// "_Rejected" folder (any parent folder of that name, compared case-insensitively).
QString rejectedFolder(const QString& imagePath);

struct Plan {
    QVector<TransferItem> items; // pair members together, the shown file last
    int images = 0;              // shown images (a pair counts once)
};
// `shown`: the shown images in order as (path, pairedPath - empty if not paired);
// `ratings`: their ratings (missing = unrated).
Plan plan(const QVector<QPair<QString, QString>>& shown, const QHash<QString, Rating>& ratings, bool toTrash);

} // namespace lf::bulkrejects
