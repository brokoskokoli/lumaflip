// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-COL-02: the output color profile follows the window's screen. Qt reports a screen
// change (QWindow::screenChanged) only when the window moves to another QScreen; macOS
// additionally tells when the profile of the current display changes (System Settings →
// Displays → Color profile) or the window's color space changes for another reason.
// This watcher forwards both macOS notifications for one top-level window.

#include <QObject>

#include <functional>

class QWidget;

namespace lf {

class ScreenProfileWatcher : public QObject {
    Q_OBJECT
public:
    // `changed` runs on the GUI thread. Watches the native window of `window` (must exist).
    ScreenProfileWatcher(QWidget* window, std::function<void()> changed, QObject* parent = nullptr);
    ~ScreenProfileWatcher() override;
    bool isActive() const { return m_token != nullptr; }

    // Tests: posts the macOS "screen profile changed" notification for the window.
    static bool postProfileChangeForTesting(QWidget* window);

private:
    void* m_token = nullptr; // retained observer tokens (macOS)
};

} // namespace lf
