// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Difference overlay / blending / wipe of two compare panes (F-GRID-04) over the first
// pane's ImageView. The first image is drawn as the first pane shows it; the second one is
// fitted into the place of the first with its own aspect ratio (no stretching), plus the
// offset and own zoom the partner pane got with ⌥ (ViewSync). Both are drawn at the size
// they are shown (screen sized), converted to sRGB and combined (core/CompareBlend.h) on a
// worker thread: the GUI thread never touches full-size pixels. Recomputed when the view, the
// images or the mode change (coalesced, one job at a time; a newer request replaces a running
// one's result). Mode and value changes reuse the last rendered layers. The wheel and pinch
// still zoom; in wipe mode a drag moves the split line.

#include "core/CompareBlend.h"
#include "core/ImageProvider.h"

#include <QFutureWatcher>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace lf {

class ImageView;
class ViewSyncBinding;

class CompareBlendOverlay : public QWidget {
    Q_OBJECT
public:
    CompareBlendOverlay(ImageView* first, ImageView* second);
    ~CompareBlendOverlay() override;

    // The pane shown against the first one; `member`/`sync`: its ⌥ offset and zoom ratio
    // (member 0 = the first pane).
    void setPartner(ImageView* second, int member);
    void setSync(ViewSyncBinding* sync);
    ImageView* partner() const { return m_second; }

    void setMode(compare::BlendMode mode, double value);
    compare::BlendMode mode() const { return m_mode; }
    double value() const { return m_value; }
    void refresh();   // coalesced recomputation on the worker
    void recompute(); // now, synchronously (tests)
    QImage composed() const { return m_composed; }
    int recomputeCount() const { return m_recomputes; }
    bool isBusy() const { return m_running || m_timer.isActive(); }

signals:
    void valueChanged(double value); // wipe line dragged
    void composedChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

public:
    // Everything a job needs, captured on the GUI thread (implicitly shared images).
    struct Job {
        DecodedImagePtr a, b;
        QSize px;
        double dpr = 1.0;
        QTransform view; // A full px -> output px
        QColor background;
        compare::BlendMode mode = compare::BlendMode::Off;
        double value = 0.5;
        QPointF centerDelta;
        double zoomRatio = 1.0;
        QImage layerA, layerB; // already rendered (only mode/value changed)
    };
    struct Result {
        QImage composed, layerA, layerB;
    };
    static Result run(const Job& job);

private:
    void invalidateLayers();
    Job makeJob() const;
    void start();
    void finished();
    void apply(const Result& result, int generation);

    QPointer<ImageView> m_first;
    QPointer<ImageView> m_second;
    QPointer<ViewSyncBinding> m_sync;
    int m_member = 1;
    compare::BlendMode m_mode = compare::BlendMode::Off;
    double m_value = 0.5;
    QImage m_composed;
    QImage m_layerA, m_layerB; // last rendered layers (output size), empty = render again
    QTimer m_timer;
    QFutureWatcher<Result> m_watcher;
    bool m_running = false;
    bool m_pending = false;
    int m_seq = 0, m_jobSeq = 0, m_appliedSeq = 0; // newest result wins
    bool m_layersValid = false;
    int m_generation = 0; // layer geometry/images; results of older generations keep no layers
    int m_jobGeneration = 0;
    int m_recomputes = 0;
};

} // namespace lf
