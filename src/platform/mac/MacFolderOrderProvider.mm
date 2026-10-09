// SPDX-License-Identifier: GPL-3.0-or-later
// Finder sort order for a folder (SPEC 6.1):
//   tier 3 (opt-in, F-SORT-04): ask the front Finder window via osascript, 300 ms limit
//   tier 1: .DS_Store records (portable parser, core/FinderViewSettings)
//   tier 2: com.apple.finder preferences (CFPreferences)
//   tier 4: name ascending
// Thread-safe: called from worker threads, holds no mutable state.

#include "core/FinderViewSettings.h"
#include "core/Platform.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QVariantList>
#include <QVariantMap>

#include <CoreFoundation/CoreFoundation.h>

namespace lf {
namespace {

constexpr int kLiveQueryTimeoutMs = 300;

QVariant fromCF(CFTypeRef value, int depth = 0) {
    if (!value || depth > 32)
        return {};
    const CFTypeID t = CFGetTypeID(value);
    if (t == CFStringGetTypeID())
        return QString::fromCFString(static_cast<CFStringRef>(value));
    if (t == CFBooleanGetTypeID())
        return bool(CFBooleanGetValue(static_cast<CFBooleanRef>(value)));
    if (t == CFNumberGetTypeID()) {
        const auto n = static_cast<CFNumberRef>(value);
        if (CFNumberIsFloatType(n)) {
            double d = 0;
            CFNumberGetValue(n, kCFNumberDoubleType, &d);
            return d;
        }
        long long v = 0;
        CFNumberGetValue(n, kCFNumberLongLongType, &v);
        return qint64(v);
    }
    if (t == CFDataGetTypeID())
        return QByteArray::fromCFData(static_cast<CFDataRef>(value));
    if (t == CFDateGetTypeID())
        return QDateTime::fromCFDate(static_cast<CFDateRef>(value));
    if (t == CFArrayGetTypeID()) {
        const auto a = static_cast<CFArrayRef>(value);
        QVariantList list;
        const CFIndex n = CFArrayGetCount(a);
        for (CFIndex i = 0; i < n; ++i)
            list.append(fromCF(CFArrayGetValueAtIndex(a, i), depth + 1));
        return list;
    }
    if (t == CFDictionaryGetTypeID()) {
        const auto dict = static_cast<CFDictionaryRef>(value);
        const CFIndex n = CFDictionaryGetCount(dict);
        std::vector<const void*> keys(static_cast<size_t>(n)), values(static_cast<size_t>(n));
        CFDictionaryGetKeysAndValues(dict, keys.data(), values.data());
        QVariantMap map;
        for (CFIndex i = 0; i < n; ++i) {
            const QVariant k = fromCF(keys[size_t(i)], depth + 1);
            map.insert(k.toString(), fromCF(values[size_t(i)], depth + 1));
        }
        return map;
    }
    return {};
}

QVariantMap readFinderPrefs() {
    const CFStringRef app = CFSTR("com.apple.finder");
    CFPreferencesAppSynchronize(app); // pick up changes Finder made while we run
    QVariantMap prefs;
    for (const char* key : {"FXPreferredViewStyle", "StandardViewSettings", "StandardViewOptions",
                            "DesktopViewSettings", "ICloudViewSettings"}) {
        CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
        CFPropertyListRef v = CFPreferencesCopyAppValue(k, app);
        if (v) {
            prefs.insert(QString::fromLatin1(key), fromCF(v));
            CFRelease(v);
        }
        CFRelease(k);
    }
    return prefs;
}

QString specialKeyFor(const QString& folder) {
    const QString home = QDir::homePath();
    if (folder == home + QLatin1String("/Desktop"))
        return QStringLiteral("DesktopViewSettings");
    const QString icloud = home + QLatin1String("/Library/Mobile Documents/com~apple~CloudDocs");
    if (folder == icloud || folder.startsWith(icloud + QLatin1Char('/')))
        return QStringLiteral("ICloudViewSettings");
    return {};
}

// Tier 3. `is running` does not send Apple Events, so Finder is never launched; the
// Automation permission prompt can only appear because the user opted in (F-SORT-04).
const char* const kLiveScript = R"(if application "Finder" is not running then return ""
tell application "Finder"
    if (count of Finder windows) is 0 then return ""
    set w to Finder window 1
    set t to POSIX path of (target of w as alias)
    set v to current view of w
    if v is list view then
        set c to sort column of list view options of w
        return t & linefeed & "list" & linefeed & ((name of c) as text) & linefeed & ((sort direction of c) as text)
    else if v is icon view then
        return t & linefeed & "icon" & linefeed & ((arrangement of icon view options of w) as text)
    end if
    return t & linefeed & "other"
end tell)";

std::optional<SortSpec> liveQuery(const QString& folder) {
    QProcess p;
    p.setProgram(QStringLiteral("/usr/bin/osascript"));
    p.setArguments({QStringLiteral("-e"), QString::fromUtf8(kLiveScript)});
    QElapsedTimer timer;
    timer.start();
    p.start(QIODevice::ReadOnly);
    if (!p.waitForStarted(kLiveQueryTimeoutMs))
        return std::nullopt;
    const int left = int(std::max<qint64>(1, kLiveQueryTimeoutMs - timer.elapsed()));
    if (!p.waitForFinished(left)) {
        p.kill();
        p.waitForFinished(100);
        return std::nullopt;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return std::nullopt;
    return finder::parseLiveQueryOutput(QString::fromUtf8(p.readAllStandardOutput()), folder);
}

class MacFolderOrderProvider final : public FolderOrderProvider {
  public:
    SortSpec finderOrder(const QString& folderPath, bool allowLiveQuery) override {
        const QString folder = QDir::cleanPath(folderPath);
        if (allowLiveQuery) {
            if (auto live = liveQuery(folder))
                return *live;
        }
        finder::GlobalContext global;
        global.finderPrefs = readFinderPrefs();
        global.specialKey = specialKeyFor(folder);
        return finder::resolve(finder::readFolderRecords(folder), global);
    }

    QHash<QString, QPointF> iconPositions(const QString& folderPath) override {
        return finder::readIconPositions(folderPath);
    }
};

} // namespace

std::unique_ptr<FolderOrderProvider> createFolderOrderProvider() {
    return std::make_unique<MacFolderOrderProvider>();
}

} // namespace lf
