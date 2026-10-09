// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainMenu.h"

#include "Application.h"
#include "ImageView.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QCoreApplication>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QUndoStack>

namespace lf {

namespace {

// Keys shown in the menu for actions whose keys are handled by input slots (4.14)
// rather than free shortcuts, so the menu still tells the user how to reach them.
QList<QKeySequence> displayFallback(Action a) {
    switch (a) {
    case Action::NextImage:
        return {QKeySequence(Qt::Key_Right)};
    case Action::PreviousImage:
        return {QKeySequence(Qt::Key_Left)};
    case Action::JumpForward:
        return {};
    default:
        return {};
    }
}

struct SortItem {
    SortKey key;
    const char* id;
};
const SortItem kSortItems[] = {
    {SortKey::Name, "name"},
    {SortKey::DateModified, "dateModified"},
    {SortKey::DateCreated, "dateCreated"},
    {SortKey::DateAdded, "dateAdded"},
    {SortKey::DateCaptured, "dateCaptured"},
    {SortKey::Size, "size"},
    {SortKey::Kind, "kind"},
};

} // namespace

MainMenu::MainMenu(Application& app) : QObject(&app), m_app(app) {
    m_bar = new QMenuBar(nullptr); // parentless: shared by all windows (global menu bar on macOS)
    m_bar->setObjectName(QStringLiteral("lfMenuBar"));
    m_dock = new QMenu();
    m_dock->setObjectName(QStringLiteral("lfDockMenu"));
    build();
#ifdef Q_OS_MACOS
    m_dock->setAsDockMenu(); // F-WIN-10
#endif
}

MainMenu::~MainMenu() {
    delete m_bar;
    delete m_dock;
}

QList<QMenu*> MainMenu::menus() const {
    QList<QMenu*> out;
    for (QAction* a : m_bar->actions())
        if (a->menu())
            out << a->menu();
    return out;
}

QAction* MainMenu::action(Action a) const {
    return m_actions.value(int(a));
}

QAction* MainMenu::command(const QString& id) const {
    return m_commands.value(id);
}

QAction* MainMenu::add(QMenu* menu, Action a, const QString& text) {
    auto* act = menu->addAction(text.isEmpty() ? actionLabel(a) : text);
    act->setObjectName(QStringLiteral("action:") + actionId(a));
    act->setShortcutContext(Qt::ApplicationShortcut);
    connect(act, &QAction::triggered, this, [this, a] { m_app.perform(a); });
    m_actions.insert(int(a), act);
    return act;
}

QAction* MainMenu::addCommand(QMenu* menu, const QString& id, const QString& text,
                              const QKeySequence& shortcut) {
    auto* act = menu->addAction(text);
    act->setObjectName(QStringLiteral("command:") + id);
    if (!shortcut.isEmpty()) {
        act->setShortcut(shortcut);
        act->setShortcutContext(Qt::ApplicationShortcut);
    }
    m_commands.insert(id, act);
    return act;
}

void MainMenu::build() {
    auto front = [this] { return m_app.frontWindow(); };

    // ---- Ablage ----
    QMenu* file = m_bar->addMenu(tr("Ablage"));
    file->setObjectName(QStringLiteral("menu:file"));
    add(file, Action::NewWindow, tr("Neues Fenster"));
    add(file, Action::DuplicateWindow, tr("Fenster duplizieren"));
    connect(addCommand(file, QStringLiteral("open"), tr("Öffnen …"), QKeySequence(QKeySequence::Open)),
            &QAction::triggered, &m_app, &Application::showOpenDialog);
    connect(addCommand(file, QStringLiteral("openFolder"), tr("Ordner öffnen …"),
                       QKeySequence(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_O)),
            &QAction::triggered, &m_app, &Application::showOpenFolderDialog);
    m_recentMenu = file->addMenu(tr("Zuletzt geöffnet"));
    m_recentMenu->setObjectName(QStringLiteral("menu:recent"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainMenu::rebuildRecent);
    rebuildRecent();
    file->addSeparator();
    add(file, Action::CloseWindow, tr("Schließen"));
    add(file, Action::CloseAllWindows, tr("Alle schließen"));
    file->addSeparator();
    add(file, Action::RevealInFinder, tr("Im Finder zeigen"));
    add(file, Action::Trash, tr("In den Papierkorb legen"));
    file->addSeparator();
    connect(addCommand(file, QStringLiteral("makeDefault"), tr("Als Standard-App festlegen …")),
            &QAction::triggered, &m_app, &Application::makeDefaultViewer);
    // Moved into the application menu ("Lumaflip") on macOS by their roles.
    QAction* about = addCommand(file, QStringLiteral("about"), tr("Über Lumaflip"));
    about->setMenuRole(QAction::AboutRole);
    connect(about, &QAction::triggered, &m_app, &Application::showAbout);
    QAction* prefs = add(file, Action::OpenPreferences, tr("Einstellungen …"));
    prefs->setMenuRole(QAction::PreferencesRole);
    QAction* quit =
        addCommand(file, QStringLiteral("quit"), tr("Lumaflip beenden"), QKeySequence(QKeySequence::Quit));
    quit->setMenuRole(QAction::QuitRole);
    connect(quit, &QAction::triggered, this, [this] {
        m_app.closeAllWindows();
        QCoreApplication::quit();
    });

    // ---- Bearbeiten ----
    QMenu* edit = m_bar->addMenu(tr("Bearbeiten"));
    edit->setObjectName(QStringLiteral("menu:edit"));
    QUndoStack* stack = m_app.fileOps().undoStack();
    m_undo = add(edit, Action::Undo, tr("Widerrufen"));
    m_redo = add(edit, Action::Redo, tr("Wiederholen"));
    auto updateUndo = [this, stack] {
        m_undo->setEnabled(stack->canUndo());
        m_undo->setText(stack->canUndo() ? tr("„%1“ widerrufen").arg(stack->undoText()) : tr("Widerrufen"));
        m_redo->setEnabled(stack->canRedo());
        m_redo->setText(stack->canRedo() ? tr("„%1“ wiederholen").arg(stack->redoText()) : tr("Wiederholen"));
    };
    connect(stack, &QUndoStack::indexChanged, this, updateUndo);
    connect(stack, &QUndoStack::cleanChanged, this, updateUndo);
    updateUndo();
    edit->addSeparator();
    add(edit, Action::CopyImage, tr("Kopieren"));
    add(edit, Action::CopyPath, tr("Pfad kopieren"));

    // ---- Darstellung ----
    QMenu* view = m_bar->addMenu(tr("Darstellung"));
    view->setObjectName(QStringLiteral("menu:view"));
    auto* zoomGroup = new QActionGroup(this);
    auto zoomCommand = [&](ZoomMode mode, const QString& text, Action a) {
        QAction* act =
            a != Action::None ? add(view, a, text) : addCommand(view, "zoom:" + zoomModeId(mode), text);
        act->setCheckable(true);
        zoomGroup->addAction(act);
        m_commands.insert("zoom:" + zoomModeId(mode), act);
        if (a == Action::None) {
            connect(act, &QAction::triggered, this, [front, mode] {
                if (auto* w = front())
                    w->setZoomMode(mode);
            });
        }
    };
    zoomCommand(ZoomMode::Fit, tr("Einpassen"), Action::ZoomFit);
    zoomCommand(ZoomMode::FitUpscale, tr("Einpassen inkl. Vergrößern"), Action::None);
    zoomCommand(ZoomMode::Actual, tr("100 % (1:1 Pixel)"), Action::ZoomActual);
    zoomCommand(ZoomMode::FillWidth, tr("Breite füllen"), Action::ZoomFillWidth);
    zoomCommand(ZoomMode::FillHeight, tr("Höhe füllen"), Action::None);
    zoomCommand(ZoomMode::Fill, tr("Fenster füllen"), Action::None);
    view->addSeparator();
    add(view, Action::ZoomIn, tr("Vergrößern"));
    add(view, Action::ZoomOut, tr("Verkleinern"));
    add(view, Action::ToggleFitActualAtCursor, tr("Einpassen ↔ 100 %"));
    add(view, Action::ToggleZoomLock, tr("Zoom fixieren"))->setCheckable(true);
    view->addSeparator();
    add(view, Action::ToggleFullScreen, tr("Vollbild"))->setCheckable(true);
    QAction* status = addCommand(view, QStringLiteral("statusBar"), tr("Statusleiste"));
    status->setCheckable(true);
    connect(status, &QAction::triggered, this,
            [](bool on) { Settings::instance().setValue(key::ShowStatusBar, on); });
    QMenu* bg = view->addMenu(tr("Hintergrund"));
    bg->setObjectName(QStringLiteral("menu:background"));
    auto* bgGroup = new QActionGroup(this);
    const QList<QPair<QString, QString>> backgrounds = {{"dark", tr("Dunkel")},
                                                        {"light", tr("Hell")},
                                                        {"system", tr("Wie System")},
                                                        {"checker", tr("Schachbrett")}};
    for (const auto& [id, text] : backgrounds) {
        QAction* act = addCommand(bg, "background:" + id, text);
        act->setCheckable(true);
        bgGroup->addAction(act);
        connect(act, &QAction::triggered, this, [id] { Settings::instance().setValue(key::Background, id); });
    }
    QAction* custom = addCommand(bg, QStringLiteral("background:custom"), tr("Eigene Farbe …"));
    custom->setCheckable(true);
    bgGroup->addAction(custom);
    connect(custom, &QAction::triggered, this, [this, front] {
        const QColor initial = QColor::fromString(Settings::instance().value(key::Background).toString());
        const QColor c = QColorDialog::getColor(initial.isValid() ? initial : QColor(40, 40, 40), front(),
                                                tr("Hintergrundfarbe"));
        if (c.isValid())
            Settings::instance().setValue(key::Background, c.name(QColor::HexRgb));
        refreshState();
    });
    view->addSeparator();
    QMenu* sort = view->addMenu(tr("Sortieren nach"));
    sort->setObjectName(QStringLiteral("menu:sort"));
    QAction* finder = addCommand(sort, QStringLiteral("sort:finder"), tr("Wie Finder"));
    finder->setCheckable(true);
    connect(finder, &QAction::triggered, this, [this, front] {
        if (auto* w = front())
            w->setSortFollowFinder();
        refreshState();
    });
    sort->addSeparator();
    auto* keyGroup = new QActionGroup(this);
    keyGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (const auto& item : kSortItems) {
        const SortKey k = item.key;
        QAction* act = addCommand(sort, "sort:" + QString::fromLatin1(item.id), sortKeyLabel(k));
        act->setCheckable(true);
        keyGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, front, k] {
            if (auto* w = front())
                w->setSortKey(k);
            refreshState();
        });
    }
    sort->addSeparator();
    auto* dirGroup = new QActionGroup(this);
    for (bool asc : {true, false}) {
        QAction* act = addCommand(sort, asc ? QStringLiteral("sort:asc") : QStringLiteral("sort:desc"),
                                  asc ? tr("Aufsteigend") : tr("Absteigend"));
        act->setCheckable(true);
        dirGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, front, asc] {
            if (auto* w = front())
                w->setSortAscending(asc);
            refreshState();
        });
    }
    connect(view, &QMenu::aboutToShow, this, &MainMenu::refreshState);
    connect(sort, &QMenu::aboutToShow, this, &MainMenu::refreshState);

    // ---- Navigation ----
    QMenu* nav = m_bar->addMenu(tr("Navigation"));
    nav->setObjectName(QStringLiteral("menu:navigation"));
    add(nav, Action::NextImage, tr("Nächstes Bild"));
    add(nav, Action::PreviousImage, tr("Vorheriges Bild"));
    add(nav, Action::FirstImage, tr("Erstes Bild"));
    add(nav, Action::LastImage, tr("Letztes Bild"));
    nav->addSeparator();
    add(nav, Action::JumpForward, tr("Mehrere Bilder vor"));
    add(nav, Action::JumpBackward, tr("Mehrere Bilder zurück"));

    // ---- Bild ----
    QMenu* image = m_bar->addMenu(tr("Bild"));
    image->setObjectName(QStringLiteral("menu:image"));
    add(image, Action::RotateLeft, tr("Links drehen"));
    add(image, Action::RotateRight, tr("Rechts drehen"));
    add(image, Action::FlipHorizontal, tr("Horizontal spiegeln"));
    add(image, Action::FlipVertical, tr("Vertikal spiegeln"));

    // ---- Fenster ----
    m_windowMenu = m_bar->addMenu(tr("Fenster"));
    m_windowMenu->setObjectName(QStringLiteral("menu:window"));
    connect(addCommand(m_windowMenu, QStringLiteral("minimize"), tr("Minimieren"),
                       QKeySequence(Qt::ControlModifier | Qt::Key_M)),
            &QAction::triggered, this, [front] {
                if (auto* w = front())
                    w->showMinimized();
            });
    connect(addCommand(m_windowMenu, QStringLiteral("zoomWindow"), tr("Zoomen")), &QAction::triggered, this,
            [front] {
                if (auto* w = front())
                    w->isMaximized() ? w->showNormal() : w->showMaximized();
            });
    m_windowMenu->addSeparator();
    connect(addCommand(m_windowMenu, QStringLiteral("bringAllToFront"), tr("Alle nach vorne bringen")),
            &QAction::triggered, this, [this] {
                for (ViewerWindow* w : m_app.windows())
                    w->raise();
                if (auto* w = m_app.frontWindow()) {
                    w->raise();
                    w->activateWindow();
                }
            });
    m_windowListStart = m_windowMenu->addSeparator();
    connect(m_windowMenu, &QMenu::aboutToShow, this, &MainMenu::refreshWindowList);

    // ---- Hilfe ----
    QMenu* help = m_bar->addMenu(tr("Hilfe"));
    help->setObjectName(QStringLiteral("menu:help"));
    add(help, Action::ShowShortcuts, tr("Tastenkürzel"));

    refreshShortcuts();
    refreshState();
    refreshWindowList();
}

void MainMenu::rebuildRecent() {
    m_recentMenu->clear();
    const QStringList recent = m_app.recentPaths();
    for (const QString& p : recent) {
        QAction* act = m_recentMenu->addAction(QFileInfo(p).fileName());
        act->setToolTip(p);
        connect(act, &QAction::triggered, this, [this, p] { m_app.openPaths({p}); });
    }
    if (!recent.isEmpty())
        m_recentMenu->addSeparator();
    QAction* clear = m_recentMenu->addAction(tr("Liste löschen"));
    clear->setEnabled(!recent.isEmpty());
    connect(clear, &QAction::triggered, this, [this] { m_app.clearRecentPaths(); });
}

void MainMenu::refreshShortcuts() {
    const InputBindings& b = m_app.bindings();
    for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it) {
        const Action a = Action(it.key());
        QList<QKeySequence> seqs = b.shortcuts(a);
        if (seqs.isEmpty())
            seqs = displayFallback(a);
        it.value()->setShortcuts(seqs);
    }
}

void MainMenu::refreshState() {
    ViewerWindow* w = m_app.frontWindow();
    const bool hasWindow = w != nullptr;
    // Only "is there a window" decides (updated synchronously when windows come and
    // go). Image dependent commands stay enabled and do nothing without an image, so
    // a stale state can never swallow a shortcut.
    for (Action a : {Action::DuplicateWindow, Action::CloseWindow,    Action::ToggleFullScreen,
                     Action::ZoomIn,          Action::ZoomOut,        Action::ZoomActual,
                     Action::ZoomFit,         Action::ZoomFillWidth,  Action::ToggleFitActualAtCursor,
                     Action::ToggleZoomLock,  Action::RevealInFinder, Action::Trash,
                     Action::CopyImage,       Action::CopyPath,       Action::NextImage,
                     Action::PreviousImage,   Action::FirstImage,     Action::LastImage,
                     Action::JumpForward,     Action::JumpBackward,   Action::RotateLeft,
                     Action::RotateRight,     Action::FlipHorizontal, Action::FlipVertical})
        if (QAction* act = action(a))
            act->setEnabled(hasWindow);
    for (const QString& id : {QStringLiteral("minimize"), QStringLiteral("zoomWindow")})
        command(id)->setEnabled(hasWindow);
    action(Action::CloseAllWindows)->setEnabled(!m_app.windows().isEmpty());

    Settings& st = Settings::instance();
    command(QStringLiteral("statusBar"))->setChecked(st.value(key::ShowStatusBar).toBool());
    const QString bg = st.value(key::Background).toString();
    if (QAction* act = command("background:" + bg))
        act->setChecked(true);
    else
        command(QStringLiteral("background:custom"))->setChecked(true);
    if (w) {
        if (QAction* z = command("zoom:" + zoomModeId(w->view()->zoomMode())))
            z->setChecked(w->view()->zoomMode() != ZoomMode::Custom);
        action(Action::ToggleZoomLock)->setChecked(w->view()->zoomLocked());
        action(Action::ToggleFullScreen)->setChecked(w->isFullScreen());
        const Collection* c = w->collection();
        const SortSpec spec = c->sortSpec();
        command(QStringLiteral("sort:finder"))->setChecked(c->followsFinder());
        for (const auto& item : kSortItems)
            command("sort:" + QString::fromLatin1(item.id))
                ->setChecked(!c->followsFinder() && spec.key == item.key);
        command(QStringLiteral("sort:asc"))->setChecked(spec.ascending);
        command(QStringLiteral("sort:desc"))->setChecked(!spec.ascending);
    }
    for (const QString& id :
         {QStringLiteral("sort:finder"), QStringLiteral("sort:asc"), QStringLiteral("sort:desc")})
        command(id)->setEnabled(hasWindow);
    for (const auto& item : kSortItems)
        command("sort:" + QString::fromLatin1(item.id))->setEnabled(hasWindow);
}

void MainMenu::refreshWindowList() {
    for (QAction* a : std::as_const(m_windowEntries))
        delete a;
    m_windowEntries.clear();
    m_dock->clear();
    m_dockEntries.clear();
    ViewerWindow* front = m_app.frontWindow();
    for (ViewerWindow* w : m_app.windows()) {
        const QString title = w->windowTitle();
        QPointer<ViewerWindow> target(w);
        auto bringToFront = [this, target] {
            if (!target)
                return;
            if (target->isMinimized())
                target->showNormal();
            target->raise();
            target->activateWindow();
            m_app.windowActivated(target);
        };
        auto* entry = new QAction(title, m_windowMenu);
        entry->setCheckable(true);
        entry->setChecked(w == front);
        connect(entry, &QAction::triggered, this, bringToFront);
        m_windowMenu->addAction(entry);
        m_windowEntries << entry;
        QAction* dockEntry = m_dock->addAction(title);
        dockEntry->setCheckable(true);
        dockEntry->setChecked(w == front);
        connect(dockEntry, &QAction::triggered, this, bringToFront);
        m_dockEntries << dockEntry;
    }
    if (!m_app.windows().isEmpty())
        m_dock->addSeparator();
    QAction* newWin = m_dock->addAction(tr("Neues Fenster"));
    connect(newWin, &QAction::triggered, &m_app, &Application::newWindow);
}

QStringList MainMenu::windowMenuTitles() const {
    QStringList out;
    for (QAction* a : m_windowEntries)
        out << a->text();
    return out;
}

QStringList MainMenu::dockMenuTitles() const {
    QStringList out;
    for (QAction* a : m_dockEntries)
        out << a->text();
    return out;
}

} // namespace lf
