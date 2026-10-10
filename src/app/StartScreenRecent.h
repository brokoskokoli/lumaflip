// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-12: the recently opened items on the start screen (window without an image):
// up to kMaxShown files and folders mixed, most recent first (RecentItems order), each
// clickable. Icons by kind only (file/folder) - no file system access on the GUI thread;
// whether an item still exists is checked when it is clicked (ViewerWindow).

#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace lf {

class StartScreenRecent : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxShown = 8;

    explicit StartScreenRecent(QWidget* parent = nullptr);

    void refresh();                 // reads RecentItems (settings only)
    void setDark(bool dark);        // colors for the image background behind the start screen
    QStringList shownPaths() const; // in display order (tests)

signals:
    void openRequested(const QString& path);

private:
    QLabel* m_title = nullptr;
    QVBoxLayout* m_list = nullptr;
    bool m_dark = true;
};

} // namespace lf
