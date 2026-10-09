// SPDX-License-Identifier: GPL-3.0-or-later
// Finder-compatible name comparison (F-SORT-01): equivalent to -[NSString
// localizedStandardCompare:], i.e. case-insensitive, numeric, width-insensitive, forced
// ordering, localized with the current locale. Verified against NSString in
// tests/unit/test_name_collation_mac.mm.

#include "core/Platform.h"

#include <CoreFoundation/CoreFoundation.h>

namespace lf {
namespace {

class MacNameCollator final : public NameCollator {
  public:
    MacNameCollator() : locale_(CFLocaleCopyCurrent()) {}
    ~MacNameCollator() override {
        if (locale_)
            CFRelease(locale_);
    }
    MacNameCollator(const MacNameCollator&) = delete;
    MacNameCollator& operator=(const MacNameCollator&) = delete;

    int compare(const QString& a, const QString& b) const override {
        // No-copy CFStrings over the QString buffers: cheap enough for large sorts.
        CFStringRef sa = CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(a.utf16()), a.size(), kCFAllocatorNull);
        CFStringRef sb = CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(b.utf16()), b.size(), kCFAllocatorNull);
        int result = 0;
        if (sa && sb) {
            const CFStringCompareFlags flags = kCFCompareCaseInsensitive | kCFCompareNumerically |
                                               kCFCompareWidthInsensitive | kCFCompareForcedOrdering |
                                               kCFCompareLocalized;
            result = int(CFStringCompareWithOptionsAndLocale(sa, sb, CFRangeMake(0, CFStringGetLength(sa)),
                                                             flags, locale_));
        } else {
            result = a.compare(b);
        }
        if (sa)
            CFRelease(sa);
        if (sb)
            CFRelease(sb);
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }

  private:
    CFLocaleRef locale_;
};

} // namespace

std::unique_ptr<NameCollator> createNameCollator() {
    return std::make_unique<MacNameCollator>();
}

} // namespace lf
