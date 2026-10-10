// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-07: drag the shown image out of the window into other apps as a file
// (F-IN-14: plain drag or ⌥-drag, depending on the setting). The drag carries the file
// URL (Finder, Mail, Nachrichten …) and, for apps that only accept pixels, the shown
// image (bounded size); the drag pixmap is a thumbnail.

#include <QImage>
#include <QPixmap>
#include <QString>

#include <functional>

class QDrag;
class QMimeData;
class QWidget;

namespace lf {

inline constexpr int kDragImageMaxPx = 2048;    // image data for apps without file support
inline constexpr int kDragThumbnailMaxPt = 128; // drag pixmap

// Mime data of a drag-out: file URL + image data (`shown` scaled to kDragImageMaxPx,
// HDR buffers converted to 8-bit sRGB). Caller owns the result.
QMimeData* createDragMimeData(const QString& path, const QImage& shown);
// Thumbnail of `shown` (long edge kDragThumbnailMaxPt points at `devicePixelRatio`).
QPixmap dragPixmap(const QImage& shown, qreal devicePixelRatio);
// Starts the drag (nested event loop until the drop) with copy action. Returns the
// action the target took.
Qt::DropAction startDragOut(QWidget* source, const QString& path, const QImage& shown);
// Tests: runs instead of QDrag::exec (which needs a real mouse).
void setDragExecHookForTesting(std::function<Qt::DropAction(QDrag*)> hook);

} // namespace lf
