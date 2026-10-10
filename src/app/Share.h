// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-06: macOS share sheet. "Teilen …" shows NSSharingServicePicker next to the image;
// the File menu also lists the sharing services (Ablage → Teilen) directly.

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QMenu;
class QWidget;

namespace lf {

struct ShareService {
    QString title;
    QImage icon;
};

// The items handed to the sharing services: file URLs (the services read the files).
QList<QUrl> shareItems(const QStringList& paths);

// Platform part (Share_mac.mm; GUI thread, AppKit).
// Services that can share `paths` (AirDrop, Mail, Nachrichten …), in system order.
QList<ShareService> shareServices(const QStringList& paths);
// Runs the service with that title; false if it is not available (any more).
bool performShareService(const QString& title, const QStringList& paths);
// Shows the share picker anchored at `rectInWindow` (coordinates of the top-level window
// of `anchor`). Non-blocking (a popover). False without a native window.
bool showSharePicker(QWidget* anchor, const QRect& rectInWindow, const QStringList& paths);

// Fills the "Teilen" submenu: "Teilen …" (picker) and one entry per service.
// `showPicker` runs for "Teilen …".
void populateShareMenu(QMenu* menu, const QStringList& paths, const std::function<void()>& showPicker);

} // namespace lf
