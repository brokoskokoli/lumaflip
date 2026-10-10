// SPDX-License-Identifier: GPL-3.0-or-later
// Finder-compatible name comparison (F-SORT-01): equivalent to -[NSString
// localizedStandardCompare:], i.e. case-insensitive, numeric, width-insensitive, forced
// ordering, localized with the current locale. Verified against NSString in
// tests/unit/test_name_collation_mac.mm.
//
// The locale is cached per collator (fetching it per comparison would cost) and re-fetched
// after NSCurrentLocaleDidChangeNotification, so a language/region change in System
// Settings applies to the next sort without restarting the app.

#include "MacNameCollator.h"
#include "core/Platform.h"

#include <QMutex>
#include <atomic>
#include <mutex>
#include <vector>

#import <Foundation/Foundation.h>

namespace lf {
namespace {

std::atomic<quint64> gLocaleGeneration{1};
std::atomic<int> gLocaleFetches{0};

void observeLocaleChanges() {
    static std::once_flag once;
    std::call_once(once, [] {
        // queue:nil -> runs synchronously on the posting thread; only bumps a counter.
        [[NSNotificationCenter defaultCenter] addObserverForName:NSCurrentLocaleDidChangeNotification
                                                          object:nil
                                                           queue:nil
                                                      usingBlock:^(NSNotification*) {
                                                        gLocaleGeneration.fetch_add(1);
                                                      }];
    });
}

class MacNameCollator final : public NameCollator {
  public:
    MacNameCollator() { observeLocaleChanges(); }
    ~MacNameCollator() override {
        for (CFLocaleRef l : retired_)
            CFRelease(l);
        if (CFLocaleRef l = locale_.load())
            CFRelease(l);
    }
    MacNameCollator(const MacNameCollator&) = delete;
    MacNameCollator& operator=(const MacNameCollator&) = delete;

    int compare(const QString& a, const QString& b) const override {
        CFLocaleRef locale = currentLocale();
        // No-copy CFStrings over the QString buffers: cheap enough for large sorts.
        CFStringRef sa = CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(a.utf16()), a.size(), kCFAllocatorNull);
        CFStringRef sb = CFStringCreateWithCharactersNoCopy(
            kCFAllocatorDefault, reinterpret_cast<const UniChar*>(b.utf16()), b.size(), kCFAllocatorNull);
        const int result = compareStrings(sa, sb, locale, a, b);
        if (sa)
            CFRelease(sa);
        if (sb)
            CFRelease(sb);
        return result;
    }

    // The comparison (F-SORT-01): localizedStandardCompare: semantics.
    static int compareStrings(CFStringRef sa, CFStringRef sb, CFLocaleRef locale, const QString& a,
                              const QString& b) {
        int result = 0;
        if (sa && sb) {
            const CFStringCompareFlags flags = kCFCompareCaseInsensitive | kCFCompareNumerically |
                                               kCFCompareWidthInsensitive | kCFCompareForcedOrdering |
                                               kCFCompareLocalized;
            result = int(CFStringCompareWithOptionsAndLocale(sa, sb, CFRangeMake(0, CFStringGetLength(sa)),
                                                             flags, locale));
        } else {
            result = a.compare(b);
        }
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }

    // P-05: one CFString per name for the whole sort instead of two per comparison. The
    // comparison itself is the same call as compare(), so the order is identical.
    std::unique_ptr<PreparedNames> prepare(QVector<QString> names) const override;

private:
    // Cached locale, refreshed when the generation changed. Superseded locales are kept
    // until destruction because another thread may still compare with them (locale
    // changes are rare, so this is a few objects at most).
    CFLocaleRef currentLocale() const {
        const quint64 gen = gLocaleGeneration.load(std::memory_order_acquire);
        if (gen == generation_.load(std::memory_order_acquire))
            return locale_.load(std::memory_order_acquire);
        QMutexLocker lock(&mutex_);
        if (gen != generation_.load(std::memory_order_relaxed)) {
            CFLocaleRef fresh = CFLocaleCopyCurrent();
            ++gLocaleFetches;
            if (CFLocaleRef old = locale_.exchange(fresh))
                retired_.push_back(old);
            generation_.store(gen, std::memory_order_release);
        }
        return locale_.load(std::memory_order_acquire);
    }

    mutable QMutex mutex_;
    mutable std::atomic<CFLocaleRef> locale_{nullptr};
    mutable std::atomic<quint64> generation_{0};
    mutable std::vector<CFLocaleRef> retired_;
};

// Names of one sort with their CFStrings (no-copy views of the QString buffers, which
// this object owns) and the locale of that moment.
class MacPreparedNames final : public PreparedNames {
public:
    MacPreparedNames(QVector<QString> names, CFLocaleRef locale)
        : m_names(std::move(names)), m_locale(locale) {
        if (m_locale)
            CFRetain(m_locale);
        m_strings.reserve(size_t(m_names.size()));
        for (const QString& s : std::as_const(m_names))
            m_strings.push_back(CFStringCreateWithCharactersNoCopy(
                kCFAllocatorDefault, reinterpret_cast<const UniChar*>(s.utf16()), s.size(),
                kCFAllocatorNull));
    }
    ~MacPreparedNames() override {
        for (CFStringRef s : m_strings)
            if (s)
                CFRelease(s);
        if (m_locale)
            CFRelease(m_locale);
    }
    MacPreparedNames(const MacPreparedNames&) = delete;
    MacPreparedNames& operator=(const MacPreparedNames&) = delete;

    int compare(qsizetype i, qsizetype j) const override {
        return MacNameCollator::compareStrings(m_strings[size_t(i)], m_strings[size_t(j)], m_locale,
                                               m_names[i], m_names[j]);
    }

private:
    QVector<QString> m_names; // must not be modified: the CFStrings point into its buffers
    CFLocaleRef m_locale;
    std::vector<CFStringRef> m_strings;
};

std::unique_ptr<PreparedNames> MacNameCollator::prepare(QVector<QString> names) const {
    return std::make_unique<MacPreparedNames>(std::move(names), currentLocale());
}

} // namespace

std::unique_ptr<NameCollator> createNameCollator() {
    return std::make_unique<MacNameCollator>();
}

int mac::nameCollatorLocaleFetches() {
    return gLocaleFetches.load();
}

} // namespace lf
