// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Draws an RGB + luminance histogram (F-INFO-03) with QPainter. Colors follow the
// widget palette (dark/light mode, F-MAC-02).

#include "core/Histogram.h"

#include <QWidget>

namespace lf {

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    void setHistogram(const Histogram& histogram);
    // No data: "Wird berechnet …" (busy) or an empty frame.
    void clear(bool busy);
    // No histogram possible (unreadable file): `note` instead of the frame contents.
    void showNote(const QString& note);
    QString note() const { return m_note; }
    const Histogram& histogram() const { return m_histogram; }
    bool isBusy() const { return m_busy; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    Histogram m_histogram;
    bool m_busy = false;
    QString m_note;
};

} // namespace lf
