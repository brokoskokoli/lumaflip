// SPDX-License-Identifier: GPL-3.0-or-later
#include "Application.h"

#include "MainMenu.h"
#include "PreferencesDialog.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
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
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

namespace lf {

namespace {

constexpr int kCascadePx = 26; // F-WIN-08
constexpr int kMaxRecent = 12;
const char* const kGeometryKey = "window/geometry";
const char* const kRecentKey = "recent/paths";

Application* s_instance = nullptr;

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta || key == Qt::Key_Alt ||
           key == Qt::Key_CapsLock || key == Qt::Key_unknown || key == 0;
}

QString normalized(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).normalized(QString::NormalizationForm_C);
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
    QHash<QString, QPointer<ViewerWindow>> trashedBy;
    // Open burst (F-OPEN-04)
    QTimer burstTimer;
    QStringList burstPaths;
    QPointer<ViewerWindow> burstWindow;
    QTimer menuTimer;
    QPointer<QDialog> shortcuts;
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
    d->bindings = std::make_unique<InputBindings>();

    QGuiApplication::setQuitOnLastWindowClosed(false); // macOS: the app stays, menu bar works
    qApp->installEventFilter(this);

    Settings& st = Settings::instance();
    if (const int mb = st.value(key::MemoryBudgetMB).toInt(); mb > 0)
        provider().setMemoryBudget(qint64(mb) << 20);

    // One folder watcher for all windows; every folder is watched once (F-WIN-09, F-OPEN-07).
    d->watcher = createFolderWatcher(this);
    connect(d->watcher.get(), &FolderWatcher::folderChanged, this, [this](const QString& folder) {
        for (ViewerWindow* w : windows())
            w->folderChanged(folder);
    });

    // File operations of any window update every window (F-FILE-01).
    FileOps& ops = fileOps();
    connect(&ops, &FileOps::fileRemoved, this, [this](const QString& path) {
        provider().invalidate(path);
        for (ViewerWindow* w : windows())
            w->fileRemoved(path);
    });
    connect(&ops, &FileOps::fileAdded, this, [this](const QString& path) {
        provider().invalidate(path);
        const QPointer<ViewerWindow> origin = d->trashedBy.take(path);
        for (ViewerWindow* w : windows())
            w->fileAdded(path, w == origin.data());
    });
    connect(&ops, &FileOps::fileRenamed, this, [this](const QString& from, const QString& to) {
        provider().invalidate(from);
        provider().invalidate(to);
        for (ViewerWindow* w : windows()) {
            const bool wasCurrent = w->currentPath() == from;
            w->fileAdded(to, wasCurrent);
            w->fileRemoved(from);
        }
    });
    connect(&ops, &FileOps::operationFailed, this, [this](const QString& message) {
        if (auto* w = frontWindow())
            w->showMessage(message, 5000);
        else
            QMessageBox::warning(nullptr, tr("Lumaflip"), message);
    });

    connect(&st, &Settings::changed, this, [this](const QString& k) {
        if (k.startsWith(QLatin1String(key::InputPrefix)) || k == QLatin1String(key::WrapMode)) {
            d->bindings->reload();
            d->menu->refreshShortcuts();
        } else if (k == QLatin1String(key::MemoryBudgetMB)) {
            const int mb = Settings::instance().value(key::MemoryBudgetMB).toInt();
            if (mb > 0)
                provider().setMemoryBudget(qint64(mb) << 20);
        }
        d->menu->refreshState();
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

// ---------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------

void Application::start(const QStringList& arguments) {
    QStringList paths;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& a = arguments[i];
        if (a.startsWith(QLatin1Char('-')))
            continue; // -psn_…, -NSDocumentRevisionsDebugMode etc.
        if (QFileInfo::exists(a))
            paths << a;
    }
    if (!paths.isEmpty()) {
        openPaths(paths); // F-OPEN-01 command line: one request
        return;
    }
    // Launched by Finder/Dock the open request arrives as QFileOpenEvent right after
    // start; only if nothing comes the start window appears (F-OPEN-12, simple).
    QTimer::singleShot(kEmptyStartDelayMs, this, [this] {
        if (windows().isEmpty() && !d->burstTimer.isActive())
            newWindow();
    });
}

void Application::queueOpen(const QString& path) {
    if (!d->burstTimer.isActive()) {
        // First request of a burst: show it immediately (P-01).
        d->burstPaths = {path};
        d->burstWindow = openPaths({path});
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
    QStringList files;
    QStringList folders;
    for (const QString& p : all) {
        const QFileInfo fi(p);
        if (fi.isDir())
            folders << fi.absoluteFilePath();
        else if (fi.exists())
            files << fi.absoluteFilePath();
    }
    const QString first = QFileInfo(all.first()).absoluteFilePath();
    const bool firstIsFolder = QFileInfo(first).isDir();
    ViewerWindow* w = d->burstWindow;
    // F-OPEN-04: several files in one request -> exactly those, in the window that
    // already shows the first one.
    if (!files.isEmpty() && !(files.size() == 1 && files.first() == first)) {
        if (w && !firstIsFolder)
            w->openPaths(files);
        else
            openInWindow(files);
    }
    for (const QString& f : std::as_const(folders))
        if (!(firstIsFolder && f == first))
            openInWindow({f});
    addRecent(all);
}

ViewerWindow* Application::openPaths(const QStringList& paths) {
    QStringList files;
    QStringList folders;
    QStringList missing;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (!fi.exists())
            missing << p;
        else if (fi.isDir())
            folders << fi.absoluteFilePath();
        else
            files << fi.absoluteFilePath();
    }
    ViewerWindow* last = nullptr;
    if (!files.isEmpty())
        last = openInWindow(files);
    for (const QString& f : std::as_const(folders))
        last = openInWindow({f});
    if (!missing.isEmpty()) {
        const QString msg = tr("Nicht gefunden: %1").arg(missing.join(QStringLiteral(", ")));
        if (auto* w = last ? last : frontWindow())
            w->showMessage(msg, 4000);
    }
    addRecent(files + folders);
    return last;
}

ViewerWindow* Application::openInWindow(const QStringList& paths) {
    const QString behavior = Settings::instance().value(key::OpenBehavior).toString();
    if (behavior == QLatin1String("newUnlessOpen") && paths.size() == 1) {
        // F-WIN-02: bring the window to front that already shows this file/folder.
        const QString p = normalized(paths.first());
        const bool dir = QFileInfo(p).isDir();
        for (ViewerWindow* w : windows()) {
            Collection* c = w->collection();
            const bool match = dir ? (c->isFolderCollection() && c->folderPath() == p) : c->indexOf(p) >= 0;
            if (!match)
                continue;
            if (!dir)
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
    if (front && front->isEmpty())
        target = front; // the start window is reused
    else if (behavior == QLatin1String("replaceFront") && front)
        target = front;
    const bool fresh = target == nullptr;
    if (fresh)
        target = createWindow();
    target->openPaths(paths); // decoding starts before the window is shown (P-01)
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
    // F-WIN-08: cascade from the front window, never exactly on top of another one.
    ViewerWindow* ref = frontWindow();
    if (!ref || ref == window)
        ref = others.last();
    QScreen* screen = ref->screen() ? ref->screen() : QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const QPoint step(kCascadePx, kCascadePx);
    QSize size = ref->size().boundedTo(avail.size() - QSize(4 * kCascadePx, 4 * kCascadePx));
    window->resize(size);
    auto taken = [&](QPoint p) {
        return std::any_of(others.cbegin(), others.cend(), [&](ViewerWindow* o) { return o->pos() == p; });
    };
    auto fits = [&](QPoint p) { return avail.contains(QRect(p, size).adjusted(0, 0, -1, -1)); };
    QPoint p = ref->pos() + step;
    int restarts = 0;
    for (int i = 0; i < 200 && (taken(p) || !fits(p)); ++i) {
        if (!fits(p)) {
            ++restarts;
            p = avail.topLeft() + QPoint((restarts * kCascadePx) % std::max(1, avail.width() / 2), 0);
        } else {
            p += step;
        }
    }
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

void Application::noteTrashedBy(const QString& path, ViewerWindow* window) {
    d->trashedBy.insert(path, window);
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

void Application::perform(Action action) {
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
        fileOps().undoStack()->undo();
        break;
    case Action::Redo:
        fileOps().undoStack()->redo();
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
    const auto answer = QMessageBox::question(
        frontWindow(), tr("Als Standard-App festlegen"),
        tr("Soll Lumaflip die Standard-App für gängige Bildformate (JPEG, PNG, HEIC, RAW …) werden?"));
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    if (shell().makeDefaultViewer(shell().imageDocumentTypes(), &error))
        QMessageBox::information(frontWindow(), tr("Lumaflip"),
                                 tr("Lumaflip ist jetzt die Standard-App für Bilder."));
    else
        QMessageBox::warning(frontWindow(), tr("Lumaflip"), error);
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
