// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-NAV-06 in a viewer window: "Gehe zu Bild …" (⌘G, a small window-modal dialog: a
// position "23", or "+5" / "-3" relative to the current image) and the shuffled browsing
// order ("Zufällige Reihenfolge", per window, one seed per session so the order is the same
// every time it is switched on; the status line shows "Zufällig"). Logic in
// core/NavigationOrder.h.

#include "core/NavigationOrder.h"

#include <QObject>
#include <QPointer>

#include <QDialog>
class QLineEdit;

namespace lf {

class Application;
class ViewerWindow;

class NavigationExtras : public QObject {
    Q_OBJECT
public:
    NavigationExtras(ViewerWindow& window, Application& app);

    // ⌘G: opens the dialog (non-blocking); goTo() is what it does on OK.
    void showGoToDialog();
    QDialog* goToDialog() const { return m_dialog; }
    bool goTo(const QString& text); // false (with a message) for an invalid entry

    bool shuffle() const { return m_shuffle; }
    void setShuffle(bool on);
    const ShuffleOrder& order() const { return m_order; }
    // Flipping in shuffled order (NextImage, PreviousImage, jumps, first/last); false if
    // shuffle is off or the action is no flipping action.
    bool navigate(int delta);
    bool goFirst(bool last);

signals:
    void shuffleChanged(bool on);

private:
    void updatePaths();

    ViewerWindow& m_window;
    Application& m_app;
    bool m_shuffle = false;
    ShuffleOrder m_order;
    QPointer<QDialog> m_dialog;
};

} // namespace lf
