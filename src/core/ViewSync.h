// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Synchronized zoom and pan of several image views: the panes of the compare mode
// (F-GRID-03) and coupled windows (F-WIN-07). Pure math, no widgets.
//
// The group keeps one shared view: zoom mode, zoom factor (1.0 = 100 %) and the visible
// center relative to the image (0..1 in both directions, so images of different size stay
// on the same spot). Every member shows the shared view plus its own offset: a member moved
// with ⌥ ("independent") changes only its offset (and its zoom ratio), the others stay.
// A normal change of one member moves the shared view and every other member follows.

#include <QPointF>
#include <QVector>

namespace lf {

struct ViewSyncState {
    int mode = 0;            // zoom mode of the app (ZoomMode as int), opaque here
    bool customZoom = false; // free factor (else the mode decides the factor per image)
    double factor = 1.0;     // 1.0 = 100 %
    QPointF center{0.5, 0.5};
    friend bool operator==(const ViewSyncState&, const ViewSyncState&) = default;
};

class ViewSync {
public:
    explicit ViewSync(int members = 0);

    void setMemberCount(int count); // new members start without offset
    int memberCount() const { return int(m_offsets.size()); }
    // Shared view before the first change (e.g. the view of the member that started it);
    // offsets and ratios are cleared.
    void reset(const ViewSyncState& shared);
    void removeMember(int member); // a pane/window left; the others keep their offsets

    struct Target {
        int member = -1;
        ViewSyncState state;
    };
    // Member `member` was changed by the user to `state`. `independent` (⌥): only that
    // member's offset/zoom ratio changes and nothing follows. Otherwise the shared view
    // moves and the result lists the new view of every other member.
    QVector<Target> changed(int member, const ViewSyncState& state, bool independent);

    ViewSyncState shared() const { return m_shared; }
    ViewSyncState stateFor(int member) const; // shared view with the member's offset
    QPointF offset(int member) const;
    double zoomRatio(int member) const; // member factor / shared factor (⌥ zoom)

    static QPointF clampCenter(QPointF center); // 0..1

private:
    ViewSyncState m_shared;
    QVector<QPointF> m_offsets;
    QVector<double> m_ratios;
};

} // namespace lf
