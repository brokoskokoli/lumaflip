// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecentItems.h"

#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QStyle>

#include <optional>

namespace lf {

namespace {

const char* const kRecentKey = "recent/paths"; // folders end with '/'
std::optional<bool> g_systemOverride;

QString tr(const char* text) {
    return QCoreApplication::translate("lf::RecentItems", text);
}

QString clean(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

RecentItem decode(const QString& stored) {
    if (stored.size() > 1 && stored.endsWith(QLatin1Char('/')))
        return {stored.chopped(1), true};
    return {stored, false};
}

QString encode(const RecentItem& item) {
    return item.folder ? item.path + QLatin1Char('/') : item.path;
}

void store(const QList<RecentItem>& list) {
    QStringList out;
    for (const RecentItem& i : list)
        out << encode(i);
    Settings::instance().setValue(QString::fromLatin1(kRecentKey), out);
}

} // namespace

QList<RecentItem> RecentItems::items() {
    QList<RecentItem> out;
    for (const QString& s : Settings::instance().value(QString::fromLatin1(kRecentKey)).toStringList())
        if (!s.isEmpty())
            out << decode(s);
    return out;
}

QStringList RecentItems::paths() {
    QStringList out;
    for (const RecentItem& i : items())
        out << i.path;
    return out;
}

void RecentItems::add(const QStringList& files, const QStringList& folders) {
    QList<RecentItem> added;
    for (const QString& f : files)
        added << RecentItem{clean(f), false};
    for (const QString& f : folders)
        added << RecentItem{clean(f), true};
    if (added.isEmpty())
        return;
    QList<RecentItem> list = items();
    for (auto it = added.crbegin(); it != added.crend(); ++it) {
        list.removeIf([&](const RecentItem& r) { return r.path == it->path; });
        list.prepend(*it);
    }
    while (list.size() > kMaxItems)
        list.removeLast();
    store(list);
    if (systemRegistrationEnabled())
        for (const RecentItem& i : added)
            noteSystemRecentDocument(i.path);
}

void RecentItems::rename(const QString& from, const QString& to) {
    QList<RecentItem> list = items();
    bool changed = false;
    for (RecentItem& i : list) {
        if (i.path == from) {
            i.path = to;
            changed = true;
        }
    }
    if (changed)
        store(list);
}

void RecentItems::remove(const QString& path) {
    QList<RecentItem> list = items();
    const auto n = list.size();
    list.removeIf([&](const RecentItem& i) { return i.path == path; });
    if (list.size() != n)
        store(list);
}

QString RecentItems::settingsKey() {
    return QString::fromLatin1(kRecentKey);
}

void RecentItems::clear() {
    store({});
    if (systemRegistrationEnabled())
        clearSystemRecentDocuments();
}

QString RecentItems::menuTitle(const RecentItem& item, const QList<RecentItem>& all) {
    const QString name =
        QFileInfo(item.path).fileName().isEmpty() ? item.path : QFileInfo(item.path).fileName();
    int same = 0;
    for (const RecentItem& o : all)
        if (QFileInfo(o.path).fileName() == QFileInfo(item.path).fileName())
            ++same;
    if (same <= 1)
        return name;
    // "IMG_1.jpg — Urlaub" like the Finder's "Zuletzt benutzt" for equal names.
    return QStringLiteral("%1 — %2").arg(name, QFileInfo(QFileInfo(item.path).absolutePath()).fileName());
}

void RecentItems::populateMenu(QMenu* menu, const std::function<void(const QString&)>& open,
                               const std::function<void()>& clearAll, bool withIcons) {
    menu->clear();
    const QList<RecentItem> list = items();
    QStyle* style = withIcons && !list.isEmpty() ? QApplication::style() : nullptr;
    // Icons by kind only: no file system access on the GUI thread.
    const QIcon fileIcon = style ? style->standardIcon(QStyle::SP_FileIcon) : QIcon();
    const QIcon folderIcon = style ? style->standardIcon(QStyle::SP_DirIcon) : QIcon();
    for (const RecentItem& item : list) {
        QAction* act = menu->addAction(item.folder ? folderIcon : fileIcon, menuTitle(item, list));
        act->setToolTip(item.path);
        act->setData(item.path);
        const QString p = item.path;
        QObject::connect(act, &QAction::triggered, menu, [open, p] { open(p); });
    }
    if (!list.isEmpty())
        menu->addSeparator();
    QAction* clear = menu->addAction(tr("Einträge löschen"));
    clear->setObjectName(QStringLiteral("recent:clear"));
    clear->setEnabled(!list.isEmpty());
    QObject::connect(clear, &QAction::triggered, menu, [clearAll] { clearAll(); });
}

void RecentItems::setSystemRegistrationEnabled(bool enabled) {
    g_systemOverride = enabled;
}

bool RecentItems::systemRegistrationEnabled() {
    return g_systemOverride.value_or(runsAsAppBundle());
}

} // namespace lf
