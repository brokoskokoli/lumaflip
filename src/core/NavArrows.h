// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-NAV-09: previous/next arrow buttons at the left and right edge of the image (the
// widgets are app/NavArrows). Pure decisions, testable without windows: in which state of
// the window the buttons may appear at all, which action each button runs (comics read
// right to left swap them like ←/→) and whether there is anything to go to in that
// direction.

#include "InputBindings.h"

namespace lf::navarrows {

// key::WrapMode (F-NAV-03 / F-IN-16)
enum class EndBehavior { Stop, Wrap, NextFolder };
EndBehavior endBehaviorFromId(const QString& id); // "stop" | "wrap" | "nextFolder"

// What the window shows and does right now.
struct Context {
    bool enabled = true;    // key::ShowNavArrows
    bool hasImage = false;  // a collection with at least one entry
    bool viewShown = false; // the single image view (not grid / compare)
    bool cropping = false;  // F-EDIT-03
    bool loupe = false;     // F-VIEW-11
    bool slideshow = false; // F-SLIDE-01
    bool liveText = false;  // F-MAC-06: native overlay above the image takes the mouse
};

// The buttons may appear (each one still needs a target, see Buttons).
bool allowed(const Context& c);

struct Position {
    int count = 0;
    int index = -1; // -1: none
    EndBehavior end = EndBehavior::Stop;
    // A folder or an archive/PDF: "nextFolder" can go on to a sibling (else it stops).
    bool canSwitchFolder = false;
    bool shuffle = false;     // F-NAV-06: shuffled order (its ends are not known here)
    bool rightToLeft = false; // F-OPEN-10: comic read right to left (← = next)
};

// Something to go to in the direction `delta` (+1 next, -1 previous) of the collection.
bool canStep(const Position& p, int delta);

struct Buttons {
    Action left = Action::PreviousImage;
    Action right = Action::NextImage;
    bool leftShown = false;
    bool rightShown = false;
};

// The action of each edge (like ← and →) and whether it has a target.
Buttons buttons(const Position& p);

} // namespace lf::navarrows
