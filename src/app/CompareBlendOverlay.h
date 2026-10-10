// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Difference overlay / blending / wipe of two compare panes (F-GRID-04) over the first
// pane's ImageView. Both images are drawn at the size they are shown - the second one into
// the place of the first (same relative position; the compare sync keeps zoom and position)
// - converted to sRGB and combined on the CPU (core/CompareBlend.h; screen sized, a few ms).
// Recomputed when the view, the images or the mode change; the wheel and pinch still zoom.
// In wipe mode a drag moves the split line.

#include "core/CompareBlend.h"
#include "core/ImageProvider.h"

#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace lf {

class ImageView;

class CompareBlendOverlay : public QWidget {
    Q_OBJECT
public:
    CompareBlendOverlay(ImageView* first, ImageView* second);

    void setMode(compare::BlendMode mode, double value);
    compare::BlendMode mode() const { return m_mode; }
    double value() const { return m_value; }
    void refresh();   // coalesced recomputation
    void recompute(); // now (tests)
    QImage composed() const { return m_composed; }
    int recomputeCount() const { return m_recomputes; }

signals:
    void valueChanged(double value); // wipe line dragged

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QImage displayable(const DecodedImagePtr& img, DecodedImagePtr* cacheKey, QImage* cache) const;

    QPointer<ImageView> m_first;
    QPointer<ImageView> m_second;
    compare::BlendMode m_mode = compare::BlendMode::Off;
    double m_value = 0.5;
    QImage m_composed;
    QTimer m_timer;
    int m_recomputes = 0;
    mutable DecodedImagePtr m_keyA, m_keyB;
    mutable QImage m_cacheA, m_cacheB;
};

} // namespace lf
