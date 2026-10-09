// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Application shell (SPEC 4.1 F-OPEN-01..05, 4.15 F-WIN-01..10, 4.12 F-MAC-01/03):
// window registry, open requests from the OS (QFileOpenEvent, grouped within
// 150 ms), command line, open behavior, shared services (one ImageProvider, one
// FileOps undo stack, one folder watcher for all windows) and the shared native
// menu bar. A plain QObject so tests can create it next to their own QApplication.

#include "core/InputBindings.h"

#include <QObject>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

namespace lf {

class FileOps;
class ImageProvider;
class MainMenu;
class Shell;
class ViewerWindow;

// Shared services; tests inject their own (e.g. a FileOps with a recording trash).
struct AppServices {
    ImageProvider* provider = nullptr; // default ImageProvider::instance()
    FileOps* fileOps = nullptr;        // default FileOps::instance()
    Shell* shell = nullptr;            // default createShell() (owned)
    // Opens System Settings URLs (F-PRIV-01); default QDesktopServices::openUrl.
    std::function<bool(const QUrl&)> openUrl;
};

class Application : public QObject {
    Q_OBJECT
public:
    using Services = AppServices;
    explicit Application(AppServices services = AppServices(), QObject* parent = nullptr);
    ~Application() override;
    static Application* instance(); // the most recently created one

    // F-OPEN-04: OS open requests arriving within this interval form one request.
    static constexpr int kOpenGroupingMs = 150;
    // Without any open request, the start window appears after this delay.
    static constexpr int kEmptyStartDelayMs = 350;

    // main(): opens the paths given on the command line (one request), otherwise
    // shows an empty window unless an open event arrives first.
    void start(const QStringList& arguments);

    // Opens paths as one request according to the open behavior (F-WIN-02).
    // Files go to one window (one file = its folder, several = exactly those),
    // every folder gets its own window. Returns the (last) window used.
    ViewerWindow* openPaths(const QStringList& paths);
    // One OS open request (QFileOpenEvent, Dock, Finder). The first path of a burst
    // is shown immediately (P-01); further paths within kOpenGroupingMs are merged
    // into the same window (F-OPEN-04).
    void queueOpen(const QString& path);

    ViewerWindow* newWindow();                           // ⌘N, empty window
    ViewerWindow* duplicateWindow(ViewerWindow* source); // ⌥⌘N
    void closeAllWindows();                              // ⌥⌘W
    QList<ViewerWindow*> windows() const;                // creation order
    ViewerWindow* frontWindow() const;                   // most recently active

    // Menu dispatch: window actions go to the front window.
    void perform(Action action);
    void showOpenDialog();
    void showOpenFolderDialog();
    void showPreferences();
    void showAbout();
    void showShortcuts();
    void makeDefaultViewer();
    bool openUrl(const QUrl& url);    // via Services::openUrl
    static QUrl privacySettingsUrl(); // Datenschutz & Sicherheit -> Dateien und Ordner

    QStringList recentPaths() const; // F-OPEN-11 (simple)
    void clearRecentPaths();

    ImageProvider& provider() const;
    FileOps& fileOps() const;
    Shell& shell() const;
    InputBindings& bindings() const;
    MainMenu* menu() const;

    // Called by windows.
    void registerWindow(ViewerWindow* window);
    void unregisterWindow(ViewerWindow* window);
    void windowActivated(ViewerWindow* window);
    void windowChanged(ViewerWindow* window); // title/state changed (menus)
    void setWindowFolder(ViewerWindow* window, const QString& oldFolder, const QString& newFolder);
    void noteTrashedBy(const QString& path, ViewerWindow* window);
    // Places a new window: last size, cascaded, never exactly on top of another (F-WIN-08).
    void placeWindow(ViewerWindow* window);

signals:
    void windowsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ViewerWindow* createWindow();
    ViewerWindow* openInWindow(const QStringList& paths);
    void addRecent(const QStringList& paths);
    void flushOpenBurst();

    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
