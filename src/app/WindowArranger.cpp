// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowArranger.h"

#include "Application.h"
#include "ViewerWindow.h"

#include <QGuiApplication>
#include <QMargins>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

// Splits `length` into `parts` integer slices that cover it exactly.
int sliceStart(int length, int parts, int index) {
    return int((qint64(length) * index) / parts);
}

} // namespace

QSize gridShape(int count) {
    if (count <= 0)
        return {0, 0};
    const int cols = int(std::ceil(std::sqrt(double(count))));
    const int rows = (count + cols - 1) / cols;
    return {cols, rows};
}

QList<QRect> arrangedFrames(Arrangement arrangement, int count, const QRect& area, QSize cascadeSize,
                            int cascadeStep) {
    QList<QRect> out;
    if (count <= 0 || area.isEmpty())
        return out;
    auto cell = [&](int col, int cols, int row, int rows) {
        const int x0 = area.left() + sliceStart(area.width(), cols, col);
        const int x1 = area.left() + sliceStart(area.width(), cols, col + 1);
        const int y0 = area.top() + sliceStart(area.height(), rows, row);
        const int y1 = area.top() + sliceStart(area.height(), rows, row + 1);
        return QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1));
    };
    switch (arrangement) {
    case Arrangement::SideBySide:
        for (int i = 0; i < count; ++i)
            out << cell(i, count, 0, 1);
        break;
    case Arrangement::Stacked:
        for (int i = 0; i < count; ++i)
            out << cell(0, 1, i, count);
        break;
    case Arrangement::Grid: {
        const QSize shape = gridShape(count);
        for (int i = 0; i < count; ++i)
            out << cell(i % shape.width(), shape.width(), i / shape.width(), shape.height());
        break;
    }
    case Arrangement::Cascade: {
        const int step = std::max(1, cascadeStep);
        QSize size = cascadeSize.isEmpty() ? QSize(area.width() * 2 / 3, area.height() * 2 / 3) : cascadeSize;
        size = size.boundedTo(area.size());
        // Restart at the top left when the next step would leave the area.
        const int maxSteps = std::max(
            1, std::min((area.width() - size.width()) / step, (area.height() - size.height()) / step) + 1);
        for (int i = 0; i < count; ++i) {
            const int k = i % maxSteps;
            const int column = i / maxSteps; // later rounds shifted right a little
            QPoint p = area.topLeft() + QPoint(k * step + column * step / 2, k * step);
            p.setX(std::min(p.x(), area.right() - size.width() + 1));
            out << QRect(p, size);
        }
        break;
    }
    }
    return out;
}

void applyFrames(const QList<QWidget*>& windows, const QList<QRect>& frames) {
    for (qsizetype i = 0; i < windows.size() && i < frames.size(); ++i) {
        QWidget* w = windows[i];
        if (w->isMaximized() || w->isFullScreen())
            w->showNormal();
        // Title bar and border: the difference between frame and client geometry.
        const QRect frame = w->frameGeometry();
        const QRect client = w->geometry();
        QMargins m(client.left() - frame.left(), client.top() - frame.top(), frame.right() - client.right(),
                   frame.bottom() - client.bottom());
        if (m.left() < 0 || m.top() < 0 || m.right() < 0 || m.bottom() < 0)
            m = QMargins();
        w->setGeometry(frames[i].marginsRemoved(m));
    }
}

int arrangeViewerWindows(Application& app, Arrangement arrangement) {
    ViewerWindow* front = app.frontWindow();
    if (!front)
        return 0;
    QScreen* screen = front->screen() ? front->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return 0;
    QList<QWidget*> list;
    if (!front->isMinimized() && !front->isFullScreen() && front->isVisible())
        list << front;
    for (ViewerWindow* w : app.windows())
        if (w != front && w->isVisible() && !w->isMinimized() && !w->isFullScreen() && w->screen() == screen)
            list << w;
    if (list.isEmpty())
        return 0;
    const QRect area = screen->availableGeometry();
    QSize cascadeSize;
    if (arrangement == Arrangement::Cascade)
        cascadeSize = front->frameGeometry().size().boundedTo(
            area.size() -
            QSize(kArrangeCascadeStepPx * int(list.size()), kArrangeCascadeStepPx * int(list.size())));
    if (cascadeSize.width() < area.width() / 3 || cascadeSize.height() < area.height() / 3)
        cascadeSize = {}; // too small to be useful: 2/3 of the screen
    applyFrames(list, arrangedFrames(arrangement, int(list.size()), area, cascadeSize));
    // The front window stays in front.
    for (auto it = list.crbegin(); it != list.crend(); ++it)
        (*it)->raise();
    front->activateWindow();
    return int(list.size());
}

} // namespace lf
