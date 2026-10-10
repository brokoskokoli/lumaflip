// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompareBlendOverlay.h"

#include "ImageView.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>

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
    connect(&m_timer, &QTimer::timeout, this, &CompareBlendOverlay::recompute);
    connect(first, &ImageView::viewTransformChanged, this, &CompareBlendOverlay::refresh);
    connect(first, &ImageView::zoomChanged, this, &CompareBlendOverlay::refresh);
    hide();
}

void CompareBlendOverlay::setMode(compare::BlendMode mode, double value) {
    m_mode = mode;
    m_value = value;
    setVisible(mode != compare::BlendMode::Off);
    if (isVisible()) {
        raise();
        refresh();
    }
}

void CompareBlendOverlay::refresh() {
    if (m_mode != compare::BlendMode::Off)
        m_timer.start();
}

QImage CompareBlendOverlay::displayable(const DecodedImagePtr& img, DecodedImagePtr* key,
                                        QImage* cache) const {
    if (!img)
        return {};
    if (*key == img)
        return *cache;
    QImage out = img->image;
    // One color space for both images (differences must not come from profiles).
    if (out.colorSpace().isValid() && out.colorSpace() != QColorSpace(QColorSpace::SRgb))
        out = out.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_ARGB32_Premultiplied);
    else
        out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    *key = img;
    *cache = out;
    return out;
}

void CompareBlendOverlay::recompute() {
    if (!m_first || !m_second || m_mode == compare::BlendMode::Off)
        return;
    const DecodedImagePtr a = m_first->image(), b = m_second->image();
    const double dpr = devicePixelRatioF();
    const QSize px = (QSizeF(size()) * dpr).toSize();
    if (!a || px.isEmpty()) {
        m_composed = QImage();
        update();
        return;
    }
    const QSizeF fullA(a->fullSize.isEmpty() ? a->image.size() : a->fullSize);
    const QTransform view = m_first->imageToWidget() * QTransform::fromScale(dpr, dpr);
    auto render = [&](const QImage& img, const QTransform& toFullA) {
        QImage out(px, QImage::Format_ARGB32_Premultiplied);
        out.fill(m_first->backgroundColor());
        if (img.isNull())
            return out;
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(toFullA * view);
        p.drawImage(0, 0, img);
        return out;
    };
    const QImage ia = displayable(a, &m_keyA, &m_cacheA);
    const QImage ib = displayable(b, &m_keyB, &m_cacheB);
    // The second image takes the place of the first (same relative position and size).
    const QTransform ta = QTransform::fromScale(fullA.width() / ia.width(), fullA.height() / ia.height());
    const QTransform tb =
        ib.isNull() ? QTransform()
                    : QTransform::fromScale(fullA.width() / ib.width(), fullA.height() / ib.height());
    m_composed = compare::composite(render(ia, ta), render(ib, tb), m_mode, m_value);
    m_composed.setDevicePixelRatio(dpr);
    ++m_recomputes;
    update();
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
    refresh();
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
