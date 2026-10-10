// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-11: "Zuletzt geöffnet" - files and folders, newest first, at most kMaxItems,
// stored in the settings. Every addition is also registered with the system recent
// documents (macOS: NSDocumentController, shown in the Dock menu and in Finder), but only
// when running as an app bundle (tests never touch the user's list).

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QMenu;

namespace lf {

struct RecentItem {
    QString path;
    bool folder = false;
    friend bool operator==(const RecentItem&, const RecentItem&) = default;
};

class RecentItems {
public:
    static constexpr int kMaxItems = 20;

    static QList<RecentItem> items(); // newest first
    static QStringList paths();
    // The first of `files` ends up on top, then the rest of the files, then `folders`.
    static void add(const QStringList& files, const QStringList& folders = {});
    static void rename(const QString& from, const QString& to); // F-FILE-04 keeps the entry
    static void clear();

    // Menu text: the file/folder name; equal names get their parent folder appended.
    static QString menuTitle(const RecentItem& item, const QList<RecentItem>& all);
    // Fills `menu` with the entries and "Einträge löschen".
    static void populateMenu(QMenu* menu, const std::function<void(const QString&)>& open,
                             const std::function<void()>& clearAll);

    // System registration: by default only inside an .app bundle; tests can force it off.
    static void setSystemRegistrationEnabled(bool enabled);
    static bool systemRegistrationEnabled();
};

// Platform part (RecentItems_mac.mm; no-ops elsewhere).
void noteSystemRecentDocument(const QString& path);
void clearSystemRecentDocuments();
bool runsAsAppBundle();

} // namespace lf
