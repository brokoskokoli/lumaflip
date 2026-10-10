// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragOut.h"

#include <QColorSpace>
#include <QDrag>
#include <QList>
#include <QMimeData>
#include <QPointer>
#include <QUrl>
#include <QWidget>

#include <algorithm>

namespace lf {

namespace {

std::function<Qt::DropAction(QDrag*)>& execHook() {
    static std::function<Qt::DropAction(QDrag*)> hook;
    return hook;
}

QImage boundedImage(const QImage& image, int maxPx) {
    if (image.isNull())
        return {};
    QImage out = image;
    if (std::max(out.width(), out.height()) > maxPx)
        out = out.scaled(maxPx, maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Float (HDR) buffers are converted for other apps (like ⌘C, F-FILE-03).
    switch (out.format()) {
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBX32FPx4:
        out = out.colorSpace().isValid() ? out.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_ARGB32)
                                         : out.convertToFormat(QImage::Format_ARGB32);
        break;
    default:
        break;
    }
    return out;
}

} // namespace

QMimeData* createDragMimeData(const QString& path, const QImage& shown) {
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    const QImage image = boundedImage(shown, kDragImageMaxPx);
    if (!image.isNull())
        mime->setImageData(image);
    return mime;
}

QPixmap dragPixmap(const QImage& shown, qreal devicePixelRatio) {
    if (shown.isNull())
        return {};
    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    const int px = int(kDragThumbnailMaxPt * dpr);
    QImage thumb = boundedImage(shown, std::max(px, 1));
    if (thumb.width() > px || thumb.height() > px)
        thumb = thumb.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pm = QPixmap::fromImage(thumb);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

Qt::DropAction startDragOut(QWidget* source, const QString& path, const QImage& shown) {
    if (!source || path.isEmpty())
        return Qt::IgnoreAction;
    auto* drag = new QDrag(source);
    drag->setMimeData(createDragMimeData(path, shown));
    const QPixmap pm = dragPixmap(shown, source->devicePixelRatioF());
    if (!pm.isNull()) {
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(int(pm.width() / pm.devicePixelRatio() / 2),
                                int(pm.height() / pm.devicePixelRatio() / 2)));
    }
    const QPointer<QDrag> guard(drag);
    const Qt::DropAction result = execHook() ? execHook()(drag) : drag->exec(Qt::CopyAction, Qt::CopyAction);
    if (guard)
        guard->deleteLater();
    return result;
}

void setDragExecHookForTesting(std::function<Qt::DropAction(QDrag*)> hook) {
    execHook() = std::move(hook);
}

} // namespace lf
