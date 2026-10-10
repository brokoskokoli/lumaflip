// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps several ImageViews on one zoom and visible part (F-GRID-03 compare panes, F-WIN-07
// coupled windows) with the math of core/ViewSync.h. A view changed by the user
// (ImageView::viewTransformChanged) moves the others; with ⌥ held only that view moves
// (its offset is remembered). Views are members by position in the list.

#include "core/ViewSync.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>

namespace lf {

class ImageView;

ViewSyncState captureViewState(const ImageView* view);
void applyViewState(ImageView* view, const ViewSyncState& state);

class ViewSyncBinding : public QObject {
    Q_OBJECT
public:
    explicit ViewSyncBinding(QObject* parent = nullptr);

    // Replaces the members; the shared view starts as the first member's view.
    void setViews(const QList<ImageView*>& views);
    QList<ImageView*> views() const;
    ViewSync& math() { return m_sync; }
    // Whether a change counts as "independent" (default: ⌥ held). Tests replace it.
    void setIndependentTest(std::function<bool()> test) { m_independent = std::move(test); }
    bool isApplying() const { return m_applying; }

signals:
    void synchronized(int sourceMember); // the others followed a change of this member

private:
    void onChanged(ImageView* view);

    QList<QPointer<ImageView>> m_views;
    ViewSync m_sync;
    bool m_applying = false;
    std::function<bool()> m_independent;
};

} // namespace lf
