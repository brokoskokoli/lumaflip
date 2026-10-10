// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragOut.h"

#include <QColorSpace>
#include <QDrag>
#include <QList>
#include <QPointer>
#include <QUrl>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstring>

namespace lf {

namespace {

QString imageMime() {
    return QStringLiteral("application/x-qt-image");
}

std::function<Qt::DropAction(QDrag*)>& execHook() {
    static std::function<Qt::DropAction(QDrag*)> hook;
    return hook;
}

// Float (HDR) buffers are converted for other apps (like ⌘C, F-FILE-03).
QImage toEightBit(QImage out) {
    switch (out.format()) {
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBX32FPx4:
        return out.colorSpace().isValid()
                   ? out.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_ARGB32)
                   : out.convertToFormat(QImage::Format_ARGB32);
    default:
        return out;
    }
}

// Nearest-neighbour copy of `src` with the longer edge `maxPx`, in the source format. Cost
// ~ output pixels for every format (QImage::scaled may convert the whole image first).
QImage sampled(const QImage& src, int maxPx) {
    const QSize size = src.size().scaled(maxPx, maxPx, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    if (src.depth() < 8)
        return src.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QImage out(size, src.format());
    if (out.isNull())
        return {};
    out.setColorSpace(src.colorSpace());
    const int bpp = src.depth() / 8;
    for (int y = 0; y < size.height(); ++y) {
        const int sy = int((qint64(y) * 2 + 1) * src.height() / (qint64(size.height()) * 2));
        const uchar* in = src.constScanLine(sy);
        uchar* o = out.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int sx = int((qint64(x) * 2 + 1) * src.width() / (qint64(size.width()) * 2));
            std::memcpy(o + qsizetype(x) * bpp, in + qsizetype(sx) * bpp, size_t(bpp));
        }
    }
    return out;
}

// Worker thread.
QImage boundedImage(const QImage& image, int maxPx) {
    if (image.isNull())
        return {};
    QImage out = image;
    if (std::max(out.width(), out.height()) > maxPx)
        out = out.scaled(maxPx, maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return toEightBit(std::move(out));
}

} // namespace

DragMimeData::DragMimeData(const QString& path, const QImage& shown) {
    setUrls({QUrl::fromLocalFile(path)});
    m_hasImage = !shown.isNull();
    if (m_hasImage) // `shown` is shared, not copied: the worker only reads it
        m_image = QtConcurrent::run([shown] { return boundedImage(shown, kDragImageMaxPx); });
}

QStringList DragMimeData::formats() const {
    QStringList f = QMimeData::formats();
    if (m_hasImage && !f.contains(imageMime()))
        f << imageMime();
    return f;
}

bool DragMimeData::isImageReady() const {
    return !m_hasImage || m_image.isFinished();
}

QVariant DragMimeData::retrieveData(const QString& mimeType, QMetaType type) const {
    if (m_hasImage && mimeType == imageMime()) {
        // Asked for by the drop target (via Qt's pasteboard), normally long after the
        // worker finished; otherwise this waits for it.
        const QImage img = m_image.result();
        return img.isNull() ? QVariant() : QVariant(img);
    }
    return QMimeData::retrieveData(mimeType, type);
}

QMimeData* createDragMimeData(const QString& path, const QImage& shown) {
    return new DragMimeData(path, shown);
}

QPixmap dragPixmap(const QImage& shown, qreal devicePixelRatio) {
    if (shown.isNull())
        return {};
    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    const int px = std::max(1, int(kDragThumbnailMaxPt * dpr));
    QImage thumb = shown;
    // GUI thread: sample huge images down first (cost ~ output size), then convert and
    // smooth the small result (cost ~ thumbnail size) instead of the full image.
    if (std::max(thumb.width(), thumb.height()) > 2 * px)
        thumb = sampled(thumb, 2 * px);
    if (thumb.width() > px || thumb.height() > px)
        thumb = thumb.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    thumb = toEightBit(std::move(thumb)); // HDR: converting is costly per pixel - only these
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
