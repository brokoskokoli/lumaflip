// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keys during a slideshow (F-SLIDE-01, F-SLIDE-03), without widgets: Space pauses, 1–9 set
// the interval in seconds, and rating stays possible. Rating keys that cannot be confused
// with the interval digits (P / X / U and their user shortcuts) always rate; the digits rate
// only where rating keys are active anyway (culling mode or "Bewertungstasten: immer",
// SPEC 5.2) - then 0–5 rate and 6–9 still set the interval.

#include "InputBindings.h"

namespace lf::slideshowinput {

enum class Kind { None, TogglePause, SetInterval, Rate };

struct Result {
    Kind kind = Kind::None;
    int seconds = 0;              // SetInterval
    Action rating = Action::None; // Rate
};

// `ratingKeysActive`: culling mode, or the input slot "ratingKeys" is "always".
Result dispatch(const InputBindings& bindings, const InputEvent& event, bool ratingKeysActive);

} // namespace lf::slideshowinput
