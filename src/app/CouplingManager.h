// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Coupled windows (F-WIN-07, like nomacs' synchronization): one group of viewer windows
// that flip together - by position or by file name (Settings key::CoupleBy, also in the
// dialog) - and share zoom and visible part (ViewSyncBinding; ⌥ moves one window on its
// own). "Fenster koppeln …" (Fenster menu) opens a dialog to pick the windows; "Entkoppeln"
// ends it. Coupled windows show "Gekoppelt" in the status line. Logic of the flip mapping:
// core/WindowCoupling.h.

#include "core/WindowCoupling.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <QDialog>

namespace lf {

class Application;
class ViewSyncBinding;
class ViewerWindow;

class CouplingManager : public QObject {
    Q_OBJECT
public:
    // One per Application (created on first use, child of it).
    static CouplingManager& of(Application& app);
    explicit CouplingManager(Application& app);
    ~CouplingManager() override;

    // Couples these windows (at least two; replaces an existing group).
    bool couple(const QList<ViewerWindow*>& windows);
    void uncouple();
    bool isCoupled(const ViewerWindow* window) const;
    QList<ViewerWindow*> group() const;
    CoupleBy coupleBy() const;

    // "Fenster koppeln …": checkboxes for the open windows (`front` preselected).
    void showDialog(ViewerWindow* front);
    QDialog* dialog() const { return m_dialog; }

signals:
    void changed();

private:
    void onCurrentChanged(ViewerWindow* source);
    void prune();
    void notifyWindows();

    Application& m_app;
    QList<QPointer<ViewerWindow>> m_group;
    ViewSyncBinding* m_views = nullptr;
    bool m_flipping = false;
    QPointer<QDialog> m_dialog;
};

} // namespace lf
