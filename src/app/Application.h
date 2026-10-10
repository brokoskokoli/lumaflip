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

class QWidget;

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
    // The key window menu commands go to; default QApplication::activeWindow().
    std::function<QWidget*()> activeWindow;
    // Ends the event loop after quit(); default QCoreApplication::quit (tests record it).
    std::function<void()> quitApp;
    // Tests: sees every action before a window or the application runs it (menu, keys,
    // mouse); returning true swallows it.
    std::function<bool(ViewerWindow* window, Action action)> interceptAction;
};

// Paths of an open request after looking at the file system (on a worker thread).
struct ClassifiedPaths {
    QStringList files;   // absolute
    QStringList folders; // absolute
    QStringList missing; // absolute
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

    // main(): opens the paths given on the command line (one request). Otherwise, if no
    // open request arrives within kEmptyStartDelayMs (plain launch), the last session is
    // restored (F-WIN-06) or an empty window appears; an open request cancels both.
    void start(const QStringList& arguments);
    bool isRestorePending() const; // the last session is about to come back

    // Opens paths as one request according to the open behavior (F-WIN-02).
    // Files go to one window (one file = its folder, several = exactly those),
    // every folder gets its own window. Paths with an image suffix are opened at once
    // (P-01); everything else is classified on a worker thread first (no file system
    // access on the GUI thread) and opened when that is done. Returns the window that
    // got the image files, or nullptr if everything opens asynchronously.
    ViewerWindow* openPaths(const QStringList& paths);
    // One OS open request (QFileOpenEvent, Dock, Finder). The first path of a burst
    // is shown immediately (P-01); further paths within kOpenGroupingMs are merged
    // into the same window (F-OPEN-04).
    void queueOpen(const QString& path);
    // Looks at `paths` on a worker thread; `done` runs on the GUI thread (skipped if
    // `context` is destroyed).
    static void classifyPaths(const QStringList& paths, QObject* context,
                              const std::function<void(const ClassifiedPaths&)>& done);

    ViewerWindow* newWindow(); // ⌘N, empty window
    // F-OPEN-10: archives/PDFs opened together with other files: each in a window of its own
    // (like folders, F-WIN-02), never in the window asking.
    void openContainersInOwnWindows(const QStringList& containers);
    ViewerWindow* duplicateWindow(ViewerWindow* source); // ⌥⌘N
    void closeAllWindows();                              // ⌥⌘W
    QList<ViewerWindow*> windows() const;                // creation order
    ViewerWindow* frontWindow() const;                   // most recently active

    // Menu dispatch: window actions go to the front window. If another window
    // (preferences, a dialog) is the key window, ⌘W closes that one and image/window
    // commands are ignored.
    void perform(Action action);
    // Services::interceptAction (tests); true: swallowed.
    bool intercepted(ViewerWindow* window, Action action) const;
    void showOpenDialog();
    void showOpenFolderDialog();
    void showPreferences();
    void showShortcuts();
    // Dock icon clicked (applicationShouldHandleReopen): an empty window if none is open.
    void handleDockReopen();
    // F-MAC-04: asks first with a checklist of the types (non-blocking, DefaultAppDialog),
    // then setDefaultViewer() for the selected ones.
    void makeDefaultViewer();
    // Registers the app for the image types without blocking; the result is shown as a
    // message (and in the preferences window) and emitted as defaultViewerFinished().
    // `types` empty: the remembered selection (all types the user did not leave out).
    void setDefaultViewer(const QStringList& types = {});
    bool openUrl(const QUrl& url);    // via Services::openUrl
    static QUrl privacySettingsUrl(); // Datenschutz & Sicherheit -> Dateien und Ordner
    // ⌘Q: remembers the windows (F-WIN-06), then quits. Pending file operations are
    // finished first (busy cursor and message); the app quits when they are done.
    void quit();
    bool isQuitPending() const; // quit() waits for file operations

    // Tests (deterministic timing): the grouping interval of OS open requests (default
    // kOpenGroupingMs), whether a burst is being collected, and ending it at once (as if
    // the interval elapsed); the delay of the busy hint for file operations (default
    // 300 ms).
    void setOpenGroupingIntervalForTesting(int ms);
    bool isOpenBurstPending() const;
    void flushOpenBurstForTesting();
    void setBusyHintDelayForTesting(int ms);

    // F-WIN-06: windows with folder/files, current image, zoom mode and geometry.
    void saveSession();
    bool hasSession() const;
    void setSessionEnabled(bool enabled); // false: neither restored nor saved (benchmarks)

    QStringList recentPaths() const; // F-OPEN-11, see RecentItems
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
    // `oldTree`/`newTree` (F-OPEN-08): the folder is watched with all its subfolders.
    void setWindowFolder(ViewerWindow* window, const QString& oldFolder, const QString& newFolder,
                         bool oldTree = false, bool newTree = false);
    // A folder is opened from another window's copy (F-WIN-09): checks in the background
    // whether it changed without a watcher event (B12: network volumes) and reloads it in
    // every window if so.
    void revalidateFolder(const QString& folder);
    // The folder changed (watcher, revalidation): one new enumeration for all windows.
    void folderChangedOnDisk(const QString& folder);
    // Places a new window: last size, cascaded, never (nearly) on top of another (F-WIN-08).
    void placeWindow(ViewerWindow* window);
    // "Nicht gefunden: …" in `preferred`, the front window or a new window (F-OPEN-01).
    void showNotFound(const QStringList& missing, ViewerWindow* preferred = nullptr);

signals:
    void windowsChanged();
    void defaultViewerFinished(bool ok, const QString& message);
    // The registration takes long (macOS may wait for the user); the result still follows.
    void defaultViewerPending(const QString& message);
    void sessionRestored();
    // The UI language changed and the menu bar, preferences etc. were rebuilt (F-I18N-03).
    void languageChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ViewerWindow* createWindow();
    // `isFolder`: paths is one folder. `reusedOrCreated` tells whether the window was
    // created (or the empty start window was reused) for this request.
    // `newWindow`: never reuse the front window (archives opened together, F-OPEN-10).
    ViewerWindow* openInWindow(const QStringList& paths, bool isFolder, bool* reusedOrCreated = nullptr,
                               bool newWindow = false);
    void addRecent(const QStringList& files, const QStringList& folders);
    void flushOpenBurst();
    void openClassified(const ClassifiedPaths& c, ViewerWindow* preferred = nullptr);
    void restoreSession(); // checks the saved windows' files (worker)
    void applyRestore();   // creates the windows
    void cancelPendingRestore();
    void updateBusy(bool busy);
    void retranslate(); // F-I18N-03: after QEvent::LanguageChange
    QString busyText() const;

    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
