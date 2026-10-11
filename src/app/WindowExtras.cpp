// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowExtras.h"

#include "Application.h"
#include "CollectionExtras.h"
#include "CompareView.h"
#include "CouplingManager.h"
#include "CullingController.h"
#include "FolderMapPanel.h"
#include "GridView.h"
#include "ImageView.h"
#include "Loupe.h"
#include "NavigationExtras.h"
#include "OverviewController.h"
#include "QuickFilterBar.h"
#include "QuickMoveController.h"
#include "TitleStrip.h"
#include "ViewerWindow.h"
#include "WindowModes.h"
#include "core/Collection.h"

#include <QDockWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStatusBar>

namespace lf {

WindowExtras::WindowExtras(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfWindowExtras"));
    m_overview = new OverviewController(window, app);
    m_loupe = new LoupeController(window);
    m_nav = new NavigationExtras(window, app);
    m_collection = new CollectionExtras(window, app);
    QWidget* central = window.centralWidget();
    m_central = central;
    m_filter = new QuickFilterBar(window, app, central);
    m_titleStrip = new TitleStripController(window, central); // F-VIEW-13/15
    connect(m_titleStrip, &TitleStripController::raised, this, [this] {
        if (m_filter->isOpen())
            m_filter->raise(); // the name filter stays usable above the strip
    });
    central->installEventFilter(this);
    watchKeys(window.view());
    connect(m_overview, &OverviewController::modeChanged, this, [this] {
        m_loupe->end();
        if (m_overview->grid())
            watchKeys(m_overview->grid());
        if (m_overview->compareView())
            watchKeys(m_overview->compareView());
        if (m_filter->isOpen())
            m_filter->raise();
        if (m_titleStrip->strip()->isVisible())
            m_titleStrip->strip()->raise();
    });
    if (CullingController* culling = window.culling()) {
        // M/C + 1–9 in the grid move/copy the selection (F-GRID-02, F-FILE-08).
        culling->quickMove()->setSubjects([this] {
            return m_overview->mode() == OverviewController::Mode::Viewer ? QStringList()
                                                                          : m_overview->subjectPaths();
        });
    }
    CouplingManager& coupling = CouplingManager::of(app);
    connect(&coupling, &CouplingManager::changed, this, &WindowExtras::updateStatus);
}

WindowExtras::~WindowExtras() = default;

void WindowExtras::watchKeys(QWidget* widget) {
    if (widget)
        widget->installEventFilter(this); // type-to-filter (F-NAV-08)
}

bool WindowExtras::ratingKeysActive() const {
    return m_overview->ratingKeysActive();
}

QWidget* WindowExtras::contentWidget() const {
    switch (m_overview->mode()) {
    case OverviewController::Mode::Grid:
        return m_overview->grid();
    case OverviewController::Mode::Compare:
        return m_overview->compareView();
    case OverviewController::Mode::Viewer:
        break;
    }
    return m_window.view();
}

void WindowExtras::focusContent() {
    if (QWidget* w = contentWidget())
        w->setFocus(Qt::OtherFocusReason);
}

bool WindowExtras::folderMapShown() const {
    return m_mapDock && !m_mapDock->isHidden();
}

void WindowExtras::setFolderMapShown(bool on) {
    if (on == folderMapShown())
        return;
    if (!m_mapDock) {
        if (!on)
            return;
        // F-INFO-08: a fixed part of the window like the info panel (SPEC 5.1), on the right.
        m_mapDock = new QDockWidget(tr("Ordnerkarte"), &m_window);
        m_mapDock->setObjectName(QStringLiteral("lfFolderMapDock"));
        m_mapDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        m_mapDock->setTitleBarWidget(new QWidget(m_mapDock));
        m_mapDock->setAllowedAreas(Qt::RightDockWidgetArea);
        m_map = new FolderMapPanel(m_window, m_mapDock);
        m_map->setMinimumWidth(260);
        m_mapDock->setWidget(m_map);
        m_mapDock->hide();
        m_window.addDockWidget(Qt::RightDockWidgetArea, m_mapDock);
        m_window.resizeDocks({m_mapDock}, {360}, Qt::Horizontal);
        connect(m_map, &FolderMapPanel::closeRequested, this, [this] { setFolderMapShown(false); });
    }
    m_mapDock->setVisible(on);
    // SPEC 10.3: the map (and its network traffic) exists only while the panel is shown.
    if (on)
        m_map->openMap();
    else
        m_map->closeMap();
    if (!on)
        focusContent();
    m_app.windowChanged(&m_window); // menu check mark
}

bool WindowExtras::perform(Action a, QPointF pos) {
    if (m_collection->perform(a))
        return true; // subfolders, RAW+JPEG pairs (also in the grid)
    switch (a) {
    case Action::GoToImage:
        m_nav->showGoToDialog();
        return true;
    case Action::ToggleShuffle:
        m_nav->setShuffle(!m_nav->shuffle());
        return true;
    case Action::FilterByName:
        m_filter->open();
        return true;
    case Action::ToggleAlwaysOnTop:
        setAlwaysOnTop(!m_onTop);
        return true;
    case Action::ToggleMinimalWindow:
        setMinimalWindow(!minimalWindow());
        return true;
    case Action::CoupleWindows:
        CouplingManager::of(m_app).showDialog(&m_window);
        return true;
    case Action::ToggleFolderMap:
        setFolderMapShown(!folderMapShown());
        return true;
    default:
        break;
    }
    if (m_overview->perform(a, pos))
        return true; // grid / compare
    if (a == Action::Magnifier) {
        m_loupe->begin(pos); // F-VIEW-11 (held key or middle button)
        return true;
    }
    // F-NAV-06: flipping in shuffled order (the slideshow has its own order).
    if (m_nav->shuffle() && !m_window.isSlideshowRunning()) {
        const int jump = m_app.bindings().jumpSize();
        switch (a) {
        case Action::NextImage:
            return m_nav->navigate(1);
        case Action::PreviousImage:
            return m_nav->navigate(-1);
        case Action::JumpForward:
            return m_nav->navigate(jump);
        case Action::JumpBackward:
            return m_nav->navigate(-jump);
        case Action::FirstImage:
            return m_nav->goFirst(false);
        case Action::LastImage:
            return m_nav->goFirst(true);
        default:
            break;
        }
    }
    // Esc clears the name filter when no mode is left to end (F-NAV-08).
    if ((a == Action::ExitModeOrNothing || a == Action::ExitModeOrCloseWindow) && !m_window.isFullScreen() &&
        !m_window.isSlideshowRunning() && !(m_window.culling() && m_window.culling()->isActive()) &&
        !m_window.collection()->nameFilter().isEmpty()) {
        m_filter->clearAndClose();
        return true;
    }
    return false;
}

void WindowExtras::attachStatusBar(QStatusBar* bar) {
    if (m_shuffleLabel || !bar)
        return;
    auto make = [bar](const char* name) {
        auto* l = new QLabel(bar);
        l->setObjectName(QString::fromLatin1(name));
        l->setContentsMargins(6, 0, 6, 0);
        bar->addWidget(l);
        l->hide();
        return l;
    };
    m_nameLabel = make("lfStatusNameFilter");
    m_shuffleLabel = make("lfStatusShuffle");
    m_coupledLabel = make("lfStatusCoupled");
    m_collection->attachStatusBar(bar);
    updateStatus();
}

QStringList WindowExtras::statusParts() const {
    QStringList parts;
    const QString name = m_window.collection()->nameFilter();
    if (!name.isEmpty())
        parts << tr("Name: „%1“").arg(name);
    if (m_nav->shuffle())
        parts << tr("Zufällig");
    if (CouplingManager::of(m_app).isCoupled(&m_window))
        parts << tr("Gekoppelt");
    parts << m_collection->statusParts();
    return parts;
}

void WindowExtras::updateStatus() {
    if (m_overview)
        m_overview->updateHeader();
    if (m_collection)
        m_collection->updateStatus();
    if (!m_shuffleLabel)
        return;
    const QString name = m_window.collection()->nameFilter();
    m_nameLabel->setText(name.isEmpty() ? QString() : tr("Name: „%1“").arg(name));
    m_nameLabel->setVisible(!name.isEmpty());
    m_shuffleLabel->setText(tr("Zufällig"));
    m_shuffleLabel->setVisible(m_nav->shuffle());
    const bool coupled = CouplingManager::of(m_app).isCoupled(&m_window);
    m_coupledLabel->setText(tr("Gekoppelt"));
    m_coupledLabel->setToolTip(
        tr("Blättern, Zoom und Ausschnitt laufen mit den anderen gekoppelten Fenstern"));
    m_coupledLabel->setVisible(coupled);
}

QString WindowExtras::emptyStateText() const {
    const QString name = m_window.collection()->nameFilter();
    if (name.isEmpty() || m_window.collection()->totalCount() == 0)
        return {};
    return tr("Kein Dateiname passt zu „%1“ – Esc hebt den Filter auf").arg(name);
}

void WindowExtras::setAlwaysOnTop(bool on) {
    m_onTop = on;
    setWindowAlwaysOnTop(&m_window, on);
    m_window.showMessage(on ? tr("Immer im Vordergrund") : tr("Nicht mehr im Vordergrund"), 1200);
    m_app.windowChanged(&m_window);
}

bool WindowExtras::minimalWindow() const {
    return m_titleStrip->borderless();
}

bool WindowExtras::menuInTitleStrip() const {
    return m_titleStrip && m_titleStrip->holdsMenuBar();
}

void WindowExtras::setMinimalWindow(bool on) {
    m_titleStrip->setBorderless(on);
    m_window.showMessage(
        on ? tr("Minimaler Fenstermodus – die Titelleiste erscheint, wenn sich die Maus bewegt")
           : tr("Minimaler Fenstermodus aus"),
        1800);
    m_app.windowChanged(&m_window);
}

void WindowExtras::windowStateChanged() {
    // Qt sets the level and style again in some transitions (full screen): re-apply - not
    // while the native full screen animation runs (a style change there breaks it); the
    // window calls this again when the animation ended (F-VIEW-10).
    if (m_window.fullScreenAnimating())
        return;
    if (m_onTop)
        setWindowAlwaysOnTop(&m_window, true);
    m_titleStrip->windowStateChanged(); // F-VIEW-13/15
}

bool WindowExtras::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize && m_central && watched == m_central.data()) {
        if (m_filter->isOpen())
            m_filter->place();
    }
    if (event->type() == QEvent::KeyPress && watched != static_cast<QObject*>(m_filter->field())) {
        // F-NAV-08: typing opens the name filter (keys without another meaning).
        auto* ke = static_cast<QKeyEvent*>(event);
        CullingController* culling = m_window.culling();
        const bool chord = culling && culling->quickMove()->isPending();
        if (!chord && !m_window.isSlideshowRunning() && !m_loupe->isActive() &&
            m_filter->maybeStart(ke, ratingKeysActive() || (culling && culling->isActive())))
            return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace lf
