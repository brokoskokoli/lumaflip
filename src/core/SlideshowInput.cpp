// SPDX-License-Identifier: GPL-3.0-or-later
#include "SlideshowInput.h"

#include "Rating.h"

namespace lf::slideshowinput {

Result dispatch(const InputBindings& bindings, const InputEvent& event, bool ratingKeysActive) {
    Result r;
    if (event.kind != InputKind::Key)
        return r;
    const Qt::KeyboardModifiers mods = event.modifiers & ~Qt::KeypadModifier;
    const bool digit = event.key >= Qt::Key_0 && event.key <= Qt::Key_9;
    if (mods == Qt::NoModifier && event.key == Qt::Key_Space) {
        r.kind = Kind::TogglePause;
        return r;
    }
    // What the key would mean as a rating key (rating keys resolved first, 5.2).
    InputEvent asRating = event;
    asRating.inCullingMode = true;
    const Action a = bindings.resolve(asRating);
    const bool rating = rating::isRatingAction(a);
    // Plain digits belong to the interval unless rating keys are active.
    if (rating && (ratingKeysActive || !(digit && mods == Qt::NoModifier))) {
        r.kind = Kind::Rate;
        r.rating = a;
        return r;
    }
    if (mods == Qt::NoModifier && digit && event.key != Qt::Key_0) {
        r.kind = Kind::SetInterval;
        r.seconds = event.key - Qt::Key_0;
    }
    return r;
}

} // namespace lf::slideshowinput
