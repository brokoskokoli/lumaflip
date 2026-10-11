// SPDX-License-Identifier: GPL-3.0-or-later
#include "OverviewController.h"

#include "Application.h"
#include "CompareView.h"
#include "CullingController.h"
#include "GridView.h"
#include "ImageView.h"
#include "QuickMoveController.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileBatch.h"
#include "core/FileOps.h"
#include "core/Rating.h"
#include "core/RatingStore.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"

#include <QBoxLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

#include <cmath>

namespace lf {

OverviewController::OverviewController(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfOverviewController"));
}

OverviewController::~OverviewController() = default;

void OverviewController::ensureGrid() {
    if (m_grid)
        return;
    auto* layout = qobject_cast<QBoxLayout*>(m_window.centralWidget()->layout());
    m_gridPanel = new QWidget(m_window.centralWidget());
    m_gridPanel->setObjectName(QStringLiteral("lfGridPanel"));
    auto* v = new QVBoxLayout(m_gridPanel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_gridHeader = new QLabel(m_gridPanel);
    m_gridHeader->setObjectName(QStringLiteral("lfGridHeader"));
    m_gridHeader->setContentsMargins(12, 6, 12, 6);
    m_gridHeader->setTextFormat(Qt::PlainText);
    m_grid = new GridView(*m_window.collection(), ThumbnailCache::instance(), m_gridPanel);
    m_grid->setExtraKeys([this](const QKeyEvent* e) { return wantsKey(e); });
    v->addWidget(m_gridHeader);
    v->addWidget(m_grid, 1);
    m_gridPanel->hide();
    if (layout)
        layout->addWidget(m_gridPanel, 1);
    connect(m_grid, &GridView::openRequested, this, [this](const QString& path) {
        m_window.collection()->setCurrentPath(path);
        closeGrid(false);
    });
    connect(m_grid, &GridView::closeRequested, this, [this] { closeGrid(false); });
    connect(m_grid, &GridView::gridSelectionChanged, this, [this] {
        updateHeader();
        m_app.windowChanged(&m_window); // menu: "Auswahl vergleichen"
    });
    connect(m_grid, &GridView::thumbSizeChanged, this, &OverviewController::updateHeader);
    Collection* c = m_window.collection();
    connect(c, &Collection::entriesChanged, this, &OverviewController::updateHeader);
    // The current image changed by other means (⌘G, a coupled window): the cursor follows.
    connect(c, &Collection::currentChanged, this, [this] {
        if (m_mode == Mode::Grid && m_grid)
            m_grid->activate();
    });
}

void OverviewController::ensureCompare() {
    if (m_compare)
        return;
    auto* layout = qobject_cast<QBoxLayout*>(m_window.centralWidget()->layout());
    m_compare = new CompareView(m_app.provider(), m_window.centralWidget());
    m_compare->setExtraKeys([this](const QKeyEvent* e) { return wantsKey(e); });
    m_compare->hide();
    if (layout)
        layout->addWidget(m_compare, 1);
    connect(m_compare, &CompareView::closeRequested, this, &OverviewController::closeCompare);
    connect(m_compare, &CompareView::paneInput, this, &OverviewController::onPaneInput);
}

void OverviewController::setMode(Mode mode) {
    if (mode == m_mode)
        return;
    const Mode old = m_mode;
    m_mode = mode;
    ImageView* view = m_window.view();
    if (old == Mode::Compare && m_compare) {
        m_compare->stop();
        m_compare->hide();
    }
    if (old == Mode::Grid && m_gridPanel)
        m_gridPanel->hide();
    switch (mode) {
    case Mode::Viewer:
        view->show();
        m_window.setOverlayHost(view);
        view->setFocus(Qt::OtherFocusReason);
        break;
    case Mode::Grid:
        view->hide();
        m_gridPanel->show();
        m_window.setOverlayHost(m_gridPanel);
        m_grid->activate();
        break;
    case Mode::Compare:
        view->hide();
        m_compare->show();
        m_window.setOverlayHost(m_compare);
        m_compare->setFocus(Qt::OtherFocusReason);
        break;
    }
    updateHeader();
    emit modeChanged();
    m_app.windowChanged(&m_window);
}

void OverviewController::showGrid() {
    if (m_window.collection()->count() == 0 && !m_window.collection()->isLoading()) {
        m_window.showMessage(tr("Keine Bilder für die Rasteransicht"));
        return;
    }
    if (m_window.isSlideshowRunning())
        m_window.stopSlideshow();
    ensureGrid();
    setMode(Mode::Grid);
}

void OverviewController::closeGrid(bool open) {
    if (m_mode != Mode::Grid)
        return;
    if (open && m_grid && !m_grid->cursorPath().isEmpty())
        m_window.collection()->setCurrentPath(m_grid->cursorPath());
    setMode(Mode::Viewer);
}

void OverviewController::toggleGrid() {
    if (m_mode == Mode::Grid)
        closeGrid(false);
    else
        showGrid();
}

bool OverviewController::compare(const QStringList& paths, int focus) {
    if (paths.size() < CompareView::kMinPanes) {
        m_window.showMessage(tr("Wähle zum Vergleichen mindestens zwei Bilder aus"));
        return false;
    }
    if (paths.size() > CompareView::kMaxPanes)
        m_window.showMessage(tr("Es werden die ersten %1 Bilder verglichen").arg(CompareView::kMaxPanes));
    if (m_window.isSlideshowRunning())
        m_window.stopSlideshow();
    ensureCompare();
    if (m_mode != Mode::Compare)
        m_compareReturn = m_mode;
    Settings& st = Settings::instance();
    m_compare->setPaths(paths, focus);
    m_compare->applyViewSettings(st.value(key::Background).toString(),
                                 st.value(key::UpscaleFilter).toString() == QLatin1String("nearest"),
                                 st.value(key::ActualSizeUnit).toString() != QLatin1String("points"));
    setMode(Mode::Compare);
    return true;
}

bool OverviewController::canCompareSelection() const {
    if (m_mode != Mode::Grid || !m_grid)
        return false;
    const int n = m_grid->selection().selectedCount();
    return n >= CompareView::kMinPanes && n <= CompareView::kMaxPanes;
}

void OverviewController::compareWithNext() {
    const Collection* c = m_window.collection();
    if (m_mode == Mode::Grid && m_grid) {
        const QStringList selected = m_grid->selectedPaths();
        if (selected.size() >= CompareView::kMinPanes) {
            compare(selected, int(std::max<qsizetype>(0, selected.indexOf(m_grid->cursorPath()))));
            return;
        }
    }
    if (m_mode == Mode::Compare) {
        closeCompare();
        return;
    }
    const QString from = m_mode == Mode::Grid && m_grid ? m_grid->cursorPath() : c->currentPath();
    const int i = c->indexOf(from);
    if (i < 0 || c->count() < 2) {
        m_window.showMessage(tr("Wähle zum Vergleichen mindestens zwei Bilder aus"));
        return;
    }
    const int next = i + 1 < c->count() ? i + 1 : i - 1;
    compare({c->at(i).path, c->at(next).path}, 0);
}

void OverviewController::closeCompare() {
    if (m_mode != Mode::Compare)
        return;
    setMode(m_compareReturn == Mode::Grid && m_grid ? Mode::Grid : Mode::Viewer);
}

QString OverviewController::headerText() const {
    const Collection* c = m_window.collection();
    QStringList parts;
    parts << tr("%n Bild(er)", nullptr, c->count());
    if (m_grid && m_grid->selection().selectedCount() > 1)
        parts << tr("%n ausgewählt", nullptr, m_grid->selection().selectedCount());
    if (CullingController* cull = m_window.culling())
        if (const QString f = cull->filterStatus(); !f.isEmpty())
            parts << f;
    if (!c->nameFilter().isEmpty())
        parts << tr("Name: „%1“").arg(c->nameFilter());
    return parts.join(QStringLiteral(" · "));
}

void OverviewController::updateHeader() {
    if (m_gridHeader)
        m_gridHeader->setText(headerText());
}

QStringList OverviewController::subjectPaths() const {
    if (m_mode == Mode::Grid && m_grid) {
        QStringList s = m_grid->selectedPaths();
        if (s.isEmpty() && !m_grid->cursorPath().isEmpty())
            s << m_grid->cursorPath();
        return s;
    }
    if (m_mode == Mode::Compare && m_compare)
        return m_compare->focusedPath().isEmpty() ? QStringList() : QStringList{m_compare->focusedPath()};
    const QString cur = m_window.currentPath();
    return cur.isEmpty() ? QStringList() : QStringList{cur};
}

bool OverviewController::wantsKey(const QKeyEvent* event) const {
    InputEvent ev;
    ev.kind = InputKind::Key;
    ev.key = event->key();
    ev.modifiers = event->modifiers();
    ev.inCullingMode = true;
    return rating::isRatingAction(m_app.bindings().resolve(ev));
}

void OverviewController::rate(const QStringList& paths, Action action) {
    if (paths.isEmpty())
        return;
    QPointer<OverviewController> self(this);
    // F-FMT-06: a pair's second file gets the same rating (own sidecar, written after the
    // shown file's). The whole selection is one undo step.
    QStringList all;
    for (const QString& path : paths) {
        all << path;
        if (const QString other = m_window.collection()->pairedPath(path); !other.isEmpty())
            all << other;
    }
    RatingStore::instance().changeRatings(
        m_app.fileOps(), all, [action](const Rating& stored) { return rating::apply(stored, action); },
        [self](bool ok, const QString& error, const QString&, const Rating&) {
            if (self && !ok)
                self->m_window.showMessage(error, 5000);
        });
    if (paths.size() == 1)
        m_window.showMessage(tr("%1: %2").arg(QFileInfo(paths.first()).fileName(), actionLabel(action)),
                             1500);
    else
        m_window.showMessage(tr("%1 – %n Bild(er)", nullptr, int(paths.size())).arg(actionLabel(action)),
                             1500);
}

void OverviewController::trash(const QStringList& selected) {
    if (selected.isEmpty())
        return;
    const QStringList paths = m_window.collection()->withPairs(selected); // F-FMT-06
    QPointer<OverviewController> self(this);
    auto run = [self, paths] {
        if (!self)
            return;
        QVector<TransferItem> items;
        for (const QString& p : paths)
            items.append({p, QString(), ConflictChoice::Skip});
        self->m_window.culling()->quickMove()->transfer(
            TransferKind::Trash, items, tr("In den Papierkorb legen"),
            [](int n) { return tr("%n Bild(er) in den Papierkorb gelegt", nullptr, n); });
    };
    if (!Settings::instance().value(key::ConfirmTrash).toBool()) {
        run();
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Question, tr("In den Papierkorb legen"),
                                tr("%n Bild(er) in den Papierkorb legen?", nullptr, int(paths.size())),
                                QMessageBox::NoButton, &m_window);
    box->setObjectName(QStringLiteral("lfConfirmTrashSelection"));
    box->setInformativeText(tr("Mit ⌘Z lässt sich das widerrufen."));
    QPushButton* yes = box->addButton(tr("In den Papierkorb legen"), QMessageBox::AcceptRole);
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(yes);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    connect(box, &QMessageBox::buttonClicked, this, [run, yes](QAbstractButton* b) {
        if (b == yes)
            run();
    });
    box->open();
}

bool OverviewController::perform(Action a, QPointF pos) {
    if (m_mode == Mode::Viewer) {
        switch (a) {
        case Action::ToggleGrid:
            showGrid();
            return true;
        case Action::CompareWithNext:
            compareWithNext();
            return true;
        default:
            return false;
        }
    }
    return m_mode == Mode::Grid ? performGrid(a) : performCompare(a, pos);
}

bool OverviewController::performGrid(Action a) {
    if (rating::isRatingAction(a)) {
        rate(subjectPaths(), a);
        return true;
    }
    switch (a) {
    case Action::ToggleGrid:
    case Action::ExitModeOrNothing:
    case Action::ExitModeOrCloseWindow:
        closeGrid(false);
        return true;
    case Action::ZoomIn:
        m_grid->zoomStep(1);
        return true;
    case Action::ZoomOut:
        m_grid->zoomStep(-1);
        return true;
    case Action::Trash:
        trash(subjectPaths());
        return true;
    case Action::NextImage:
        m_grid->moveGridCursor(grid::Move::Right, false);
        return true;
    case Action::PreviousImage:
        m_grid->moveGridCursor(grid::Move::Left, false);
        return true;
    case Action::FirstImage:
        m_grid->moveGridCursor(grid::Move::Home, false);
        return true;
    case Action::LastImage:
        m_grid->moveGridCursor(grid::Move::End, false);
        return true;
    case Action::CompareWithNext:
        compareWithNext();
        return true;
    case Action::ToggleSlideshow:
        closeGrid(true);
        return false; // the slideshow starts from the image under the cursor
    case Action::RevealInFinder:
    case Action::CopyImage:
    case Action::CopyPath:
    case Action::OpenInEditor:
        // Commands for one image act on the image under the cursor.
        if (!m_grid->cursorPath().isEmpty())
            m_window.collection()->setCurrentPath(m_grid->cursorPath());
        return false;
    case Action::ZoomActual:
    case Action::ZoomFit:
    case Action::ZoomFillWidth:
    case Action::ToggleFitActualAtCursor:
    case Action::ToggleZoomLock:
    case Action::ScrollUp:
    case Action::ScrollDown:
    case Action::ScrollLeft:
    case Action::ScrollRight:
    case Action::ScrollPageUp:
    case Action::ScrollPageDown:
    case Action::ScrollHome:
    case Action::ScrollEnd:
    case Action::RotateLeft:
    case Action::RotateRight:
    case Action::FlipHorizontal:
    case Action::FlipVertical:
    case Action::SaveRotation:
    case Action::Rename:
    case Action::Magnifier:
    case Action::NextPage:
    case Action::PreviousPage:
    case Action::ToggleAnimation:
    case Action::NextFrame:
    case Action::PreviousFrame:
    case Action::JumpForward:
    case Action::JumpBackward:
    case Action::NextFolder:
    case Action::PreviousFolder:
        return true; // no meaning in the grid
    default:
        return false;
    }
}

bool OverviewController::performCompare(Action a, QPointF) {
    ImageView* v = m_compare ? m_compare->focusedView() : nullptr;
    if (!v)
        return false;
    if (rating::isRatingAction(a)) {
        rate(subjectPaths(), a);
        return true;
    }
    const QPointF center(v->width() / 2.0, v->height() / 2.0);
    const double step = std::max(16.0, v->height() * 0.1);
    switch (a) {
    case Action::ExitModeOrNothing:
    case Action::ExitModeOrCloseWindow:
    case Action::CompareWithNext:
        closeCompare();
        return true;
    case Action::ToggleGrid:
        setMode(Mode::Viewer);
        showGrid();
        return true;
    case Action::ZoomIn:
        v->zoomStep(1);
        return true;
    case Action::ZoomOut:
        v->zoomStep(-1);
        return true;
    case Action::ZoomActual:
        v->setZoomMode(ZoomMode::Actual);
        return true;
    case Action::ZoomFit:
        v->setZoomMode(ZoomMode::Fit);
        return true;
    case Action::ZoomFillWidth:
        v->setZoomMode(ZoomMode::FillWidth);
        return true;
    case Action::ToggleFitActualAtCursor:
        v->toggleFitActual(center); // keys/menu: at the pane's center
        return true;
    case Action::ScrollUp:
        v->panBy(QPointF(0, step));
        return true;
    case Action::ScrollDown:
        v->panBy(QPointF(0, -step));
        return true;
    case Action::ScrollLeft:
        v->panBy(QPointF(step, 0));
        return true;
    case Action::ScrollRight:
        v->panBy(QPointF(-step, 0));
        return true;
    case Action::NextImage:
        m_compare->cycleFocus(1);
        return true;
    case Action::PreviousImage:
        m_compare->cycleFocus(-1);
        return true;
    case Action::ToggleFullScreen:
    case Action::ToggleInfoPanel:
    case Action::ToggleFilmstrip:
    case Action::CloseWindow:
    case Action::CloseAllWindows:
    case Action::NewWindow:
    case Action::DuplicateWindow:
    case Action::OpenPreferences:
    case Action::ShowShortcuts:
    case Action::Undo:
    case Action::Redo:
    case Action::ToggleAlwaysOnTop:
    case Action::ToggleMinimalWindow:
    case Action::CoupleWindows:
    case Action::ToggleCullingMode:
        return false; // window commands work as usual
    case Action::RevealInFinder:
    case Action::CopyPath:
    case Action::CopyImage:
    case Action::OpenInEditor:
        m_window.collection()->setCurrentPath(m_compare->focusedPath());
        return false;
    default:
        return true; // image commands of the viewer have no meaning here
    }
}

void OverviewController::onPaneInput(int pane, const InputEvent& event, QPointF pos) {
    ImageView* v = m_compare ? m_compare->paneView(pane) : nullptr;
    if (!v)
        return;
    InputEvent ev = event;
    ev.inCullingMode = true;
    const InputBindings& b = m_app.bindings();
    const Action a = b.resolve(ev);
    switch (a) {
    case Action::ZoomIn:
    case Action::ZoomOut: {
        const int steps = std::max(1, std::abs(ev.wheelSteps));
        const double f = std::pow(1.0 + b.zoomStepPercent() / 100.0, steps);
        v->zoomBy(a == Action::ZoomIn ? f : 1.0 / f, pos);
        return;
    }
    case Action::ToggleFitActualAtCursor:
        v->toggleFitActual(pos);
        return;
    case Action::ScrollUp:
    case Action::ScrollDown:
    case Action::ScrollLeft:
    case Action::ScrollRight: {
        const double step = std::max(16.0, v->height() * 0.1);
        const QPointF d = a == Action::ScrollUp     ? QPointF(0, step)
                          : a == Action::ScrollDown ? QPointF(0, -step)
                          : a == Action::ScrollLeft ? QPointF(step, 0)
                                                    : QPointF(-step, 0);
        v->panBy(d);
        return;
    }
    case Action::ToggleFullScreen:
    case Action::CloseWindow:
        m_window.perform(a);
        return;
    default:
        return; // flipping etc. has no meaning in a pane
    }
}

} // namespace lf
