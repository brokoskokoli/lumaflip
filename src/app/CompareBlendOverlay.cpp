// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompareBlendOverlay.h"

#include "ImageView.h"
#include "ViewSyncBinding.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

CompareBlendOverlay::CompareBlendOverlay(ImageView* first, ImageView* second)
    : QWidget(first), m_first(first), m_second(second) {
    setObjectName(QStringLiteral("lfCompareBlendOverlay"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(first->rect());
    first->installEventFilter(this);
    m_timer.setSingleShot(true);
    m_timer.setInterval(0);
    connect(&m_timer, &QTimer::timeout, this, &CompareBlendOverlay::start);
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &CompareBlendOverlay::finished);
    connect(first, &ImageView::viewTransformChanged, this, &CompareBlendOverlay::refresh);
    connect(first, &ImageView::zoomChanged, this, &CompareBlendOverlay::refresh);
    hide();
}

CompareBlendOverlay::~CompareBlendOverlay() {
    // A running job only holds copies (implicitly shared images); its result is dropped.
    m_watcher.disconnect(this);
}

void CompareBlendOverlay::setSync(ViewSyncBinding* sync) {
    m_sync = sync;
    refresh();
}

void CompareBlendOverlay::setPartner(ImageView* second, int member) {
    if (m_second == second && m_member == member)
        return;
    m_second = second;
    m_member = member;
    refresh();
}

void CompareBlendOverlay::setMode(compare::BlendMode mode, double value) {
    m_mode = mode;
    m_value = value;
    setVisible(mode != compare::BlendMode::Off);
    if (isVisible()) {
        raise();
        m_timer.start(); // mode/value only: the rendered layers stay valid
    }
}

void CompareBlendOverlay::invalidateLayers() {
    ++m_generation;
    m_layersValid = false;
    m_layerA = m_layerB = QImage();
}

void CompareBlendOverlay::refresh() {
    invalidateLayers(); // view, size or images changed (also while off: layers are stale)
    if (m_mode != compare::BlendMode::Off)
        m_timer.start();
}

CompareBlendOverlay::Job CompareBlendOverlay::makeJob() const {
    Job j;
    if (!m_first)
        return j;
    j.a = m_first->image();
    j.b = m_second ? m_second->image() : DecodedImagePtr();
    j.dpr = devicePixelRatioF();
    j.px = (QSizeF(size()) * j.dpr).toSize();
    j.mode = m_mode;
    j.value = m_value;
    if (m_layersValid && m_layerA.size() == j.px) {
        j.layerA = m_layerA;
        j.layerB = m_layerB;
        return j;
    }
    j.view = m_first->imageToWidget() * QTransform::fromScale(j.dpr, j.dpr);
    j.background = m_first->backgroundColor();
    if (m_sync) {
        // ⌥-drag / ⌥-zoom of a single pane (F-GRID-03): the partner keeps its offset.
        ViewSync& math = m_sync->math();
        j.centerDelta = math.offset(m_member) - math.offset(0);
        const double r0 = math.zoomRatio(0);
        j.zoomRatio = r0 > 0 ? math.zoomRatio(m_member) / r0 : 1.0;
    }
    return j;
}

CompareBlendOverlay::Result CompareBlendOverlay::run(const Job& j) {
    Result r;
    if (!j.a || j.a->image.isNull() || j.px.isEmpty())
        return r;
    if (!j.layerA.isNull()) {
        r.layerA = j.layerA;
        r.layerB = j.layerB;
    } else {
        const QImage& ia = j.a->image;
        const QSizeF fullA(j.a->fullSize.isEmpty() ? ia.size() : j.a->fullSize);
        const QTransform ta = QTransform::fromScale(fullA.width() / ia.width(), fullA.height() / ia.height());
        r.layerA = compare::renderLayer(ia, ta * j.view, j.px, j.background);
        if (j.b && !j.b->image.isNull()) {
            const QImage& ib = j.b->image;
            const QSizeF fullB(j.b->fullSize.isEmpty() ? ib.size() : j.b->fullSize);
            const QTransform tb =
                QTransform::fromScale(fullB.width() / ib.width(), fullB.height() / ib.height());
            const QTransform place = compare::alignPartner(
                compare::partnerPlacement(fullA, fullB), j.view, fullB,
                QPointF(j.px.width() / 2.0, j.px.height() / 2.0), j.centerDelta, j.zoomRatio);
            r.layerB = compare::renderLayer(ib, tb * place, j.px, j.background);
        } else {
            r.layerB = compare::renderLayer(QImage(), QTransform(), j.px, j.background);
        }
    }
    r.composed = compare::composite(r.layerA, r.layerB, j.mode, j.value);
    r.composed.setDevicePixelRatio(j.dpr);
    return r;
}

void CompareBlendOverlay::start() {
    if (m_mode == compare::BlendMode::Off)
        return;
    if (m_running) {
        m_timer.stop();
        m_pending = true; // started again when the running job is done
        return;
    }
    m_pending = false;
    const Job job = makeJob();
    if (!job.a || job.px.isEmpty()) {
        m_composed = QImage();
        update();
        return;
    }
    m_running = true;
    m_jobGeneration = m_generation;
    m_jobSeq = ++m_seq;
    m_watcher.setFuture(QtConcurrent::run(&CompareBlendOverlay::run, job));
}

void CompareBlendOverlay::finished() {
    m_running = false;
    const Result r = m_watcher.result();
    if (m_jobSeq > m_appliedSeq) {
        m_appliedSeq = m_jobSeq;
        apply(r, m_jobGeneration);
    }
    if (m_pending)
        start();
}

void CompareBlendOverlay::recompute() {
    m_timer.stop();
    if (m_mode == compare::BlendMode::Off)
        return;
    const Job job = makeJob();
    m_appliedSeq = ++m_seq; // a job still running is older: its result is dropped
    apply(run(job), m_generation);
}

void CompareBlendOverlay::apply(const Result& r, int generation) {
    if (m_mode == compare::BlendMode::Off)
        return;
    if (generation == m_generation && !r.layerA.isNull()) {
        m_layerA = r.layerA;
        m_layerB = r.layerB;
        m_layersValid = true;
    }
    m_composed = r.composed;
    ++m_recomputes;
    update();
    emit composedChanged();
}

void CompareBlendOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (m_composed.isNull()) {
        p.fillRect(rect(), m_first ? m_first->backgroundColor() : Qt::black);
        return;
    }
    p.drawImage(0, 0, m_composed);
    if (m_mode == compare::BlendMode::Wipe) {
        const double x = m_value * width();
        p.setPen(QPen(QColor(0, 0, 0, 150), 3));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
}

void CompareBlendOverlay::mousePressEvent(QMouseEvent* e) {
    if (m_mode != compare::BlendMode::Wipe || e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }
    mouseMoveEvent(e);
}

void CompareBlendOverlay::mouseMoveEvent(QMouseEvent* e) {
    if (m_mode != compare::BlendMode::Wipe || !(e->buttons() & Qt::LeftButton) || width() <= 0) {
        e->ignore();
        return;
    }
    m_value = std::clamp(e->position().x() / width(), 0.0, 1.0);
    emit valueChanged(m_value);
    m_timer.start(); // the split only: the layers stay
}

bool CompareBlendOverlay::event(QEvent* e) {
    bool forward = false;
    switch (e->type()) {
    case QEvent::Wheel:
    case QEvent::NativeGesture:
        forward = true; // zoom and pan keep working
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        forward = m_mode != compare::BlendMode::Wipe; // dragging pans (wipe: moves the line)
        break;
    default:
        break;
    }
    if (forward && m_first && m_first->canvasWidget())
        return QCoreApplication::sendEvent(m_first->canvasWidget(), e);
    return QWidget::event(e);
}

bool CompareBlendOverlay::eventFilter(QObject* watched, QEvent* e) {
    if (watched == m_first && e->type() == QEvent::Resize) {
        setGeometry(m_first->rect());
        refresh();
    }
    return QWidget::eventFilter(watched, e);
}

} // namespace lf
