// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): what a window does differently while it shows the images inside an archive
// or PDF (Collection::openContainer, VirtualPath.h).
//  - The images are opened for viewing only: actions that change, move or hand on the file
//    (trash, rename, quick move/copy, ratings, saving the rotation, editor, drag out,
//    wallpaper, Quick Look, custom actions …) are refused with a clear message instead of
//    failing on a path that is not a file.
//  - Comic archives (cbz/cbr/cb7) can be read right to left (key::ArchiveRightToLeft): the
//    arrow keys and swipes change direction; Space, Page Down and the wheel keep going to
//    the next page.

#include "InputBindings.h"

#include <QString>

namespace lf::containers {

// Actions refused for a member path (see above).
bool isBlocked(Action action);
// The message for a refused action (user facing).
QString blockedMessage();
// `action` for `event` with right-to-left reading: NextImage <-> PreviousImage for ←/→ keys
// and horizontal swipes; anything else unchanged.
Action readingDirection(Action action, const InputEvent& event, bool rightToLeft);

} // namespace lf::containers
