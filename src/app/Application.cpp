// SPDX-License-Identifier: GPL-3.0-or-later
#include "Application.h"

#include "ImageView.h"
#include "MainMenu.h"
#include "PreferencesDialog.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderCache.h"
#include "core/ImageFormats.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

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
constexpr int kMaxRecent = 12;
const char* const kGeometryKey = "window/geometry";
const char* const kRecentKey = "recent/paths";
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
    QTimer menuTimer;
    QTimer busyTimer;
    bool busyCursor = false;
    QPointer<QDialog> shortcuts;
    bool started = false;      // start() ran (Dock reopen handling)
    bool startPending = false; // start() still waits for an open event / the session
    bool quitting = false;
    bool sessionEnabled = true;
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
    d->bindings = std::make_unique<InputBindings>();

    QGuiApplication::setQuitOnLastWindowClosed(false); // macOS: the app stays, menu bar works
    qApp->installEventFilter(this);

    Settings& st = Settings::instance();
    if (const int mb = st.value(key::MemoryBudgetMB).toInt(); mb > 0)
        provider().setMemoryBudget(qint64(mb) << 20);
    provider().setRawEmbeddedOnly(st.value(key::RawEmbeddedOnly).toBool());

    // One folder watcher for all windows; every folder is watched once (F-WIN-09, F-OPEN-07).
    // One change = one enumeration (FolderCache), then every window re-sorts its copy.
    d->watcher = createFolderWatcher(this);
    connect(d->watcher.get(), &FolderWatcher::folderChanged, this, [this](const QString& folder) {
        FolderCache::instance().invalidate(folder);
        for (ViewerWindow* w : windows())
            w->folderChanged(folder);
    });

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
        if (busy)
            d->busyTimer.start();
        else
            updateBusy(false);
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
        }
        d->menu->refreshState();
    });

    // Dock icon clicked (applicationShouldHandleReopen arrives as a forced "active" state
    // change): without any window, open an empty one like other document apps.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            handleReopen();
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

    d->menu = new MainMenu(*this);
}

Application::~Application() {
    qApp->removeEventFilter(this);
    updateBusy(false);
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
    if (busy) {
        QGuiApplication::setOverrideCursor(Qt::BusyCursor);
        if (auto* w = frontWindow())
            w->showMessage(tr("Dateivorgang läuft …"), 60000);
    } else {
        QGuiApplication::restoreOverrideCursor();
        if (auto* w = frontWindow(); w && w->toastText() == tr("Dateivorgang läuft …"))
            w->hideMessage();
    }
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
        openPaths(paths); // F-OPEN-01 command line: one request (missing ones are reported)
        return;
    }
    if (d->sessionEnabled && Settings::instance().value(key::RestoreWindows).toBool() && hasSession()) {
        restoreSession(); // F-WIN-06
        return;
    }
    // Launched by Finder/Dock the open request arrives as QFileOpenEvent right after
    // start; only if nothing comes the start window appears (F-OPEN-12, simple).
    d->startPending = true;
    QTimer::singleShot(kEmptyStartDelayMs, this, [this] {
        d->startPending = false;
        if (windows().isEmpty() && !d->burstTimer.isActive())
            newWindow();
    });
}

void Application::handleReopen() {
    if (!d->started || d->startPending || d->quitting || d->burstTimer.isActive() || !windows().isEmpty())
        return;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->isWindow() && !w->inherits("QMenu") && !w->inherits("QMenuBar"))
            return; // e.g. only the preferences window is open: macOS brings it forward
    newWindow();
}

void Application::queueOpen(const QString& path) {
    if (!d->burstTimer.isActive()) {
        // First request of a burst: show it immediately (P-01).
        d->burstPaths = {path};
        d->burstWindow = nullptr;
        d->burstWindowOwned = false;
        if (hasImageSuffix(QFileInfo(path).fileName())) {
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
                    addRecent({abs});
            });
        } else {
            openPaths({path});
        }
        d->burstTimer.start();
        return;
    }
    d->burstPaths << path;
    d->burstTimer.start(); // keep grouping while requests keep coming
}

void Application::flushOpenBurst() {
    const QStringList all = d->burstPaths;
    d->burstPaths.clear();
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
        addRecent(c.files + c.folders);
    });
}

ViewerWindow* Application::openPaths(const QStringList& paths) {
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
        addRecent(c.files + c.folders);
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
    if (reusedOrCreated)
        *reusedOrCreated = fresh || reused;
    // Decoding starts before the window is shown (P-01).
    if (isFolder)
        target->openFolder(paths.first());
    else if (paths.size() == 1)
        target->openFile(paths.first());
    else
        target->openFiles(paths);
    if (fresh || !target->isVisible())
        target->show();
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
    // Only what still exists comes back (volumes may be gone); checked on a worker.
    classifyPaths(paths, this, [this, list](const ClassifiedPaths& c) {
        d->startPending = false;
        const QStringList existing = c.files + c.folders;
        for (const QVariant& v : list) {
            ViewerWindow* w = createWindow();
            if (!w->restoreSessionState(v.toMap(), existing)) {
                w->close();
                continue;
            }
            w->show();
            windowActivated(w);
        }
        if (windows().isEmpty() && !d->burstTimer.isActive())
            newWindow();
        emit sessionRestored();
    });
}

void Application::quit() {
    if (!d->quitting) {
        saveSession();
        d->quitting = true;
    }
    closeAllWindows();
    QCoreApplication::quit();
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

void Application::perform(Action action) {
    // Menu shortcuts act on the key window. If that is not a viewer (preferences,
    // shortcut overview, a message box), image and window commands must not reach the
    // viewer hidden behind it.
    QWidget* active = d->services.activeWindow ? d->services.activeWindow() : nullptr;
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
    if (ViewerWindow* w = frontWindow()) {
        w->perform(action);
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

void Application::showAbout() {
    QMessageBox::about(frontWindow(), tr("Über Lumaflip"),
                       tr("<h3>Lumaflip %1</h3>"
                          "<p>Schneller Bildbetrachter für macOS: Doppelklick öffnet den ganzen Ordner – "
                          "in Finder-Reihenfolge.</p>"
                          "<p>Freie Software unter der GNU General Public License v3 oder später.<br>"
                          "Verwendet Qt (LGPLv3).</p>")
                           .arg(QCoreApplication::applicationVersion()));
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
    for (Action a : InputBindings::allActions()) {
        const QList<QKeySequence> seqs = bindings().shortcuts(a);
        if (seqs.isEmpty())
            continue;
        QStringList keys;
        for (const QKeySequence& s : seqs)
            keys << s.toString(QKeySequence::NativeText);
        rows.append({actionLabel(a), keys.join(QStringLiteral(", "))});
    }
    table->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        table->setItem(r, 0, new QTableWidgetItem(rows[r].first));
        table->setItem(r, 1, new QTableWidgetItem(rows[r].second));
    }
    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    dlg->resize(560, 620);
    d->shortcuts = dlg;
    dlg->show();
}

void Application::makeDefaultViewer() {
    // Non-blocking question (the answer starts the asynchronous registration).
    auto* box =
        new QMessageBox(QMessageBox::Question, tr("Als Standard-App festlegen"),
                        tr("Soll Lumaflip die Standard-App für gängige Bildformate (JPEG, PNG, HEIC, RAW …) "
                           "werden?"),
                        QMessageBox::NoButton, PreferencesDialog::existingInstance());
    box->setObjectName(QStringLiteral("lfMakeDefaultQuestion"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* yes = box->addButton(tr("Festlegen"), QMessageBox::AcceptRole);
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(yes);
    connect(box, &QMessageBox::buttonClicked, this, [this, yes](QAbstractButton* b) {
        if (b == yes)
            setDefaultViewer();
    });
    box->open();
}

void Application::setDefaultViewer() {
    if (auto* prefs = PreferencesDialog::existingInstance())
        prefs->showStatus(tr("Lumaflip wird als Standard-App festgelegt …"));
    shell().makeDefaultViewerAsync(shell().imageDocumentTypes(), this, [this](bool ok, const QString& error) {
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
    });
}

bool Application::openUrl(const QUrl& url) {
    return d->services.openUrl(url);
}

QUrl Application::privacySettingsUrl() {
    return QUrl(
        QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"));
}

QStringList Application::recentPaths() const {
    return Settings::instance().value(QString::fromLatin1(kRecentKey)).toStringList();
}

void Application::clearRecentPaths() {
    Settings::instance().setValue(QString::fromLatin1(kRecentKey), QStringList());
}

void Application::addRecent(const QStringList& paths) {
    if (paths.isEmpty())
        return;
    QStringList recent = recentPaths();
    for (auto it = paths.crbegin(); it != paths.crend(); ++it) {
        recent.removeAll(*it);
        recent.prepend(*it);
    }
    while (recent.size() > kMaxRecent)
        recent.removeLast();
    Settings::instance().setValue(QString::fromLatin1(kRecentKey), recent);
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
    case QEvent::Quit:
        // Quit from the Dock or at logout: windows are still open here (F-WIN-06).
        if (watched == qApp && !d->quitting) {
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
