// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M4 features of one viewer window:
// - F-OPEN-10 archives and PDF: refuses file operations on images inside a container
//   (core/ContainerPolicy.h), "Im Finder zeigen" shows the archive, right-to-left reading of
//   comic archives (key::ArchiveRightToLeft, Action::ToggleRightToLeft);
// - F-MAC-06 Live Text / Visual Look Up (⇧⌘L, Darstellung → Live Text): on demand for the
//   shown image, cancelled when flipping (core/LiveText.h, LiveTextOverlay.h);
// - F-MAC-07 Quick Look (⌘Y) and the window's part of the Services menu (MacIntegration.h);
// - F-FILE-09 "Als Schreibtischhintergrund festlegen …" (asks first: this screen / all);
// - F-FILE-10 custom actions on the current file (CustomActionsUi.h).

#include "core/InputBindings.h"

#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

namespace lf {

class Application;
class LiveTextOverlay;
class LiveTextSession;
class ViewerWindow;

class WindowIntegration : public QObject {
    Q_OBJECT
public:
    WindowIntegration(ViewerWindow& window, Application& app);
    ~WindowIntegration() override;

    static WindowIntegration* of(ViewerWindow* window); // null if none

    // M4 actions and the container guard; true: handled (also when refused with a message).
    bool perform(Action action);
    // Right-to-left reading of comic archives for ←/→ and swipes.
    Action adjustInput(Action action, const InputEvent& event) const;
    bool rightToLeftActive() const; // a comic archive is shown with key::ArchiveRightToLeft

    // The window shows another file / a better stage of it.
    void currentChanged(const QString& path);
    void imageShown(const QString& path);

    // Live Text (F-MAC-06)
    LiveTextSession* liveText() const { return m_session; }
    bool liveTextActive() const;
    static bool liveTextAvailable(); // VisionKit present and supported

    // F-FILE-09: the confirmation (null unless shown; tests).
    QMessageBox* wallpaperQuestion() const { return m_wallpaperBox; }
    // Starts setting the wallpaper after the confirmation (tests call it directly).
    void setWallpaper(bool allScreens);

    QString statusText() const; // Live Text state for the status line, else empty

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool refuseInContainer(Action action);
    void installServices(); // F-MAC-07 responder, once the native window exists
    void toggleLiveText();
    void updateLiveTextImage();
    void updateLiveTextBlocking();
    void updateLiveTextOverlay(); // native view shown only over the visible image view
    void askWallpaper();
    void quickLook();

    ViewerWindow& m_window;
    Application& m_app;
    LiveTextSession* m_session = nullptr;
    LiveTextOverlay* m_overlay = nullptr;
    QPointer<QMessageBox> m_wallpaperBox;
    bool m_servicesInstalled = false;
    bool m_liveTextLoading = false; // the bridge library loads on a worker (first use)
};

} // namespace lf
