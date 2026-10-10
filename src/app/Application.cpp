// SPDX-License-Identifier: GPL-3.0-or-later
#include "Application.h"

#include "DefaultAppDialog.h"
#include "DockReopen.h"
#include "EdrDisplay.h"
#include "ImageView.h"
#include "MainMenu.h"
#include "MenuKeyEquivalents.h"
#include "PreferencesDialog.h"
#include "RecentItems.h"
#include "Translations.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderCache.h"
#include "core/ImageFormats.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/StartupTrace.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QFutureWatcher>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

namespace {

constexpr int kCascadePx = 26;   // F-WIN-08
constexpr int kTakenPx = 10;     // windows closer than this count as "on top of each other"
constexpr int kBusyHintMs = 300; // file operation running longer: busy cursor + message
const char* const kGeometryKey = "window/geometry";
const char* const kSessionKey = "session/windows"; // F-WIN-06

Application* s_instance = nullptr;

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta || key == Qt::Key_Alt ||
           key == Qt::Key_CapsLock || key == Qt::Key_unknown || key == 0;
}

// Absolute, clean path - string operations only (no file system access).
QString absPath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString normalized(const QString& path) {
    return absPath(path).normalized(QString::NormalizationForm_C);
}

QStringList minus(const QStringList& a, const QStringList& b) {
    QStringList out;
    for (const QString& s : a)
        if (!b.contains(s))
            out << s;
    return out;
}

} // namespace

struct Application::Private {
    AppServices services;
    std::unique_ptr<Shell> ownShell;
    std::unique_ptr<InputBindings> bindings;
    std::unique_ptr<FolderWatcher> watcher;
    MainMenu* menu = nullptr;
    QHash<QString, int> watchCount;           // folder -> number of windows showing it
    QList<QPointer<ViewerWindow>> windows;    // open windows
    QList<QPointer<ViewerWindow>> allWindows; // incl. closed ones awaiting deferred deletion
    QPointer<ViewerWindow> front;
    // Open burst (F-OPEN-04)
    QTimer burstTimer;
    QStringList burstPaths;
    QPointer<ViewerWindow> burstWindow;
    bool burstWindowOwned = false; // created (or the empty start window) for this burst
    bool burstFirstShown = false;  // the first path of the burst was opened right away
    QTimer menuTimer;
    QTimer busyTimer;
    bool busyCursor = false;
    QPointer<ViewerWindow> busyWindow; // shows the "file operation running" message
    QPointer<QDialog> shortcuts;
    bool started = false;      // start() ran (Dock reopen handling)
    bool startPending = false; // start() still waits for an open event / the session
    bool quitting = false;
    bool quitWhenIdle = false; // quit requested while file operations were pending
    bool sessionEnabled = true;
    // F-WIN-06: the session is restored only for a plain launch, i.e. if no open request
    // arrives during the startup window (kEmptyStartDelayMs).
    enum class Restore { None, Classifying, Ready, Cancelled };
    Restore restore = Restore::None;
    QVariantList restoreList;        // the saved windows
    ClassifiedPaths restoreExisting; // what of them still exists
    bool startWindowElapsed = false;
    bool retranslatePending = false; // F-I18N-03
};

Application::Application(AppServices services, QObject* parent)
    : QObject(parent), d(std::make_unique<Private>()) {
    s_instance = this;
    d->services = std::move(services);
    if (!d->services.provider)
        d->services.provider = &ImageProvider::instance();
    if (!d->services.fileOps)
        d->services.fileOps = &FileOps::instance();
    if (!d->services.shell) {
        d->ownShell = createShell();
        d->services.shell = d->ownShell.get();
    }
    if (!d->services.openUrl)
        d->services.openUrl = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
    if (!d->services.activeWindow)
        d->services.activeWindow = [] { return QApplication::activeWindow(); };
    if (!d->services.quitApp)
        d->services.quitApp = [] { QCoreApplication::quit(); };
    trace::mark("Application: default services");
    d->bindings = std::make_unique<InputBindings>();
    trace::mark("Application: bindings");

    QGuiApplication::setQuitOnLastWindowClosed(false); // macOS: the app stays, menu bar works
    qApp->installEventFilter(this);

    Settings& st = Settings::instance();
    if (const int mb = st.value(key::MemoryBudgetMB).toInt(); mb > 0)
        provider().setMemoryBudget(qint64(mb) << 20);
    provider().setRawEmbeddedOnly(st.value(key::RawEmbeddedOnly).toBool());
    // F-COL-03: HDR content with its extended range when a display can show it (EDR).
    auto updateHdrDecoding = [this] {
        provider().setHdrDecoding(edrPresentationAvailable() && anyScreenSupportsEdr());
    };
    updateHdrDecoding();
    connect(qApp, &QGuiApplication::screenAdded, this, updateHdrDecoding);
    connect(qApp, &QGuiApplication::screenRemoved, this, updateHdrDecoding);
    trace::mark("Application: settings read");

    // One folder watcher for all windows; every folder is watched once (F-WIN-09, F-OPEN-07).
    // One change = one enumeration (FolderCache), then every window re-sorts its copy.
    d->watcher = createFolderWatcher(this);
    trace::mark("Application: folder watcher");
    connect(d->watcher.get(), &FolderWatcher::folderChanged, this, &Application::folderChangedOnDisk);

    // File operations of any window update every window (F-FILE-01). The shared folder
    // snapshot is outdated as well; the windows themselves are updated in place.
    FileOps& ops = fileOps();
    auto invalidateFolderOf = [](const QString& path) {
        FolderCache::instance().invalidate(QFileInfo(path).absolutePath());
    };
    connect(&ops, &FileOps::fileRemoved, this, [this, invalidateFolderOf](const QString& path) {
        provider().invalidate(path);
        invalidateFolderOf(path);
        for (ViewerWindow* w : windows())
            w->fileRemoved(path);
    });
    connect(&ops, &FileOps::fileAdded, this, [this, invalidateFolderOf](const QString& path) {
        provider().invalidate(path);
        invalidateFolderOf(path);
        for (ViewerWindow* w : windows())
            w->fileAdded(path, false);
    });
    // Undo of a trash: the window that trashed the file shows it again (F-FILE-01). The
    // origin travels with the undo command, so it is right after undo/redo/undo too.
    connect(&ops, &FileOps::fileRestored, this, [this](const QString& path, QObject* origin) {
        for (ViewerWindow* w : windows())
            if (w == origin)
                w->fileAdded(path, true);
    });
    connect(&ops, &FileOps::fileRenamed, this,
            [this, invalidateFolderOf](const QString& from, const QString& to) {
                provider().invalidate(from);
                provider().invalidate(to);
                invalidateFolderOf(from);
                invalidateFolderOf(to);
                for (ViewerWindow* w : windows())
                    w->fileRenamed(from, to);
                RecentItems::rename(from, to); // F-OPEN-11 entry follows F-FILE-04
            });
    connect(&ops, &FileOps::operationFailed, this, [this](const QString& message) {
        if (auto* w = frontWindow())
            w->showMessage(message, 5000);
        else
            QMessageBox::warning(nullptr, tr("Lumaflip"), message);
    });
    d->busyTimer.setSingleShot(true);
    d->busyTimer.setInterval(kBusyHintMs);
    connect(&d->busyTimer, &QTimer::timeout, this, [this] { updateBusy(true); });
    connect(&ops, &FileOps::busyChanged, this, [this](bool busy) {
        if (busy) {
            d->busyTimer.start();
            return;
        }
        updateBusy(false);
        if (d->quitWhenIdle) // ⌘Q waited for the file operations (never dropped)
            QTimer::singleShot(0, this, &Application::quit);
    });

    connect(&st, &Settings::changed, this, [this](const QString& k) {
        if (k.startsWith(QLatin1String(key::InputPrefix)) || k == QLatin1String(key::WrapMode)) {
            d->bindings->reload();
            d->menu->refreshShortcuts();
        } else if (k == QLatin1String(key::MemoryBudgetMB)) {
            const int mb = Settings::instance().value(key::MemoryBudgetMB).toInt();
            if (mb > 0)
                provider().setMemoryBudget(qint64(mb) << 20);
        } else if (k == QLatin1String(key::RawEmbeddedOnly)) {
            provider().setRawEmbeddedOnly(Settings::instance().value(key::RawEmbeddedOnly).toBool());
        } else if (k == QLatin1String(key::Language)) {
            installTranslations(resolvedUiLanguage()); // F-I18N-03: LanguageChange follows
        }
        d->menu->refreshState();
    });

    // F-MAC-01: ⇧⌘-letter menu shortcuts must not answer the plain ⌘-letter (⌘R, ⌘F).
    installMenuKeyEquivalentFix();
    fixShiftedMenuKeyEquivalents();

    // Dock icon clicked (applicationShouldHandleReopen:hasVisibleWindows:, not every
    // activation such as ⌘-Tab): without any window, open an empty one like other
    // document apps.
    QPointer<Application> self(this);
    installDockReopenHandler([self] {
        if (self)
            self->handleDockReopen();
    });

    d->burstTimer.setSingleShot(true);
    d->burstTimer.setInterval(kOpenGroupingMs);
    connect(&d->burstTimer, &QTimer::timeout, this, &Application::flushOpenBurst);
    d->menuTimer.setSingleShot(true);
    d->menuTimer.setInterval(250);
    connect(&d->menuTimer, &QTimer::timeout, this, [this] {
        d->menu->refreshWindowList();
        d->menu->refreshState();
    });

    trace::mark("Application: services ready");
    d->menu = new MainMenu(*this);
    trace::mark("Application: menu created");
}

Application::~Application() {
    qApp->removeEventFilter(this);
    if (s_instance == this)
        installDockReopenHandler({});
    updateBusy(false);
    // Worker threads of the shell (default makeDefaultViewerAsync) use it.
    shell().waitForPendingCalls();
    const QList<QPointer<ViewerWindow>> list = d->allWindows;
    for (const auto& w : list)
        delete w.data(); // windows reference the services owned/used here
    if (d->shortcuts)
        delete d->shortcuts.data();
    delete d->menu;
    d->menu = nullptr;
    d->watcher.reset();
    if (s_instance == this)
        s_instance = nullptr;
}

Application* Application::instance() {
    return s_instance;
}

ImageProvider& Application::provider() const {
    return *d->services.provider;
}
FileOps& Application::fileOps() const {
    return *d->services.fileOps;
}
Shell& Application::shell() const {
    return *d->services.shell;
}
InputBindings& Application::bindings() const {
    return *d->bindings;
}
MainMenu* Application::menu() const {
    return d->menu;
}

void Application::updateBusy(bool busy) {
    if (!busy)
        d->busyTimer.stop();
    if (busy == d->busyCursor)
        return;
    d->busyCursor = busy;
    const QString text = busyText();
    if (busy) {
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
        d->busyWindow = frontWindow();
        if (d->busyWindow)
            d->busyWindow->showMessage(text, 60000);
    } else {
        QGuiApplication::restoreOverrideCursor();
        // Hidden where it was shown, even if another window came to the front meanwhile.
        if (ViewerWindow* w = d->busyWindow; w && w->toastText() == text)
            w->hideMessage();
        d->busyWindow = nullptr;
    }
}

QString Application::busyText() const {
    return d->quitWhenIdle ? tr("Dateivorgänge werden abgeschlossen, dann wird Lumaflip beendet …")
                           : tr("Dateivorgang läuft …");
}

// ---------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------

void Application::classifyPaths(const QStringList& paths, QObject* context,
                                std::function<void(const ClassifiedPaths&)> done) {
    QStringList absolute;
    for (const QString& p : paths)
        absolute << absPath(p);
    using Watcher = QFutureWatcher<ClassifiedPaths>;
    auto* w = new Watcher(context);
    QObject::connect(w, &Watcher::finished, context, [w, done] {
        const ClassifiedPaths r = w->result();
        w->deleteLater();
        done(r);
    });
    w->setFuture(QtConcurrent::run([absolute] {
        ClassifiedPaths r;
        for (const QString& p : absolute) {
            const QFileInfo fi(p);
            if (!fi.exists())
                r.missing << p;
            else if (fi.isDir())
                r.folders << p;
            else
                r.files << p;
        }
        return r;
    }));
}

void Application::start(const QStringList& arguments) {
    d->started = true;
    QStringList paths;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& a = arguments[i];
        if (a.startsWith(QLatin1Char('-')))
            continue; // -psn_…, -NSDocumentRevisionsDebugMode etc.
        paths << a;
    }
    if (!paths.isEmpty()) {
        openPaths(paths); // F-OPEN-01 command line: one request (missing ones are reported), no restore
        return;
    }
    // Launched by Finder/Dock the open request arrives as QFileOpenEvent right after
    // start. F-WIN-06: the last session comes back only for a plain launch, i.e. if no
    // open request arrives within the startup window; the opened image has priority
    // (P-01). Without a session the start window appears (F-OPEN-12, simple).
    d->startPending = true;
    d->startWindowElapsed = false;
    if (d->sessionEnabled && Settings::instance().value(key::RestoreWindows).toBool() && hasSession())
        restoreSession(); // looks at the files now, applies them after the startup window
    QTimer::singleShot(kEmptyStartDelayMs, this, [this] {
        d->startWindowElapsed = true;
        if (d->restore == Private::Restore::Classifying)
            return; // the restore finishes (and ends the start phase) when the files are checked
        if (d->restore == Private::Restore::Ready) {
            applyRestore();
            return;
        }
        d->startPending = false;
        if (windows().isEmpty() && !d->burstTimer.isActive())
            newWindow();
    });
}

void Application::cancelPendingRestore() {
    if (d->restore != Private::Restore::Classifying && d->restore != Private::Restore::Ready)
        return;
    d->restore = Private::Restore::Cancelled; // the saved session stays until the next save
    d->restoreList.clear();
    d->startPending = false;
}

bool Application::isRestorePending() const {
    return d->restore == Private::Restore::Classifying || d->restore == Private::Restore::Ready;
}

void Application::handleDockReopen() {
    if (!d->started || d->startPending || d->quitting || d->burstTimer.isActive() || !windows().isEmpty())
        return;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->isWindow() && !w->inherits("QMenu") && !w->inherits("QMenuBar"))
            return; // e.g. only the preferences window is open: macOS brings it forward
    newWindow();
}

void Application::queueOpen(const QString& path) {
    cancelPendingRestore(); // F-WIN-06: launched by an open request
    if (!d->burstTimer.isActive()) {
        // First request of a burst: an image is shown immediately (P-01); anything else
        // is looked at together with the rest of the burst (one request, F-OPEN-04).
        d->burstPaths = {path};
        d->burstWindow = nullptr;
        d->burstWindowOwned = false;
        d->burstFirstShown = false;
        if (hasImageSuffix(QFileInfo(path).fileName())) {
            d->burstFirstShown = true;
            bool owned = false;
            d->burstWindow = openInWindow({absPath(path)}, false, &owned);
            d->burstWindowOwned = owned;
            // Verified on a worker: a folder named like an image, or a missing file.
            const QString abs = absPath(path);
            QPointer<ViewerWindow> w = d->burstWindow;
            classifyPaths({abs}, this, [this, abs, w](const ClassifiedPaths& c) {
                if (!c.missing.isEmpty())
                    showNotFound(c.missing, w);
                else if (!c.folders.isEmpty() && w)
                    w->openFolder(abs);
                if (c.missing.isEmpty())
                    addRecent(c.files, c.folders);
            });
        }
        d->burstTimer.start();
        return;
    }
    d->burstPaths << path;
    d->burstTimer.start(); // keep grouping while requests keep coming
}

void Application::openClassified(const ClassifiedPaths& c, ViewerWindow* preferred) {
    // One request: the files in one window (one file = its folder, several = exactly
    // those, F-OPEN-04), every folder in a window of its own (F-WIN-02).
    if (!c.files.isEmpty())
        openInWindow(c.files, false);
    for (const QString& f : std::as_const(c.folders))
        openInWindow({f}, true);
    if (!c.missing.isEmpty())
        showNotFound(c.missing, preferred);
    addRecent(c.files, c.folders);
}

void Application::flushOpenBurst() {
    const QStringList all = d->burstPaths;
    d->burstPaths.clear();
    if (all.isEmpty())
        return;
    if (!d->burstFirstShown) {
        // Nothing was opened yet (the first path has no image suffix): the whole burst is
        // classified and opened as one request, each path exactly once.
        classifyPaths(all, this, [this](const ClassifiedPaths& c) { openClassified(c); });
        return;
    }
    if (all.size() <= 1)
        return;
    const QPointer<ViewerWindow> w = d->burstWindow;
    const bool owned = d->burstWindowOwned;
    classifyPaths(all, this, [this, all, w, owned](const ClassifiedPaths& c) {
        const QString first = absPath(all.first());
        const bool firstIsFolder = c.folders.contains(first);
        // F-OPEN-04: several files in one request -> exactly those, in the window that
        // was opened for the first one - but never in a window that already existed
        // (open behavior "new window unless open" brought that one to the front).
        if (!c.files.isEmpty() && !(c.files.size() == 1 && c.files.first() == first)) {
            if (w && owned && !firstIsFolder) {
                if (c.files.size() == 1)
                    w->openFile(c.files.first());
                else
                    w->openFiles(c.files);
            } else {
                openInWindow(c.files, false);
            }
        }
        for (const QString& f : std::as_const(c.folders))
            if (f != first)
                openInWindow({f}, true);
        const QStringList missing = minus(c.missing, {first}); // the first one was reported already
        if (!missing.isEmpty())
            showNotFound(missing, w);
        addRecent(c.files, c.folders);
    });
}

ViewerWindow* Application::openPaths(const QStringList& paths) {
    cancelPendingRestore(); // F-WIN-06: an open request wins over the last session
    // P-01: image files are opened right away, without touching the file system here.
    QStringList images;
    for (const QString& p : paths)
        if (hasImageSuffix(QFileInfo(p).fileName()))
            images << absPath(p);
    ViewerWindow* imageWindow = images.isEmpty() ? nullptr : openInWindow(images, false);
    const QPointer<ViewerWindow> w = imageWindow;
    classifyPaths(paths, this, [this, images, w](const ClassifiedPaths& c) {
        // Files without an image suffix (still tried, F-FMT-02 shows why they fail).
        const QStringList otherFiles = minus(c.files, images);
        if (!otherFiles.isEmpty())
            openInWindow(otherFiles, false);
        for (const QString& f : std::as_const(c.folders)) {
            if (images.contains(f) && w && images.size() == 1)
                w->openFolder(f); // a folder named like an image ("Fotos.png")
            else
                openInWindow({f}, true);
        }
        if (!c.missing.isEmpty())
            showNotFound(c.missing, w);
        addRecent(c.files, c.folders);
    });
    return imageWindow;
}

void Application::showNotFound(const QStringList& missing, ViewerWindow* preferred) {
    if (missing.isEmpty())
        return;
    QStringList names;
    for (const QString& m : missing)
        names << QFileInfo(m).fileName();
    const QString msg = tr("Nicht gefunden: %1").arg(names.join(QStringLiteral(", ")));
    ViewerWindow* w = preferred ? preferred : frontWindow();
    if (!w)
        w = newWindow(); // never lose the message (e.g. launched with a missing path)
    w->showMessage(msg, 6000);
}

ViewerWindow* Application::openInWindow(const QStringList& paths, bool isFolder, bool* reusedOrCreated) {
    if (reusedOrCreated)
        *reusedOrCreated = false;
    const QString behavior = Settings::instance().value(key::OpenBehavior).toString();
    if (behavior == QLatin1String("newUnlessOpen") && paths.size() == 1) {
        // F-WIN-02: bring the window to front that already shows this file/folder.
        const QString p = normalized(paths.first());
        for (ViewerWindow* w : windows()) {
            Collection* c = w->collection();
            const bool match =
                isFolder ? (c->isFolderCollection() && c->folderPath() == p) : c->indexOf(p) >= 0;
            if (!match)
                continue;
            if (!isFolder)
                c->setCurrentPath(p);
            if (w->isMinimized())
                w->showNormal();
            w->raise();
            w->activateWindow();
            windowActivated(w);
            return w;
        }
    }
    ViewerWindow* target = nullptr;
    ViewerWindow* front = frontWindow();
    bool reused = false;
    if (front && front->isEmpty()) {
        target = front; // the start window is reused
        reused = true;
    } else if (behavior == QLatin1String("replaceFront") && front) {
        target = front;
    }
    const bool fresh = target == nullptr;
    if (fresh)
        target = createWindow();
    trace::once("openInWindow: window ready");
    if (reusedOrCreated)
        *reusedOrCreated = fresh || reused;
    // Decoding starts before the window is shown (P-01).
    if (isFolder)
        target->openFolder(paths.first());
    else if (paths.size() == 1)
        target->openFile(paths.first());
    else
        target->openFiles(paths);
    trace::once("openInWindow: open started");
    if (fresh || !target->isVisible())
        target->show();
    trace::once("openInWindow: shown");
    target->raise();
    target->activateWindow();
    windowActivated(target);
    return target;
}

ViewerWindow* Application::createWindow() {
    auto* w = new ViewerWindow(*this); // registers itself
    placeWindow(w);
    return w;
}

ViewerWindow* Application::newWindow() {
    ViewerWindow* w = createWindow();
    w->show();
    w->raise();
    w->activateWindow();
    windowActivated(w);
    return w;
}

ViewerWindow* Application::duplicateWindow(ViewerWindow* source) {
    if (!source)
        return newWindow();
    ViewerWindow* w = createWindow();
    w->copyStateFrom(*source);
    w->show();
    w->copyViewFrom(*source); // zoom + visible part, once the view has its final size
    w->raise();
    w->activateWindow();
    windowActivated(w);
    return w;
}

void Application::closeAllWindows() {
    const QList<ViewerWindow*> list = windows();
    for (ViewerWindow* w : list)
        w->close();
}

QList<ViewerWindow*> Application::windows() const {
    QList<ViewerWindow*> out;
    for (const auto& w : d->windows)
        if (w)
            out << w.data();
    return out;
}

ViewerWindow* Application::frontWindow() const {
    if (d->front)
        return d->front;
    const QList<ViewerWindow*> list = windows();
    return list.isEmpty() ? nullptr : list.last();
}

void Application::placeWindow(ViewerWindow* window) {
    const QByteArray geometry = Settings::instance().value(QString::fromLatin1(kGeometryKey)).toByteArray();
    if (geometry.isEmpty() || !window->restoreGeometry(geometry)) {
        window->resize(1100, 760);
        if (QScreen* s = QGuiApplication::primaryScreen()) {
            const QRect avail = s->availableGeometry();
            window->move(avail.center() - QPoint(window->width() / 2, window->height() / 2));
        }
    }
    QList<ViewerWindow*> others = windows();
    others.removeAll(window);
    if (others.isEmpty())
        return; // F-MAC-05: the last position
    // F-WIN-08: cascade from the front window, never (nearly) on top of another one.
    ViewerWindow* ref = frontWindow();
    if (!ref || ref == window)
        ref = others.last();
    QScreen* screen = ref->screen() ? ref->screen() : QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QSize size = ref->size().boundedTo(avail.size() - QSize(4 * kCascadePx, 4 * kCascadePx));
    window->resize(size);
    // Frame (title bar) around the client area, for the "fits on the screen" check.
    const QSize frameExtra = (ref->frameGeometry().size() - ref->size()).expandedTo(QSize(0, 0));
    const QSize outer = size + frameExtra;
    auto taken = [&](QPoint p) {
        return std::any_of(others.cbegin(), others.cend(), [&](ViewerWindow* o) {
            const QPoint q = o->pos();
            return std::abs(q.x() - p.x()) < kTakenPx && std::abs(q.y() - p.y()) < kTakenPx;
        });
    };
    auto fits = [&](QPoint p) { return avail.contains(QRect(p, outer).adjusted(0, 0, -1, -1)); };
    const QPoint step(kCascadePx, kCascadePx);
    // Candidates: the diagonal continuing from the front window, then diagonals starting
    // at the top edge (shifted right step by step) and at the left edge (shifted down).
    QList<QPoint> candidates;
    for (QPoint p = ref->pos() + step; fits(p); p += step)
        candidates << p;
    auto diagonal = [&](QPoint start) {
        for (QPoint p = start; fits(p); p += step)
            candidates << p;
    };
    for (int x = 0; fits(avail.topLeft() + QPoint(x, 0)); x += kCascadePx)
        diagonal(avail.topLeft() + QPoint(x, 0));
    for (int y = kCascadePx; fits(avail.topLeft() + QPoint(0, y)); y += kCascadePx)
        diagonal(avail.topLeft() + QPoint(0, y));
    for (const QPoint& p : std::as_const(candidates)) {
        if (!taken(p)) {
            window->move(p);
            return;
        }
    }
    // The screen is full of windows: overlap is unavoidable, stay on the screen.
    QPoint p = ref->pos() + step;
    p.setX(std::clamp(p.x(), avail.left(), std::max(avail.left(), avail.right() - outer.width())));
    p.setY(std::clamp(p.y(), avail.top(), std::max(avail.top(), avail.bottom() - outer.height())));
    window->move(p);
}

void Application::registerWindow(ViewerWindow* window) {
    d->windows.append(window);
    d->allWindows.removeAll(QPointer<ViewerWindow>());
    d->allWindows.append(window);
    d->front = window;
    emit windowsChanged();
    if (d->menu) {
        d->menu->refreshWindowList();
        d->menu->refreshState();
    }
}

void Application::unregisterWindow(ViewerWindow* window) {
    d->windows.removeAll(QPointer<ViewerWindow>(window));
    d->windows.removeAll(QPointer<ViewerWindow>());
    if (d->front == window || !d->front) {
        const QList<ViewerWindow*> list = windows();
        d->front = list.isEmpty() ? nullptr : list.last();
    }
    emit windowsChanged();
    if (d->menu) {
        d->menu->refreshWindowList();
        d->menu->refreshState();
    }
}

void Application::windowActivated(ViewerWindow* window) {
    if (d->front == window)
        return;
    QPointer<ViewerWindow> old = d->front;
    d->front = window;
    if (old)
        old->refreshActivity(); // the previous front window prefetches less (F-WIN-09)
    if (window)
        window->refreshActivity();
    d->menuTimer.start();
}

void Application::windowChanged(ViewerWindow*) {
    d->menuTimer.start();
}

void Application::revalidateFolder(const QString& folder) {
    if (folder.isEmpty())
        return;
    // A stat on a worker; only a changed folder costs anything (B12, F-WIN-09).
    auto* w = new QFutureWatcher<bool>(this);
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w, folder] {
        const bool stale = w->result();
        w->deleteLater();
        if (stale)
            folderChangedOnDisk(folder);
    });
    w->setFuture(QtConcurrent::run([folder] { return FolderCache::instance().isStale(folder); }));
}

void Application::folderChangedOnDisk(const QString& folder) {
    // One change = one enumeration (FolderCache), then every window re-sorts its copy.
    FolderCache::instance().invalidate(folder);
    for (ViewerWindow* w : windows())
        w->folderChanged(folder);
}

void Application::setWindowFolder(ViewerWindow*, const QString& oldFolder, const QString& newFolder) {
    if (!oldFolder.isEmpty()) {
        auto it = d->watchCount.find(oldFolder);
        if (it != d->watchCount.end() && --*it <= 0) {
            d->watchCount.erase(it);
            if (d->watcher)
                d->watcher->unwatch(oldFolder);
        }
    }
    if (!newFolder.isEmpty() && d->watchCount[newFolder]++ == 0 && d->watcher)
        d->watcher->watch(newFolder);
}

// ---------------------------------------------------------------------------------------
// Session (F-WIN-06)
// ---------------------------------------------------------------------------------------

void Application::setSessionEnabled(bool enabled) {
    d->sessionEnabled = enabled;
}

void Application::saveSession() {
    if (!d->sessionEnabled)
        return;
    if (isRestorePending())
        return; // ⌘Q before the last session came back: keep it as it was saved
    QVariantList list;
    if (Settings::instance().value(key::RestoreWindows).toBool()) {
        for (ViewerWindow* w : windows()) {
            if (w->isEmpty())
                continue;
            const QVariantMap state = w->sessionState();
            if (!state.isEmpty())
                list << state;
        }
    }
    Settings::instance().setValue(QString::fromLatin1(kSessionKey), list);
}

bool Application::hasSession() const {
    return !Settings::instance().value(QString::fromLatin1(kSessionKey)).toList().isEmpty();
}

void Application::restoreSession() {
    const QVariantList list = Settings::instance().value(QString::fromLatin1(kSessionKey)).toList();
    QStringList paths;
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        paths << m.value(QStringLiteral("folder")).toString();
        paths << m.value(QStringLiteral("files")).toStringList();
        paths << m.value(QStringLiteral("current")).toString();
    }
    paths.removeAll(QString());
    d->startPending = true;
    d->restore = Private::Restore::Classifying;
    d->restoreList = list;
    // Only what still exists comes back (volumes may be gone); checked on a worker.
    classifyPaths(paths, this, [this](const ClassifiedPaths& c) {
        if (d->restore != Private::Restore::Classifying)
            return; // an open request came first
        d->restoreExisting = c;
        d->restore = Private::Restore::Ready;
        if (d->startWindowElapsed)
            applyRestore();
    });
}

void Application::applyRestore() {
    if (d->restore != Private::Restore::Ready)
        return;
    const QVariantList list = d->restoreList;
    const QStringList existing = d->restoreExisting.files + d->restoreExisting.folders;
    d->restore = Private::Restore::None;
    d->restoreList.clear();
    d->startPending = false;
    // Windows the user opened meanwhile (e.g. ⌘N) stay in front.
    const bool othersOpen = !windows().isEmpty();
    ViewerWindow* last = nullptr;
    for (const QVariant& v : list) {
        ViewerWindow* w = createWindow();
        if (!w->restoreSessionState(v.toMap(), existing)) {
            w->close();
            continue;
        }
        w->show();
        last = w;
    }
    if (last && !othersOpen) {
        last->raise();
        last->activateWindow();
        windowActivated(last);
    }
    if (windows().isEmpty() && !d->burstTimer.isActive())
        newWindow();
    emit sessionRestored();
}

void Application::quit() {
    if (fileOps().isBusy()) {
        // Queued file operations (e.g. trash on a slow network volume) are never dropped:
        // wait for them with a busy cursor and a message, then quit (busyChanged).
        const bool first = !d->quitWhenIdle;
        d->quitWhenIdle = true;
        if (first) {
            if (!d->busyCursor) {
                updateBusy(true);
            } else if (d->busyWindow) {
                d->busyWindow->showMessage(busyText(), 60000);
            } else if (ViewerWindow* w = frontWindow()) {
                d->busyWindow = w;
                w->showMessage(busyText(), 60000);
            }
        }
        return;
    }
    d->quitWhenIdle = false;
    if (!d->quitting) {
        saveSession();
        d->quitting = true;
    }
    closeAllWindows();
    d->services.quitApp();
}

bool Application::isQuitPending() const {
    return d->quitWhenIdle;
}

void Application::setOpenGroupingIntervalForTesting(int ms) {
    d->burstTimer.setInterval(ms > 0 ? ms : kOpenGroupingMs);
}

bool Application::isOpenBurstPending() const {
    return d->burstTimer.isActive();
}

void Application::flushOpenBurstForTesting() {
    if (!d->burstTimer.isActive())
        return;
    d->burstTimer.stop();
    flushOpenBurst();
}

void Application::setBusyHintDelayForTesting(int ms) {
    d->busyTimer.setInterval(ms >= 0 ? ms : kBusyHintMs);
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

bool Application::intercepted(ViewerWindow* window, Action action) const {
    return d->services.interceptAction && d->services.interceptAction(window, action);
}

void Application::perform(Action action) {
    // Menu shortcuts act on the key window. If that is not a viewer (preferences,
    // shortcut overview, a message box), image and window commands must not reach the
    // viewer hidden behind it.
    QWidget* active = d->services.activeWindow ? d->services.activeWindow() : nullptr;
    ViewerWindow* front = frontWindow();
    if ((!front || (active && !qobject_cast<ViewerWindow*>(active))) && intercepted(nullptr, action))
        return; // (with a viewer in front, ViewerWindow::perform asks)
    if (active && !qobject_cast<ViewerWindow*>(active)) {
        switch (action) {
        case Action::CloseWindow:
        case Action::ExitModeOrCloseWindow:
            active->close();
            break;
        case Action::NewWindow:
            newWindow();
            break;
        case Action::OpenPreferences:
            showPreferences();
            break;
        case Action::ShowShortcuts:
            showShortcuts();
            break;
        case Action::Undo:
            fileOps().undo();
            break;
        case Action::Redo:
            fileOps().redo();
            break;
        case Action::CloseAllWindows:
            closeAllWindows();
            break;
        default:
            break; // image/window commands: ignored
        }
        return;
    }
    if (front) {
        front->perform(action);
        return;
    }
    switch (action) {
    case Action::NewWindow:
        newWindow();
        break;
    case Action::OpenPreferences:
        showPreferences();
        break;
    case Action::ShowShortcuts:
        showShortcuts();
        break;
    case Action::Undo:
        fileOps().undo();
        break;
    case Action::Redo:
        fileOps().redo();
        break;
    default:
        break;
    }
}

void Application::showOpenDialog() {
    QStringList patterns;
    for (const QString& s : imageSuffixes())
        patterns << QStringLiteral("*.") + s;
    const QString filter = tr("Bilder (%1);;Alle Dateien (*)").arg(patterns.join(QLatin1Char(' ')));
    QString dir;
    if (auto* w = frontWindow(); w && !w->collection()->folderPath().isEmpty())
        dir = w->collection()->folderPath();
    else
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QStringList files = QFileDialog::getOpenFileNames(frontWindow(), tr("Öffnen"), dir, filter);
    if (!files.isEmpty())
        openPaths(files);
}

void Application::showOpenFolderDialog() {
    const QString dir =
        QFileDialog::getExistingDirectory(frontWindow(), tr("Ordner öffnen"),
                                          QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    if (!dir.isEmpty())
        openPaths({dir});
}

void Application::showPreferences() {
    PreferencesDialog* dlg = PreferencesDialog::showPreferences(nullptr);
    connect(dlg, &PreferencesDialog::makeDefaultRequested, this, &Application::makeDefaultViewer,
            Qt::UniqueConnection);
}

void Application::showShortcuts() {
    if (d->shortcuts) {
        d->shortcuts->show();
        d->shortcuts->raise();
        d->shortcuts->activateWindow();
        return;
    }
    auto* dlg = new QDialog(nullptr);
    dlg->setObjectName(QStringLiteral("lfShortcutsDialog"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Tastenkürzel"));
    auto* layout = new QVBoxLayout(dlg);
    auto* table = new QTableWidget(dlg);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({tr("Aktion"), tr("Taste(n)")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    QList<QPair<QString, QString>> rows = {
        {tr("Nächstes / vorheriges Bild"), tr("→ / ←, Leertaste / ⇧Leertaste, Bild↓ / Bild↑, Mausrad")},
        {tr("Erstes / letztes Bild"), tr("Pos1 / Ende")},
        {tr("Vollbild / Modus verlassen"), tr("Esc")},
    };
    // Planned actions (slideshow, info panel, rating, rename …) have keys already but do
    // nothing yet: listed last, grayed out and marked, never as usable.
    QList<QPair<QString, QString>> later;
    for (Action a : InputBindings::allActions()) {
        const QList<QKeySequence> seqs = bindings().shortcuts(a);
        if (seqs.isEmpty())
            continue;
        QStringList keys;
        for (const QKeySequence& s : seqs)
            keys << s.toString(QKeySequence::NativeText);
        if (InputBindings::isActionAvailable(a))
            rows.append({actionLabel(a), keys.join(QStringLiteral(", "))});
        else
            later.append(
                {tr("%1 (in einer späteren Version)").arg(actionLabel(a)), keys.join(QStringLiteral(", "))});
    }
    const int available = int(rows.size());
    rows += later;
    table->setObjectName(QStringLiteral("lfShortcutsTable"));
    table->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        auto* name = new QTableWidgetItem(rows[r].first);
        auto* keys = new QTableWidgetItem(rows[r].second);
        if (r >= available) {
            name->setFlags(name->flags() & ~Qt::ItemIsEnabled);
            keys->setFlags(keys->flags() & ~Qt::ItemIsEnabled);
        }
        table->setItem(r, 0, name);
        table->setItem(r, 1, keys);
    }
    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    dlg->resize(560, 620);
    d->shortcuts = dlg;
    dlg->show();
}

void Application::makeDefaultViewer() {
    // F-MAC-04: non-blocking checklist of the types (all preselected, choice remembered);
    // confirming starts the asynchronous registration of the selected types only.
    auto* dlg = new DefaultAppDialog(shell().imageDocumentTypes(), PreferencesDialog::existingInstance());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DefaultAppDialog::confirmed, this,
            [this](const QStringList& types) { setDefaultViewer(types); });
    dlg->open();
}

void Application::setDefaultViewer(const QStringList& selected) {
    const QStringList types =
        selected.isEmpty() ? DefaultAppDialog::rememberedSelection(shell().imageDocumentTypes()) : selected;
    if (types.isEmpty())
        return; // everything deselected: nothing to change
    if (auto* prefs = PreferencesDialog::existingInstance())
        prefs->showStatus(tr("Lumaflip wird als Standard-App festgelegt …"));
    auto done = [this](bool ok, const QString& error) {
        const QString message =
            ok ? tr("Lumaflip ist jetzt die Standard-App für Bilder.")
               : tr("Lumaflip konnte nicht als Standard-App festgelegt werden:\n%1").arg(error);
        if (auto* prefs = PreferencesDialog::existingInstance())
            prefs->showStatus(message);
        auto* box = new QMessageBox(ok ? QMessageBox::Information : QMessageBox::Warning, tr("Lumaflip"),
                                    message, QMessageBox::Ok, PreferencesDialog::existingInstance());
        box->setObjectName(QStringLiteral("lfMakeDefaultResult"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
        emit defaultViewerFinished(ok, message);
    };
    // Slow answer (macOS may ask the user): a note, not a failure; the result follows.
    auto pending = [this](const QString& message) {
        if (auto* prefs = PreferencesDialog::existingInstance())
            prefs->showStatus(message);
        else if (ViewerWindow* w = frontWindow())
            w->showMessage(message, 5000);
        emit defaultViewerPending(message);
    };
    shell().makeDefaultViewerAsync(types, this, done, pending); // unselected types stay untouched
}

void Application::retranslate() {
    // F-I18N-03 live switch. Windows retranslate themselves (changeEvent); what is built
    // once with its texts is built again: the menu bar (with the Dock menu), the
    // preferences and the shortcut overview.
    d->retranslatePending = false;
    delete d->menu;
    d->menu = new MainMenu(*this);
    if (PreferencesDialog* prefs = PreferencesDialog::recreateForLanguageChange())
        connect(prefs, &PreferencesDialog::makeDefaultRequested, this, &Application::makeDefaultViewer,
                Qt::UniqueConnection);
    if (d->shortcuts) {
        const bool visible = d->shortcuts->isVisible();
        const QRect geometry = d->shortcuts->geometry();
        delete d->shortcuts.data();
        if (visible) {
            showShortcuts();
            d->shortcuts->setGeometry(geometry);
        }
    }
    for (ViewerWindow* w : windows())
        windowChanged(w);
    emit languageChanged();
}

bool Application::openUrl(const QUrl& url) {
    return d->services.openUrl(url);
}

QUrl Application::privacySettingsUrl() {
    return QUrl(
        QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"));
}

QStringList Application::recentPaths() const {
    return RecentItems::paths();
}

void Application::clearRecentPaths() {
    RecentItems::clear();
}

void Application::addRecent(const QStringList& files, const QStringList& folders) {
    RecentItems::add(files, folders); // F-OPEN-11 (also the system list: Dock menu)
}

// ---------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------

bool Application::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::FileOpen: {
        // F-OPEN-01: Finder double click, "Öffnen mit", Dock drop, `open -a`.
        auto* e = static_cast<QFileOpenEvent*>(event);
        QString path = e->file();
        if (path.isEmpty() && e->url().isLocalFile())
            path = e->url().toLocalFile();
        if (!path.isEmpty())
            queueOpen(path);
        return true;
    }
    case QEvent::LanguageChange:
        // F-I18N-03: one rebuild for the several events of a switch (Qt's and our catalogue).
        if (watched == qApp && !d->retranslatePending) {
            d->retranslatePending = true;
            QTimer::singleShot(0, this, &Application::retranslate);
        }
        break;
    case QEvent::Quit:
        if (watched != qApp)
            break;
        if (fileOps().isBusy()) {
            // Quit from the Dock or at logout while file operations are pending: refused
            // for now (macOS cancels the termination), the app quits by itself once they
            // are done (never dropped silently).
            quit();
            event->ignore();
            return true;
        }
        // Quit from the Dock or at logout: windows are still open here (F-WIN-06).
        if (!d->quitting) {
            saveSession();
            d->quitting = true;
        }
        break;
    case QEvent::ShortcutOverride: {
        // Keys without ⌘/⌃ go to the focus widget instead of the menu shortcuts: in
        // a viewer window they are resolved by InputBindings (slots, culling rule,
        // 4.14); in dialogs they keep their normal meaning.
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!(ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) && !isModifierKey(ke->key())) {
            ke->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace lf
