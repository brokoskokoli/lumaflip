// SPDX-License-Identifier: GPL-3.0-or-later
#include "HistogramWidget.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

bool isDark(const QPalette& p) {
    return p.color(QPalette::Window).lightnessF() < 0.5;
}

// Closed area under one channel, bins spread over `r`. Heights use a square root scale so
// smaller peaks stay visible next to large ones.
QPainterPath channelPath(const std::array<quint32, Histogram::kBins>& bins, double maxCount,
                         const QRectF& r) {
    QPainterPath path;
    path.moveTo(r.left(), r.bottom());
    const double norm = std::sqrt(std::max(1.0, maxCount));
    for (int i = 0; i < Histogram::kBins; ++i) {
        const double x = r.left() + r.width() * (i + 0.5) / Histogram::kBins;
        const double v = std::min(1.0, std::sqrt(double(bins[i])) / norm);
        path.lineTo(x, r.bottom() - v * r.height());
    }
    path.lineTo(r.right(), r.bottom());
    path.closeSubpath();
    return path;
}

} // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("lfHistogram"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum); // up to sizeHint, down to 48 px
    setAccessibleName(tr("Histogramm"));
}

void HistogramWidget::setHistogram(const Histogram& histogram) {
    m_histogram = histogram;
    m_busy = false;
    m_note.clear();
    update();
}

void HistogramWidget::clear(bool busy) {
    m_histogram = Histogram();
    m_busy = busy;
    m_note.clear();
    update();
}

void HistogramWidget::showNote(const QString& note) {
    m_histogram = Histogram();
    m_busy = false;
    m_note = note;
    update();
}

QSize HistogramWidget::sizeHint() const {
    return {256, 110};
}

QSize HistogramWidget::minimumSizeHint() const {
    return {120, 48}; // shrinks in small windows (the metadata have priority)
}

void HistogramWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        update();
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    const bool dark = isDark(pal);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.setBrush(dark ? QColor(0, 0, 0, 90) : QColor(0, 0, 0, 14));
    p.drawRoundedRect(frame, 4, 4);
    // Quarter lines as orientation.
    p.setPen(QPen(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 30), 1));
    for (int i = 1; i < 4; ++i) {
        const double x = frame.left() + frame.width() * i / 4.0;
        p.drawLine(QPointF(x, frame.top() + 2), QPointF(x, frame.bottom() - 2));
    }
    if (m_histogram.isEmpty()) {
        if (m_busy || !m_note.isEmpty()) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(frame, Qt::AlignCenter, m_busy ? tr("Wird berechnet …") : m_note);
        }
        return;
    }
    const QRectF area = frame.adjusted(2, 4, -2, -2);
    const double maxCount = m_histogram.maxCount();
    // Luminance as a neutral area behind the colors.
    const QColor lumaFill = dark ? QColor(220, 220, 220, 90) : QColor(70, 70, 70, 80);
    p.setPen(Qt::NoPen);
    p.setBrush(lumaFill);
    p.drawPath(channelPath(m_histogram.luma, maxCount, area));
    // RGB: additive on dark backgrounds (overlaps turn white), multiplied on light ones.
    p.setCompositionMode(dark ? QPainter::CompositionMode_Plus : QPainter::CompositionMode_Multiply);
    const int a = dark ? 150 : 170;
    const QColor colors[3] = {dark ? QColor(230, 40, 40, a) : QColor(235, 60, 60, a),
                              dark ? QColor(40, 200, 60, a) : QColor(40, 180, 70, a),
                              dark ? QColor(50, 90, 240, a) : QColor(60, 100, 240, a)};
    const std::array<quint32, Histogram::kBins>* channels[3] = {&m_histogram.red, &m_histogram.green,
                                                                &m_histogram.blue};
    for (int c = 0; c < 3; ++c) {
        p.setBrush(colors[c]);
        p.drawPath(channelPath(*channels[c], maxCount, area));
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // Luminance outline on top.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(dark ? QColor(240, 240, 240, 200) : QColor(40, 40, 40, 200), 1));
    p.drawPath(channelPath(m_histogram.luma, maxCount, area));
}

} // namespace lf
