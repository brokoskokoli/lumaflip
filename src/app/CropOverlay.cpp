// SPDX-License-Identifier: GPL-3.0-or-later
#include "CropOverlay.h"

#include "ImageView.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>

#include <cmath>

namespace lf {

namespace {

constexpr double kHandle = 9.0;     // handle square (logical px)
constexpr double kGrab = 12.0;      // grab distance around edges/corners
constexpr double kMinWidget = 12.0; // smallest selection on screen

const crop::Aspect kAspects[] = {crop::Aspect::Free, crop::Aspect::Original, crop::Aspect::Square,
                                 crop::Aspect::R4x3, crop::Aspect::R3x2,     crop::Aspect::R16x9};

} // namespace

CropOverlay::CropOverlay(ImageView* view) : QWidget(view), m_view(view) {
    setObjectName(QStringLiteral("lfCropOverlay"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(view->rect());
    view->installEventFilter(this);

    m_bar = new QWidget(this);
    m_bar->setObjectName(QStringLiteral("lfCropBar"));
    m_bar->setAttribute(Qt::WA_StyledBackground);
    m_bar->setStyleSheet(
        QStringLiteral("#lfCropBar { background: rgba(24, 24, 24, 225); border-radius: 10px; "
                       "border: 1px solid rgba(255, 255, 255, 40); }"
                       "#lfCropBar QLabel { color: #f2f2f2; background: transparent; }"
                       "#lfCropBar QToolButton, #lfCropBar QPushButton { color: #f2f2f2; background: "
                       "rgba(255, 255, 255, 28); "
                       "border: 1px solid rgba(255, 255, 255, 80); border-radius: 6px; padding: 3px 10px; }"
                       "#lfCropBar QToolButton:checked { background: rgba(255, 255, 255, 80); }"
                       "#lfCropBar QPushButton:default { background: rgba(80, 140, 255, 160); }"));
    auto* row = new QHBoxLayout(m_bar);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(8);
    m_aspectBox = new QComboBox(m_bar);
    m_aspectBox->setObjectName(QStringLiteral("lfCropAspect"));
    m_aspectBox->setFocusPolicy(Qt::NoFocus);
    m_portraitButton = new QToolButton(m_bar);
    m_portraitButton->setObjectName(QStringLiteral("lfCropPortrait"));
    m_portraitButton->setCheckable(true);
    m_portraitButton->setFocusPolicy(Qt::NoFocus);
    m_gridButton = new QToolButton(m_bar);
    m_gridButton->setObjectName(QStringLiteral("lfCropGrid"));
    m_gridButton->setCheckable(true);
    m_gridButton->setChecked(true);
    m_gridButton->setFocusPolicy(Qt::NoFocus);
    m_sizeLabel = new QLabel(m_bar);
    m_sizeLabel->setObjectName(QStringLiteral("lfCropSize"));
    m_sizeLabel->setMinimumWidth(110);
    m_save = new QPushButton(m_bar);
    m_save->setObjectName(QStringLiteral("lfCropSave"));
    m_save->setDefault(true);
    m_save->setFocusPolicy(Qt::NoFocus);
    m_saveAs = new QPushButton(m_bar);
    m_saveAs->setObjectName(QStringLiteral("lfCropSaveAs"));
    m_saveAs->setFocusPolicy(Qt::NoFocus);
    m_cancel = new QPushButton(m_bar);
    m_cancel->setObjectName(QStringLiteral("lfCropCancel"));
    m_cancel->setFocusPolicy(Qt::NoFocus);
    for (QWidget* w : std::initializer_list<QWidget*>{m_aspectBox, m_portraitButton, m_gridButton,
                                                      m_sizeLabel, m_save, m_saveAs, m_cancel})
        row->addWidget(w);
    retranslateUi();
    connect(m_aspectBox, &QComboBox::activated, this,
            [this](int index) { setAspect(crop::aspectFromId(m_aspectBox->itemData(index).toString())); });
    connect(m_portraitButton, &QToolButton::toggled, this, &CropOverlay::setPortrait);
    connect(m_gridButton, &QToolButton::toggled, this, &CropOverlay::setGridShown);
    connect(m_save, &QPushButton::clicked, this, [this] { emit saveRequested(false); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] { emit saveRequested(true); });
    connect(m_cancel, &QPushButton::clicked, this, &CropOverlay::cancelRequested);
    connect(view, &ImageView::viewTransformChanged, this, qOverload<>(&QWidget::update));
    connect(view, &ImageView::zoomChanged, this, qOverload<>(&QWidget::update));
}

void CropOverlay::retranslateUi() {
    const QString current = crop::aspectId(m_aspect);
    m_aspectBox->clear();
    for (crop::Aspect a : kAspects)
        m_aspectBox->addItem(crop::aspectLabel(a), crop::aspectId(a));
    m_aspectBox->setCurrentIndex(std::max(0, m_aspectBox->findData(current)));
    m_aspectBox->setToolTip(tr("Seitenverhältnis"));
    m_portraitButton->setText(tr("Hochformat"));
    m_portraitButton->setToolTip(tr("Seitenverhältnis hochkant statt quer"));
    m_gridButton->setText(tr("Drittel"));
    m_gridButton->setToolTip(tr("Raster der Drittel-Regel einblenden"));
    m_save->setText(tr("Speichern"));
    m_save->setToolTip(tr("Zuschnitt in der Datei speichern (Enter, widerrufbar)"));
    m_saveAs->setText(tr("Speichern unter …"));
    m_saveAs->setToolTip(tr("Zuschnitt als neue Datei speichern – das Original bleibt unverändert"));
    m_cancel->setText(tr("Abbrechen"));
    m_cancel->setToolTip(tr("Zuschneiden beenden (Esc)"));
    updateBar();
}

QSize CropOverlay::imageSize() const {
    const auto img = m_view ? m_view->image() : nullptr;
    if (!img)
        return {};
    return img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
}

QTransform CropOverlay::toWidget() const {
    return m_view ? m_view->imageToWidget() : QTransform();
}

QRectF CropOverlay::widgetRect() const {
    return toWidget().mapRect(m_rect);
}

QRectF CropOverlay::widgetBounds() const {
    return toWidget().mapRect(QRectF(QPointF(0, 0), QSizeF(imageSize())));
}

double CropOverlay::screenRatio() const {
    const double r = crop::aspectRatio(m_aspect, QSizeF(imageSize()), m_portrait);
    if (r <= 0)
        return 0;
    const QTransform t = toWidget();
    const bool swapped = std::abs(t.m12()) > std::abs(t.m11()); // view rotated by 90°/270°
    return swapped ? 1.0 / r : r;
}

void CropOverlay::reset() {
    const QSizeF size(imageSize());
    m_rect = QRectF(QPointF(0, 0), size);
    applyRatio();
}

void CropOverlay::setImageRect(const QRectF& rect) {
    m_rect = rect.intersected(QRectF(QPointF(0, 0), QSizeF(imageSize())));
    updateBar();
    update();
    emit rectChanged();
}

QRect CropOverlay::cropRect() const {
    const QRect r(QPoint(int(std::lround(m_rect.left())), int(std::lround(m_rect.top()))),
                  QPoint(int(std::lround(m_rect.right())) - 1, int(std::lround(m_rect.bottom())) - 1));
    return r.intersected(QRect(QPoint(0, 0), imageSize()));
}

void CropOverlay::applyRatio() {
    const double r = crop::aspectRatio(m_aspect, QSizeF(imageSize()), m_portrait);
    if (r > 0)
        m_rect = crop::fitRatio(m_rect.isEmpty() ? QRectF(QPointF(0, 0), QSizeF(imageSize())) : m_rect, r);
    setImageRect(m_rect);
}

void CropOverlay::setAspect(crop::Aspect aspect) {
    m_aspect = aspect;
    m_aspectBox->setCurrentIndex(std::max(0, m_aspectBox->findData(crop::aspectId(aspect))));
    // A new ratio starts from the whole image (the largest rectangle of that ratio).
    m_rect = QRectF(QPointF(0, 0), QSizeF(imageSize()));
    applyRatio();
}

void CropOverlay::setPortrait(bool portrait) {
    if (m_portrait == portrait)
        return;
    m_portrait = portrait;
    m_portraitButton->setChecked(portrait);
    if (m_aspect != crop::Aspect::Free) {
        m_rect = QRectF(QPointF(0, 0), QSizeF(imageSize()));
        applyRatio();
    }
}

void CropOverlay::setGridShown(bool shown) {
    m_grid = shown;
    m_gridButton->setChecked(shown);
    update();
}

QString CropOverlay::sizeText() const {
    const QRect r = cropRect();
    //: crop size in pixels, e.g. "1920 × 1080 px"
    return tr("%1 × %2 px").arg(r.width()).arg(r.height());
}

void CropOverlay::updateBar() {
    m_sizeLabel->setText(sizeText());
    m_bar->adjustSize();
    placeBar();
}

void CropOverlay::placeBar() {
    const QSize s = m_bar->sizeHint();
    m_bar->setGeometry((width() - s.width()) / 2, height() - s.height() - 16, s.width(), s.height());
    m_bar->raise();
}

crop::Handle CropOverlay::hitTest(QPointF p) const {
    const QRectF r = widgetRect();
    const bool nearL = std::abs(p.x() - r.left()) <= kGrab, nearR = std::abs(p.x() - r.right()) <= kGrab;
    const bool nearT = std::abs(p.y() - r.top()) <= kGrab, nearB = std::abs(p.y() - r.bottom()) <= kGrab;
    const bool inX = p.x() >= r.left() - kGrab && p.x() <= r.right() + kGrab;
    const bool inY = p.y() >= r.top() - kGrab && p.y() <= r.bottom() + kGrab;
    if (nearL && nearT)
        return crop::Handle::TopLeft;
    if (nearR && nearT)
        return crop::Handle::TopRight;
    if (nearL && nearB)
        return crop::Handle::BottomLeft;
    if (nearR && nearB)
        return crop::Handle::BottomRight;
    if (nearL && inY)
        return crop::Handle::Left;
    if (nearR && inY)
        return crop::Handle::Right;
    if (nearT && inX)
        return crop::Handle::Top;
    if (nearB && inX)
        return crop::Handle::Bottom;
    if (r.contains(p))
        return crop::Handle::Move;
    return crop::Handle::None;
}

void CropOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRectF r = widgetRect();
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    QPainterPath inside;
    inside.addRect(r);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));
    if (m_grid) {
        p.setPen(QPen(QColor(255, 255, 255, 110), 1));
        const auto th = crop::thirds(r);
        p.drawLine(QPointF(th[0], r.top()), QPointF(th[0], r.bottom()));
        p.drawLine(QPointF(th[1], r.top()), QPointF(th[1], r.bottom()));
        p.drawLine(QPointF(r.left(), th[2]), QPointF(r.right(), th[2]));
        p.drawLine(QPointF(r.left(), th[3]), QPointF(r.right(), th[3]));
    }
    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
    p.drawRect(r);
    p.setPen(QPen(Qt::white, 1));
    p.drawRect(r);
    p.setBrush(Qt::white);
    p.setPen(QPen(QColor(0, 0, 0, 180), 1));
    const QPointF c = r.center();
    for (const QPointF& h :
         {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight(), QPointF(c.x(), r.top()),
          QPointF(c.x(), r.bottom()), QPointF(r.left(), c.y()), QPointF(r.right(), c.y())})
        p.drawRect(QRectF(h.x() - kHandle / 2, h.y() - kHandle / 2, kHandle, kHandle));
    // Size above the selection (inside when there is no room above).
    const QString text = sizeText();
    const QFontMetrics fm(font());
    const QRectF label(r.left(), r.top() - fm.height() - 8 >= 0 ? r.top() - fm.height() - 8 : r.top() + 4,
                       fm.horizontalAdvance(text) + 12, fm.height() + 4);
    QPainterPath pill;
    pill.addRoundedRect(label, 4, 4);
    p.fillPath(pill, QColor(20, 20, 20, 190));
    p.setPen(Qt::white);
    p.drawText(label, Qt::AlignCenter, text);
}

void CropOverlay::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || imageSize().isEmpty()) {
        e->ignore();
        return;
    }
    m_pressPos = e->position();
    m_pressRect = widgetRect();
    m_drag = hitTest(e->position());
    if (m_drag == crop::Handle::None) {
        if (!widgetBounds().contains(e->position())) {
            m_drag = crop::Handle::None;
            return;
        }
        m_drag = crop::Handle::New;
    }
    e->accept();
}

void CropOverlay::mouseMoveEvent(QMouseEvent* e) {
    if (m_drag == crop::Handle::None) {
        switch (hitTest(e->position())) {
        case crop::Handle::TopLeft:
        case crop::Handle::BottomRight:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case crop::Handle::TopRight:
        case crop::Handle::BottomLeft:
            setCursor(Qt::SizeBDiagCursor);
            break;
        case crop::Handle::Left:
        case crop::Handle::Right:
            setCursor(Qt::SizeHorCursor);
            break;
        case crop::Handle::Top:
        case crop::Handle::Bottom:
            setCursor(Qt::SizeVerCursor);
            break;
        case crop::Handle::Move:
            setCursor(Qt::SizeAllCursor);
            break;
        default:
            setCursor(Qt::CrossCursor);
            break;
        }
        return;
    }
    const QRectF next = crop::dragRect(m_pressRect, m_drag, e->position() - m_pressPos, widgetBounds(),
                                       screenRatio(), kMinWidget, m_pressPos);
    setImageRect(toWidget().inverted().mapRect(next));
}

void CropOverlay::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton)
        m_drag = crop::Handle::None;
}

bool CropOverlay::event(QEvent* e) {
    switch (e->type()) {
    case QEvent::Wheel:
    case QEvent::NativeGesture:
        // Zoom and pan keep working while cropping.
        if (m_view && m_view->canvasWidget())
            return QCoreApplication::sendEvent(m_view->canvasWidget(), e);
        break;
    case QEvent::LanguageChange:
        retranslateUi();
        break;
    default:
        break;
    }
    return QWidget::event(e);
}

bool CropOverlay::eventFilter(QObject* watched, QEvent* e) {
    if (watched == m_view && e->type() == QEvent::Resize) {
        setGeometry(m_view->rect());
        placeBar();
    }
    return QWidget::eventFilter(watched, e);
}

} // namespace lf
