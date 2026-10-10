// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Interactive crop (F-EDIT-03) over an ImageView: the selection with eight handles, the
// darkened outside, the rule-of-thirds grid and the size in pixels; a bar below with the
// aspect preset (free, original, 1:1, 4:3, 3:2, 16:9), the portrait toggle, the grid toggle
// and "Speichern" / "Speichern unter …" / "Abbrechen". The selection lives in full-size
// image pixels (EXIF orientation applied, like DecodedImage::fullSize), so it follows zoom,
// pan, the view rotation and mirroring (ImageView::imageToWidget()). Dragging inside moves
// it, outside starts a new one; the wheel and pinch still zoom (forwarded to the canvas).
// Keys (Enter, Esc) are handled by the owner (WindowEditing).

#include "core/CropSave.h"

#include <QPointer>
#include <QRectF>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace lf {

class ImageView;

class CropOverlay : public QWidget {
    Q_OBJECT
public:
    explicit CropOverlay(ImageView* view);

    // Starts with the whole image (or the largest centered rectangle of the preset ratio).
    void reset();
    QRectF imageRect() const { return m_rect; } // full-size image pixels
    void setImageRect(const QRectF& rect);      // clipped to the image
    QRect cropRect() const;                     // rounded, inside the image
    QSize imageSize() const;                    // full size of the shown image

    crop::Aspect aspect() const { return m_aspect; }
    void setAspect(crop::Aspect aspect);
    bool portrait() const { return m_portrait; }
    void setPortrait(bool portrait);
    bool gridShown() const { return m_grid; }
    void setGridShown(bool shown);
    QString sizeText() const; // "1920 × 1080 px"
    void retranslateUi();
    QWidget* bar() const { return m_bar; }

signals:
    void saveRequested(bool saveAs);
    void cancelRequested();
    void rectChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QTransform toWidget() const; // image px -> widget
    QRectF widgetRect() const;
    QRectF widgetBounds() const; // the whole image in widget coordinates
    double screenRatio() const;  // aspect ratio in widget coordinates (0 = free)
    crop::Handle hitTest(QPointF p) const;
    void applyRatio();
    void updateBar();
    void placeBar();

    QPointer<ImageView> m_view;
    QRectF m_rect; // image px
    crop::Aspect m_aspect = crop::Aspect::Free;
    bool m_portrait = false;
    bool m_grid = true;
    crop::Handle m_drag = crop::Handle::None;
    QPointF m_pressPos;
    QRectF m_pressRect; // widget coordinates at the press
    QWidget* m_bar = nullptr;
    QComboBox* m_aspectBox = nullptr;
    QToolButton* m_portraitButton = nullptr;
    QToolButton* m_gridButton = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_saveAs = nullptr;
    QPushButton* m_cancel = nullptr;
};

} // namespace lf
