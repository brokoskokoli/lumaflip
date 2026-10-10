// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Culling and rating in a viewer window (SPEC 4.9):
// - F-CULL-01/02: rating keys 0–5, P, X, U (in the culling mode, or always if the input
//   slot "ratingKeys" says so) write through the RatingStore (asynchronous, ⌘Z undoes);
// - F-CULL-03: optionally the next image follows (key::CullAutoAdvance);
// - F-CULL-04: the rating filter of the window's Collection, shown in the status line
//   ("Filter: ≥ 3 ★ — 12 / 412");
// - F-CULL-05: all rejects to the trash or into "_Rejected/" next to them (one undo step);
// - F-CULL-06: the culling mode with an overlay (rating, pick/reject, large key help) and
//   optionally a locked zoom (key::CullLockZoom); Esc leaves it.
// - F-FILE-08: quick move/copy keys (QuickMoveController).

#include "core/Rating.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>

class QLabel;
class QStatusBar;

namespace lf {

class Application;
class QuickMoveController;
class ViewerWindow;
struct InputEvent;
enum class Action;

class CullingController : public QObject {
    Q_OBJECT
public:
    CullingController(ViewerWindow& window, Application& app);
    ~CullingController() override;

    static CullingController* of(ViewerWindow* window); // null if none (not created yet)

    bool isActive() const { return m_active; }
    void setActive(bool on);

    // A key before the bindings resolve it (quick move sequence); true: consumed.
    bool interceptKey(const InputEvent& event);
    // Rating, culling mode, quick move and Esc (leaves the culling mode); true: handled.
    bool perform(Action action);

    // F-CULL-01: rate the current image (menu and keys).
    void rate(Action action);
    std::optional<Rating> currentRating() const; // known rating of the current image

    // F-CULL-04
    void setFilter(const RatingFilter& filter);
    RatingFilter filter() const;
    QString filterStatus() const; // "Filter: ≥ 3 ★ — 12 / 412", empty without a filter

    // F-CULL-05: `toTrash` false = into "_Rejected/" next to each image.
    void bulkRejects(bool toTrash);

    QuickMoveController* quickMove() const { return m_quick; }

    // Status bar hooks (ViewerWindow): the filter label.
    void attachStatusBar(QStatusBar* bar);
    void updateStatus();
    QWidget* overlay() const; // F-CULL-06 (null until the culling mode was used)

signals:
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onCurrentChanged();
    void onRatingChanged(const QString& path, const Rating& rating);
    void updateOverlay();
    void layoutOverlay();

    ViewerWindow& m_window;
    Application& m_app;
    QuickMoveController* m_quick = nullptr;
    bool m_active = false;
    bool m_zoomWasLocked = false;
    bool m_noticeShown = false;
    QPointer<QWidget> m_overlay;
    QLabel* m_filterLabel = nullptr;
};

} // namespace lf
