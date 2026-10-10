// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M3 collection features of a viewer window (owned by WindowExtras):
// - F-OPEN-08 "Unterordner einbeziehen": per window/session toggle, progress and the
//   subfolder of the current image in the status bar.
// - F-FMT-06 RAW+JPEG pairs: follows the setting, "RAW+JPEG" badge in the status bar,
//   "RAW anzeigen" switches to the other file of the pair.

#include "core/InputBindings.h"

#include <QObject>
#include <QStringList>

class QLabel;
class QStatusBar;

namespace lf {

class Application;
class ViewerWindow;

class CollectionExtras : public QObject {
    Q_OBJECT
public:
    CollectionExtras(ViewerWindow& window, Application& app);

    bool perform(Action action); // true: handled
    void attachStatusBar(QStatusBar* bar);
    void updateStatus();
    QStringList statusParts() const; // progress, subfolder, "RAW+JPEG"

    bool includesSubfolders() const;
    void setIncludeSubfolders(bool on);
    bool currentIsPair() const;   // the current image stands for a RAW+JPEG pair
    bool currentShowsRaw() const; // … and shows its RAW file
    void toggleRawJpeg();

    QString progressText() const { return m_progress; }
    QString subfolderText() const;
    QString pairText() const;

private:
    ViewerWindow& m_window;
    Application& m_app;
    QString m_progress; // while a recursive load runs
    QLabel* m_progressLabel = nullptr;
    QLabel* m_folderLabel = nullptr;
    QLabel* m_pairLabel = nullptr;
};

} // namespace lf
