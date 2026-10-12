// SPDX-License-Identifier: GPL-3.0-or-later
#include "NavArrows.h"

namespace lf::navarrows {

EndBehavior endBehaviorFromId(const QString& id) {
    if (id == QLatin1String("wrap"))
        return EndBehavior::Wrap;
    if (id == QLatin1String("nextFolder"))
        return EndBehavior::NextFolder;
    return EndBehavior::Stop;
}

bool allowed(const Context& c) {
    return c.enabled && c.hasImage && c.viewShown && !c.cropping && !c.loupe && !c.slideshow && !c.liveText;
}

bool canStep(const Position& p, int delta) {
    if (p.count <= 0 || delta == 0)
        return false;
    if (p.end == EndBehavior::NextFolder && p.canSwitchFolder)
        return true; // at the end it goes on to the neighbouring folder or archive
    if (p.count == 1)
        return false;
    if (p.shuffle || p.end == EndBehavior::Wrap)
        return true;
    if (p.index < 0)
        return false;
    return delta > 0 ? p.index < p.count - 1 : p.index > 0;
}

Buttons buttons(const Position& p) {
    Buttons b;
    if (p.rightToLeft) {
        b.left = Action::NextImage;
        b.right = Action::PreviousImage;
    }
    b.leftShown = canStep(p, b.left == Action::NextImage ? 1 : -1);
    b.rightShown = canStep(p, b.right == Action::NextImage ? 1 : -1);
    return b;
}

} // namespace lf::navarrows
