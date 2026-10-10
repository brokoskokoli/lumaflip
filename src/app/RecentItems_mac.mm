// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-11: system recent documents (Dock menu, Finder "Zuletzt benutzt").

#include "RecentItems.h"

#import <AppKit/AppKit.h>

namespace lf {

bool runsAsAppBundle() {
    @autoreleasepool {
        return [[[NSBundle mainBundle] bundleURL].pathExtension isEqualToString:@"app"];
    }
}

void noteSystemRecentDocument(const QString& path) {
    @autoreleasepool {
        // Cheap (no file system access of our own); AppKit stores a bookmark asynchronously.
        [[NSDocumentController sharedDocumentController]
            noteNewRecentDocumentURL:[NSURL fileURLWithPath:path.toNSString()]];
    }
}

void clearSystemRecentDocuments() {
    @autoreleasepool {
        [[NSDocumentController sharedDocumentController] clearRecentDocuments:nil];
    }
}

} // namespace lf
