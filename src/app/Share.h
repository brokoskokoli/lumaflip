// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-06: macOS share sheet. "Teilen …" shows NSSharingServicePicker next to the image;
// the File menu also lists the sharing services (Ablage → Teilen) directly.
//
// Asking the system for the services of a file and rendering their icons takes a moment
// (first query after login, many share extensions), so the list is computed on a worker
// thread when it is first needed and cached per file type (UTType); the menu shows a
// placeholder ("Wird geladen …") until it arrives and is filled then.

#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <optional>

class QMenu;
class QWidget;

namespace lf {

struct ShareService {
    QString title;
    QImage icon;
};

// The items handed to the sharing services: file URLs (the services read the files).
QList<QUrl> shareItems(const QStringList& paths);

// Platform part (Share_mac.mm).
// Services that can share `paths` (AirDrop, Mail, Nachrichten …), in system order, with
// their icons. Blocking: ShareServiceCache calls it on a worker thread.
QList<ShareService> shareServices(const QStringList& paths);
// The file type the services depend on: the UTType identifier of the file name
// extension ("public.jpeg"), or the lower-case suffix where there is none. No file access.
QString shareTypeKey(const QString& path);
// GUI thread: runs the service with that title; false if it is not available (any more).
bool performShareService(const QString& title, const QStringList& paths);
// Shows the share picker anchored at `rectInWindow` (coordinates of the top-level window
// of `anchor`). Non-blocking (a popover). False without a native window.
bool showSharePicker(QWidget* anchor, const QRect& rectInWindow, const QStringList& paths);

// The service lists, cached per type (GUI-thread object, owned by the application).
class ShareServiceCache : public QObject {
    Q_OBJECT
public:
    using Query = std::function<QList<ShareService>(const QStringList& paths)>;
    static ShareServiceCache& instance();

    std::optional<QList<ShareService>> cached(const QString& path) const;
    // Starts the query on a worker unless cached or already running; servicesReady()
    // follows on the GUI thread.
    void request(const QString& path);
    bool isQuerying(const QString& path) const;

    // Tests: replace the system query (also clears the cache).
    void setQueryForTesting(Query query);
    void clearCache();

signals:
    void servicesReady(const QString& typeKey);

private:
    explicit ShareServiceCache(QObject* parent = nullptr);
    struct Private;
    std::shared_ptr<Private> d;
};

// Fills the "Teilen" submenu: "Teilen …" (picker) and one entry per service - or a
// disabled "Wird geladen …" (object name "share:pending") while the services of that type
// are looked up; the menu is filled again when they arrive. Never blocks.
// `showPicker` runs for "Teilen …".
void populateShareMenu(QMenu* menu, const QStringList& paths, const std::function<void()>& showPicker);

} // namespace lf
