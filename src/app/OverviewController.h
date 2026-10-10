// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Grid view (F-GRID-02) and compare mode (F-GRID-03) of a viewer window. Both replace the
// image view in the window while they are shown; the window's commands (menu, keys) are
// routed here first (perform()):
// - grid: ⌘+ / ⌘− change the thumbnail size, rating keys rate the selection, ⌘⌫ moves the
//   selection to the trash (one undo step), M/C + 1–9 move/copy the selection, G or Esc
//   closes, Enter opens the image under the cursor;
// - compare: zoom commands and rating keys apply to the focused pane, Esc closes.
// Rating keys are active in both (the digits rate, SPEC 5.2), like in the culling mode.

#include "core/InputBindings.h"

#include <QObject>
#include <QPointF>
#include <QStringList>

class QKeyEvent;
class QLabel;
class QWidget;

namespace lf {

class Application;
class CompareView;
class GridView;
class ViewerWindow;

class OverviewController : public QObject {
    Q_OBJECT
public:
    enum class Mode { Viewer, Grid, Compare };

    OverviewController(ViewerWindow& window, Application& app);
    ~OverviewController() override;

    Mode mode() const { return m_mode; }
    bool isGridShown() const { return m_mode == Mode::Grid; }
    bool isCompareShown() const { return m_mode == Mode::Compare; }

    void showGrid();
    // Back to the viewer; `open` = the image under the grid cursor becomes current.
    void closeGrid(bool open = false);
    void toggleGrid();
    // F-GRID-03: 2–4 images; false (with a message) for fewer than 2.
    bool compare(const QStringList& paths, int focus = 0);
    void compareWithNext(); // grid: the selection (2–4) or cursor + next; viewer: current + next
    void closeCompare();    // back to where the compare mode was started (grid or viewer)
    bool canCompareSelection() const;

    GridView* grid() const { return m_grid; }       // null until first shown
    CompareView* compareView() const { return m_compare; }
    QWidget* gridPanel() const { return m_gridPanel; }
    QString headerText() const; // grid header: count, selection, filters

    // Paths the commands act on: grid selection, focused compare pane, else the current image.
    QStringList subjectPaths() const;
    // Window commands while the grid or compare view is shown; true: handled here.
    bool perform(Action action, QPointF pos);
    // A key the grid/compare view takes before the menu shortcuts (rating keys).
    bool wantsKey(const QKeyEvent* event) const;
    // The rating keys are active (grid or compare shown).
    bool ratingKeysActive() const { return m_mode != Mode::Viewer; }
    void updateHeader();

signals:
    void modeChanged();

private:
    void ensureGrid();
    void ensureCompare();
    void setMode(Mode mode);
    void rate(const QStringList& paths, Action action);
    void trash(const QStringList& paths);
    void onPaneInput(int pane, const InputEvent& event, QPointF pos);
    bool performCompare(Action action, QPointF pos);
    bool performGrid(Action action);

    ViewerWindow& m_window;
    Application& m_app;
    Mode m_mode = Mode::Viewer;
    Mode m_compareReturn = Mode::Viewer;
    QWidget* m_gridPanel = nullptr;
    QLabel* m_gridHeader = nullptr;
    GridView* m_grid = nullptr;
    CompareView* m_compare = nullptr;
};

} // namespace lf
