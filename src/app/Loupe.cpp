// SPDX-License-Identifier: GPL-3.0-or-later
#include "Loupe.h"

#include "ImageView.h"
#include "ViewerWindow.h"
#include "core/Settings.h"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

bool isFloatFormat(QImage::Format f) {
    return f == QImage::Format_RGBA16FPx4 || f == QImage::Format_RGBA16FPx4_Premultiplied ||
           f == QImage::Format_RGBX16FPx4 || f == QImage::Format_RGBA32FPx4 ||
           f == QImage::Format_RGBA32FPx4_Premultiplied || f == QImage::Format_RGBX32FPx4;
}

// The part of the stage to draw, as an 8-bit sRGB image (QPainter draws device values).
QImage drawable(const QImage& stage, const QRect& rect) {
    QImage crop = stage.copy(rect);
    const QColorSpace cs = crop.colorSpace();
    if (cs.isValid() && cs != QColorSpace(QColorSpace::SRgb))
        return crop.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_ARGB32_Premultiplied);
    if (isFloatFormat(crop.format()))
        return crop.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return crop;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------------------

LoupeWidget::LoupeWidget(ImageView* view, QWidget* parent) : QWidget(parent), m_view(view) {
    setObjectName(QStringLiteral("lfLoupe"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
}

double LoupeWidget::magnification() const {
    return loupe::magnificationFor(Settings::instance().value(key::LoupeZoom).toString());
}

int LoupeWidget::diameter() const {
    return loupe::diameterFor(Settings::instance().value(key::LoupeSize).toString());
}

void LoupeWidget::setCursorPos(QPointF pos) {
    const QRectF old = loupeGeometry().target.adjusted(-4, -4, 4, 4);
    m_pos = pos;
    update(old.toAlignedRect());
    update(loupeGeometry().target.adjusted(-4, -4, 4, 4).toAlignedRect());
}

loupe::Geometry LoupeWidget::loupeGeometry() const {
    if (!m_view || !m_view->image())
        return loupe::compute(QTransform(), QSize(), m_pos, diameter(), 1.0, magnification(), 1.0);
    const DecodedImagePtr img = m_view->image();
    const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
    return loupe::compute(m_view->imageToWidget(), full, m_pos, diameter(), m_view->zoomFactor(),
                          magnification(), m_view->devicePixelRatioF());
}

bool LoupeWidget::fullResolutionWanted() const {
    if (!m_view || !m_view->image())
        return false;
    const DecodedImagePtr img = m_view->image();
    const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
    return loupe::needsFullResolution(img->image.size(), full, loupeGeometry().deviceScale);
}

void LoupeWidget::paintEvent(QPaintEvent*) {
    if (!m_view)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const loupe::Geometry g = loupeGeometry();
    QPainterPath circle;
    circle.addEllipse(g.target);
    p.save();
    p.setClipPath(circle);
    p.fillRect(g.target, m_view->backgroundColor());
    const DecodedImagePtr img = m_view->image();
    if (img && !img->image.isNull() && !g.source.isEmpty()) {
        const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
        const double sx = double(img->image.width()) / full.width();
        const double sy = double(img->image.height()) / full.height();
        const QRect stageRect =
            QRectF(g.source.x() * sx, g.source.y() * sy, g.source.width() * sx, g.source.height() * sy)
                .toAlignedRect()
                .adjusted(-1, -1, 1, 1)
                .intersected(img->image.rect());
        if (!stageRect.isEmpty()) {
            const QImage crop = drawable(img->image, stageRect);
            const QRectF inFull(stageRect.x() / sx, stageRect.y() / sy, stageRect.width() / sx,
                                stageRect.height() / sy);
            // Magnified pixels stay sharp (pixel peeping); a coarse stage is smoothed.
            p.setRenderHint(QPainter::SmoothPixmapTransform, g.deviceScale / sx < 1.0);
            p.setTransform(g.transform, true);
            p.drawImage(inFull, crop);
        }
    }
    p.restore();
    // Ring and the magnification.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3));
    p.drawEllipse(g.target);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
    p.drawEllipse(g.target);
    QFont f = font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    //: magnification shown at the bottom of the loupe, e.g. "200 %"
    const QString label = tr("%1 %").arg(qRound(magnification() * 100));
    const QFontMetricsF fm(f);
    const QRectF lr(g.target.center().x() - fm.horizontalAdvance(label) / 2 - 6,
                    g.target.bottom() - fm.height() - 10, fm.horizontalAdvance(label) + 12, fm.height() + 2);
    QPainterPath pill;
    pill.addRoundedRect(lr, lr.height() / 2, lr.height() / 2);
    p.fillPath(pill, QColor(20, 20, 20, 170));
    p.setPen(Qt::white);
    p.drawText(lr, Qt::AlignCenter, label);
}

// ---------------------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------------------

LoupeController::LoupeController(ViewerWindow& window) : QObject(&window), m_window(window) {
    setObjectName(QStringLiteral("lfLoupeController"));
    connect(&window, &ViewerWindow::imageShown, this, [this] {
        if (m_active && m_widget) {
            m_widget->update();
            requestFullIfNeeded();
        }
    });
}

LoupeController::~LoupeController() = default;

void LoupeController::begin(QPointF pos) {
    ImageView* view = m_window.view();
    if (!view->isVisible() || !view->image())
        return;
    if (pos.x() < 0)
        pos = view->mapFromGlobal(QCursor::pos());
    if (!m_widget) {
        m_widget = new LoupeWidget(view, view);
        m_widget->hide();
    }
    if (!m_active) {
        m_active = true;
        m_fullRequested = false;
        m_byMouse = QGuiApplication::mouseButtons() & Qt::MiddleButton;
        view->installEventFilter(this);
        for (QWidget* child : view->findChildren<QWidget*>())
            if (child != m_widget)
                child->installEventFilter(this);
        m_widget->setGeometry(view->rect());
        m_widget->show();
        m_widget->raise();
        QGuiApplication::setOverrideCursor(Qt::CrossCursor);
    }
    follow(pos);
}

void LoupeController::end() {
    if (!m_active)
        return;
    m_active = false;
    ImageView* view = m_window.view();
    view->removeEventFilter(this);
    for (QWidget* child : view->findChildren<QWidget*>())
        child->removeEventFilter(this);
    if (m_widget)
        m_widget->hide();
    QGuiApplication::restoreOverrideCursor();
}

void LoupeController::follow(QPointF pos) {
    if (!m_widget)
        return;
    m_widget->setCursorPos(pos);
    requestFullIfNeeded();
}

void LoupeController::requestFullIfNeeded() {
    if (m_fullRequested || !m_widget || !m_widget->fullResolutionWanted())
        return;
    m_fullRequested = true;
    // The window loads the full resolution for the view exactly as for zooming in
    // (ViewerWindow: needsHigherResolution -> Quality::Full); imageShown() repaints.
    emit m_window.view()->needsHigherResolution();
}

bool LoupeController::eventFilter(QObject* watched, QEvent* event) {
    if (!m_active)
        return QObject::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        follow(m_window.view()->mapFromGlobal(me->globalPosition()));
        break;
    }
    case QEvent::MouseButtonRelease:
        if (m_byMouse && static_cast<QMouseEvent*>(event)->button() == Qt::MiddleButton)
            end();
        break;
    case QEvent::KeyRelease: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!m_byMouse && !ke->isAutoRepeat())
            end();
        break;
    }
    case QEvent::FocusOut:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
        if (watched == m_window.view())
            end();
        break;
    case QEvent::Resize:
        if (m_widget && watched == m_window.view())
            m_widget->setGeometry(m_window.view()->rect());
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace lf
