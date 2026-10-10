// SPDX-License-Identifier: GPL-3.0-or-later
#include "MenuKeyEquivalents.h"

#import <AppKit/AppKit.h>

namespace lf {

namespace {

int fixMenu(NSMenu* menu) {
    int changed = 0;
    for (NSMenuItem* item in menu.itemArray) {
        if (item.submenu)
            changed += fixMenu(item.submenu);
        NSString* key = item.keyEquivalent;
        if (key.length != 1 || !(item.keyEquivalentModifierMask & NSEventModifierFlagShift))
            continue;
        const unichar c = [key characterAtIndex:0];
        if (![[NSCharacterSet lowercaseLetterCharacterSet] characterIsMember:c])
            continue;
        NSString* upper = key.uppercaseString;
        if (upper.length == 1 && ![upper isEqualToString:key]) {
            item.keyEquivalent = upper; // the Shift bit stays (shown as ⇧, matches ⇧ only)
            ++changed;
        }
    }
    return changed;
}

} // namespace

int fixShiftedMenuKeyEquivalents() {
    @autoreleasepool {
        if (NSApp == nil || NSApp.mainMenu == nil)
            return 0;
        return fixMenu(NSApp.mainMenu);
    }
}

void installMenuKeyEquivalentFix() {
    static id monitor = nil;
    if (monitor != nil || NSApp == nil)
        return;
    // Local monitors run inside -[NSApplication sendEvent:] before the event is dispatched,
    // i.e. before the window and the main menu look for a key equivalent.
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                    handler:^NSEvent*(NSEvent* event) {
                                                      fixShiftedMenuKeyEquivalents();
                                                      return event;
                                                    }];
}

} // namespace lf
