// SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewSync.h"

#include <algorithm>
#include <cmath>

namespace lf {

ViewSync::ViewSync(int members) {
    setMemberCount(members);
}

void ViewSync::setMemberCount(int count) {
    count = std::max(0, count);
    m_offsets.resize(count, QPointF());
    m_ratios.resize(count, 1.0);
}

void ViewSync::reset(const ViewSyncState& shared) {
    m_shared = shared;
    m_shared.center = clampCenter(shared.center);
    std::fill(m_offsets.begin(), m_offsets.end(), QPointF());
    std::fill(m_ratios.begin(), m_ratios.end(), 1.0);
}

void ViewSync::removeMember(int member) {
    if (member < 0 || member >= m_offsets.size())
        return;
    m_offsets.removeAt(member);
    m_ratios.removeAt(member);
}

QPointF ViewSync::clampCenter(QPointF c) {
    return QPointF(std::clamp(c.x(), 0.0, 1.0), std::clamp(c.y(), 0.0, 1.0));
}

QPointF ViewSync::offset(int member) const {
    return member >= 0 && member < m_offsets.size() ? m_offsets.at(member) : QPointF();
}

double ViewSync::zoomRatio(int member) const {
    return member >= 0 && member < m_ratios.size() ? m_ratios.at(member) : 1.0;
}

ViewSyncState ViewSync::stateFor(int member) const {
    ViewSyncState s = m_shared;
    s.center = clampCenter(m_shared.center + offset(member));
    const double r = zoomRatio(member);
    if (std::abs(r - 1.0) > 1e-9) {
        s.factor = m_shared.factor * r;
        s.customZoom = true; // a member zoomed on its own keeps a free factor
    }
    return s;
}

QVector<ViewSync::Target> ViewSync::changed(int member, const ViewSyncState& state, bool independent) {
    QVector<Target> out;
    if (member < 0 || member >= m_offsets.size())
        return out;
    const ViewSyncState s{state.mode, state.customZoom, state.factor, clampCenter(state.center)};
    if (independent) {
        // ⌥: this member moves on its own, the shared view stays.
        m_offsets[member] = s.center - m_shared.center;
        if (s.customZoom && m_shared.factor > 0)
            m_ratios[member] = s.factor / m_shared.factor;
        else if (!s.customZoom)
            m_ratios[member] = 1.0;
        return out;
    }
    // The shared view is what this member shows minus its own offset/ratio.
    m_shared.center = clampCenter(s.center - m_offsets.at(member));
    m_shared.mode = s.mode;
    m_shared.customZoom = s.customZoom;
    const double r = m_ratios.at(member);
    m_shared.factor = s.customZoom && r > 0 ? s.factor / r : s.factor;
    if (!s.customZoom) {
        // A zoom mode (Fit …) ends every member's own zoom: all show the mode again.
        std::fill(m_ratios.begin(), m_ratios.end(), 1.0);
    }
    out.reserve(m_offsets.size() - 1);
    for (int i = 0; i < m_offsets.size(); ++i)
        if (i != member)
            out.append({i, stateFor(i)});
    return out;
}

} // namespace lf
