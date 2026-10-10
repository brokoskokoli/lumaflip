// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-07: drag the shown image out of the window into other apps as a file
// (F-IN-14: plain drag or ⌥-drag, depending on the setting). The drag carries the file
// URL (Finder, Mail, Nachrichten …) and, for apps that only accept pixels, the shown
// image (bounded size); the drag pixmap is a thumbnail.
//
// The GUI thread only does cheap work when the drag starts: the URL and a thumbnail made
// by sampling (no smoothing of the full image). The bounded image for other apps is
// scaled and converted on a worker as soon as the drag starts and handed over when a
// drop target asks for it (QMimeData::retrieveData, a promise), normally long done by then.

#include <QFuture>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QString>

#include <functional>

class QDrag;
class QWidget;

namespace lf {

inline constexpr int kDragImageMaxPx = 2048;    // image data for apps without file support
inline constexpr int kDragThumbnailMaxPt = 128; // drag pixmap

// Mime data of a drag-out: the file URL, and image data (`shown` scaled to
// kDragImageMaxPx, HDR buffers converted to 8-bit sRGB) prepared on a worker.
class DragMimeData : public QMimeData {
    Q_OBJECT
public:
    DragMimeData(const QString& path, const QImage& shown);
    QStringList formats() const override;
    // True once the image data is prepared (or there is none).
    bool isImageReady() const;

protected:
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override;

private:
    QFuture<QImage> m_image;
    bool m_hasImage = false;
};

// Caller owns the result (a DragMimeData).
QMimeData* createDragMimeData(const QString& path, const QImage& shown);
// Thumbnail of `shown` (long edge kDragThumbnailMaxPt points at `devicePixelRatio`); cheap
// also for huge images (sampled down first, smoothed only at thumbnail size).
QPixmap dragPixmap(const QImage& shown, qreal devicePixelRatio);
// Starts the drag (nested event loop until the drop) with copy action. Returns the
// action the target took.
Qt::DropAction startDragOut(QWidget* source, const QString& path, const QImage& shown);
// Tests: runs instead of QDrag::exec (which needs a real mouse).
void setDragExecHookForTesting(std::function<Qt::DropAction(QDrag*)> hook);

} // namespace lf
