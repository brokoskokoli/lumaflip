// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageView.h"

#include "DisplayColorSpace.h"
#include "ImageCanvas.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointer>
#include <QPushButton>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>

namespace lf {

namespace {

// Trackpad thresholds (logical px accumulated within one gesture).
constexpr double kSwipeThreshold = 50.0;
constexpr double kTrackpadStepThreshold = 80.0;

const QColor kDark(30, 30, 30);
const QColor kLight(236, 236, 236);
const QColor kChecker1(255, 255, 255);
const QColor kChecker2(204, 204, 204);
constexpr double kCheckerCellPt = 8.0;

class ErrorCard : public QFrame {
public:
    explicit ErrorCard(QWidget* parent) : QFrame(parent) {
        setObjectName(QStringLiteral("lfErrorCard"));
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral("#lfErrorCard { background: rgba(20, 20, 20, 225); border-radius: 12px; "
                                     "border: 1px solid rgba(255, 255, 255, 40); }"
                                     "#lfErrorCard QLabel { color: #f2f2f2; background: transparent; }"
                                     "#lfErrorCard QPushButton { color: #f2f2f2; background: rgba(255, 255, 255, 28); "
                                     "border: 1px solid rgba(255, 255, 255, 90); border-radius: 6px; "
                                     "padding: 4px 14px; }"
                                     "#lfErrorCard QPushButton:hover { background: rgba(255, 255, 255, 50); }"
                                     "#lfErrorCard QPushButton:pressed { background: rgba(255, 255, 255, 80); }"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(8);
        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("lfErrorTitle"));
        QFont tf = m_title->font();
        tf.setPointSizeF(tf.pointSizeF() * 1.35);
        tf.setBold(true);
        m_title->setFont(tf);
        m_title->setAlignment(Qt::AlignCenter);
        m_title->setWordWrap(true);
        m_detail = new QLabel(this);
        m_detail->setObjectName(QStringLiteral("lfErrorDetail"));
        m_detail->setAlignment(Qt::AlignCenter);
        m_detail->setWordWrap(true);
        m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_action = new QPushButton(this);
        m_action->setObjectName(QStringLiteral("lfErrorAction"));
        m_action->setFocusPolicy(Qt::NoFocus);  // arrow keys keep flipping (F-FMT-02)
        m_action->hide();
        layout->addWidget(m_title);
        layout->addWidget(m_detail);
        layout->addWidget(m_action, 0, Qt::AlignHCenter);
    }
    void setTexts(const QString& title, const QString& detail, const QString& action) {
        m_title->setText(title);
        m_detail->setText(detail);
        m_detail->setVisible(!detail.isEmpty());
        m_action->setText(action);
        m_action->setVisible(!action.isEmpty());
    }
    QPushButton* actionButton() const { return m_action; }
    void place(const QSize& area) {
        const int w = std::clamp(area.width() - 48, 120, 520);
        setFixedWidth(w);
        adjustSize();
        const QSize s = sizeHint().expandedTo(QSize(w, 0));
        setGeometry((area.width() - w) / 2, (area.height() - s.height()) / 2, w, s.height());
    }

private:
    QLabel* m_title;
    QLabel* m_detail;
    QPushButton* m_action;
};

} // namespace

struct ImageView::Private {
    ImageView* q = nullptr;
    ImageCanvas* canvas = nullptr;
    ErrorCard* card = nullptr;
    ZoomModel model;
    DecodedImagePtr image;
    QString background = QStringLiteral("dark");
    QColorSpace explicitOutput;
    bool screenHooked = false;
    QPointer<QWindow> hookedWindow;

    // signals state
    double lastFactor = -1;
    ZoomMode lastMode = ZoomMode::Custom;
    bool needsHigherSent = false;

    // input state
    bool leftDown = false;
    bool dragging = false;
    bool altDrag = false;
    bool suppressClick = false;
    QPointF pressPos;
    QPointF lastPos;
    double wheelAccum = 0;
    QPointF trackpadAccum;
    bool trackpadFired = false;

    double dpr() const { return canvas->devicePixelRatioF(); }
    QPointF toDevice(QPointF widgetPoint) const { return widgetPoint * dpr(); }
    QPointF centerDevice() const {
        const QSize s = model.viewportSize();
        return QPointF(s.width() / 2.0, s.height() / 2.0);
    }

    void syncGeometry() {
        const double r = dpr();
        model.setDevicePixelRatio(r);
        model.setViewportSize(canvas->size() * r);
    }

    void sync() {
        canvas->setViewTransform(model.imageToViewport());
        const double f = model.factor();
        if (std::abs(f - lastFactor) > 1e-9 || model.mode() != lastMode) {
            lastFactor = f;
            lastMode = model.mode();
            emit q->zoomChanged(f, lastMode);
        }
        if (image) {
            const bool need = model.needsHigherResolution(image->image.size()) &&
                              (image->isPreview || image->image.size() != model.imageSize());
            if (need && !needsHigherSent) {
                needsHigherSent = true;
                emit q->needsHigherResolution();
            }
        }
        updateCursor();
    }

    void updateCursor() {
        if (dragging && !altDrag)
            canvas->setCursor(Qt::ClosedHandCursor);
        else if (image && model.isZoomed())
            canvas->setCursor(Qt::OpenHandCursor);
        else
            canvas->unsetCursor();
    }

    // Base color of the background setting; `checker` = checkerboard pattern.
    QColor resolveBackground(bool* checker) const {
        QString v = background.trimmed().toLower();
        if (v == QLatin1String("system")) {
            v = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light ? QStringLiteral("light")
                                                                                       : QStringLiteral("dark");
        }
        QColor base = kDark;
        *checker = false;
        if (v == QLatin1String("light")) {
            base = kLight;
        } else if (v == QLatin1String("checker")) {
            *checker = true;
        } else if (v.startsWith(QLatin1Char('#'))) {
            const QColor c = QColor::fromString(v);
            if (c.isValid())
                base = c;
        }
        return base;
    }

    void applyBackground() {
        bool checker = false;
        const QColor base = resolveBackground(&checker);
        canvas->setBackground(base, checker, kChecker1, kChecker2, kCheckerCellPt * dpr());
    }

    void updateOutputColorSpace() {
        canvas->setOutputColorSpace(explicitOutput.isValid() ? explicitOutput : windowColorSpace(q));
    }

    void hookScreen() {
        QWindow* w = q->window()->windowHandle();
        if (!w || w == hookedWindow)
            return;
        hookedWindow = w;
        // F-COL-02 (basic): re-read the output profile when the window changes screens.
        QObject::connect(w, &QWindow::screenChanged, q, [this] {
            updateOutputColorSpace();
            syncGeometry();
            applyBackground();
            sync();
        });
    }

    InputEvent makeEvent(InputKind kind, Qt::KeyboardModifiers mods) const {
        InputEvent ev;
        ev.kind = kind;
        ev.modifiers = mods;
        ev.zoomed = image && model.isZoomed();
        return ev;
    }

    void forward(const InputEvent& ev, QPointF pos) { emit q->userInput(ev, pos); }

    bool handleWheel(QWheelEvent* e) {
        const bool trackpad = !e->pixelDelta().isNull() || e->phase() != Qt::NoScrollPhase;
        const Qt::KeyboardModifiers mods = e->modifiers();
        if (trackpad) {
            if (e->phase() == Qt::ScrollBegin) {
                trackpadAccum = {};
                trackpadFired = false;
            }
            const QPointF delta = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 8.0 : QPointF(e->pixelDelta());
            const bool commandLike = mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
            if (!commandLike && image && model.isZoomed()) {
                // Two finger scroll pans; macOS supplies momentum events (F-VIEW-04).
                model.panBy(toDevice(delta));
                sync();
                return true;
            }
            if (e->phase() == Qt::ScrollMomentum || trackpadFired)
                return true;
            trackpadAccum += delta;
            const double ax = std::abs(trackpadAccum.x()), ay = std::abs(trackpadAccum.y());
            if (!commandLike && ax > kSwipeThreshold && ax > ay * 1.5) {
                // Physical finger direction (deltas follow the fingers with natural scrolling).
                const double sign = trackpadAccum.x() > 0 ? 1.0 : -1.0;
                InputEvent ev = makeEvent(InputKind::Swipe, mods);
                ev.swipeDirection = int(e->inverted() ? sign : -sign);
                trackpadFired = true;
                forward(ev, e->position());
            } else if (ay > kTrackpadStepThreshold && ay > ax) {
                InputEvent ev = makeEvent(InputKind::Wheel, mods);
                ev.wheelSteps = trackpadAccum.y() < 0 ? 1 : -1;
                ev.atScrollEdge = model.atEdge(ev.wheelSteps > 0 ? Qt::BottomEdge : Qt::TopEdge);
                trackpadFired = true;
                forward(ev, e->position());
            }
            return true;
        }
        // Real mouse wheel: accumulate to notches of 120 (high resolution wheels).
        const QPoint ad = e->angleDelta();
        wheelAccum += ad.y() != 0 ? ad.y() : ad.x();
        const int whole = int(wheelAccum / 120.0);
        if (whole == 0)
            return true;
        wheelAccum -= whole * 120.0;
        InputEvent ev = makeEvent(InputKind::Wheel, mods);
        ev.wheelSteps = -whole;  // +down / -up
        ev.atScrollEdge = model.atEdge(ev.wheelSteps > 0 ? Qt::BottomEdge : Qt::TopEdge);
        forward(ev, e->position());
        return true;
    }

    bool handleGesture(QNativeGestureEvent* e) {
        const QPointF pos = e->position();
        switch (e->gestureType()) {
        case Qt::ZoomNativeGesture:
            if (image) {
                model.zoomBy(1.0 + e->value(), toDevice(pos));
                sync();
            }
            return true;
        case Qt::SmartZoomNativeGesture:
            if (image) {
                model.toggleFitActual(toDevice(pos));
                sync();
            }
            return true;
        case Qt::SwipeNativeGesture: {
            // Qt reports the swipe as an angle: 0 = right, 180 = left.
            InputEvent ev = makeEvent(InputKind::Swipe, e->modifiers());
            ev.swipeDirection = std::abs(e->value() - 180.0) < 1.0 ? -1 : (std::abs(e->value()) < 1.0 ? 1 : 0);
            if (ev.swipeDirection != 0)
                forward(ev, pos);
            return true;
        }
        case Qt::BeginNativeGesture:
        case Qt::EndNativeGesture:
            return true;
        default:
            return false;
        }
    }

    bool handleMouse(QEvent* event) {
        auto* e = static_cast<QMouseEvent*>(event);
        const QPointF pos = e->position();
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (e->button() == Qt::LeftButton) {
                leftDown = true;
                dragging = false;
                altDrag = e->modifiers() & Qt::AltModifier;
                pressPos = lastPos = pos;
                return true;
            }
            if (e->button() == Qt::MiddleButton) {
                forward(makeEvent(InputKind::MiddleClick, e->modifiers()), pos);
                return true;
            }
            if (e->button() == Qt::BackButton) {
                forward(makeEvent(InputKind::BackButton, e->modifiers()), pos);
                return true;
            }
            if (e->button() == Qt::ForwardButton) {
                forward(makeEvent(InputKind::ForwardButton, e->modifiers()), pos);
                return true;
            }
            return false;
        case QEvent::MouseMove:
            if (!leftDown)
                return false;
            if (!dragging && (pos - pressPos).manhattanLength() >= QApplication::startDragDistance()) {
                dragging = true;
                if (altDrag)
                    emit q->dragOutRequested();  // F-IN-14: ⌥-drag = drag-out
                updateCursor();
            }
            if (dragging && !altDrag) {
                model.panBy(toDevice(pos - lastPos));
                sync();
            }
            lastPos = pos;
            return true;
        case QEvent::MouseButtonRelease:
            if (e->button() != Qt::LeftButton)
                return e->button() == Qt::MiddleButton || e->button() == Qt::BackButton ||
                       e->button() == Qt::ForwardButton;
            if (leftDown && !dragging && !suppressClick)
                forward(makeEvent(InputKind::Click, e->modifiers()), pos);
            leftDown = dragging = altDrag = suppressClick = false;
            updateCursor();
            return true;
        case QEvent::MouseButtonDblClick:
            if (e->button() == Qt::LeftButton) {
                leftDown = true;
                dragging = false;
                suppressClick = true;  // the release after a double click is not a click
                pressPos = lastPos = pos;
                forward(makeEvent(InputKind::DoubleClick, e->modifiers()), pos);
                return true;
            }
            return false;
        default:
            return false;
        }
    }
};

ImageView::ImageView(QWidget* parent) : QWidget(parent), d(new Private) {
    d->q = this;
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_AcceptTouchEvents, false);
    d->canvas = new ImageCanvas(this);
    d->canvas->setFocusPolicy(Qt::NoFocus);
    d->canvas->installEventFilter(this);
    d->canvas->setMouseTracking(false);
    d->card = new ErrorCard(this);
    d->card->hide();
    connect(d->card->actionButton(), &QPushButton::clicked, this, &ImageView::errorActionTriggered);
    d->updateOutputColorSpace();
    d->applyBackground();
    d->syncGeometry();
}

ImageView::~ImageView() { delete d; }

void ImageView::setImage(DecodedImagePtr image, bool sameFile) {
    if (!image || image->image.isNull()) {
        clear();
        return;
    }
    d->card->hide();
    const bool sameAsShown = sameFile && d->image;
    const QSize full = image->fullSize.isEmpty() ? image->image.size() : image->fullSize;
    d->syncGeometry();
    if (!sameAsShown || full != d->model.imageSize()) {
        // Same file with a different full size behaves like a locked swap.
        const bool locked = d->model.zoomLocked();
        d->model.setZoomLocked(locked || sameAsShown);
        d->model.setImageSize(full);
        d->model.setZoomLocked(locked);
    }
    d->image = std::move(image);
    d->needsHigherSent = false;
    d->canvas->setCurrent(d->image, sameAsShown);
    d->sync();
}

void ImageView::clear() {
    d->image.reset();
    d->canvas->setCurrent(nullptr, false);
    d->card->hide();
    d->sync();
}

void ImageView::showError(const QString& title, const QString& detail, const QString& actionText) {
    d->image.reset();
    d->canvas->setCurrent(nullptr, false);
    d->card->setTexts(title, detail, actionText);
    d->card->place(size());
    d->card->show();
    d->card->raise();
    d->sync();
}

bool ImageView::isShowingError() const { return !d->card->isHidden(); }

DecodedImagePtr ImageView::image() const { return d->image; }

void ImageView::setPreloadImages(const QList<DecodedImagePtr>& images) { d->canvas->setPreload(images); }

void ImageView::setZoomMode(ZoomMode mode) {
    d->syncGeometry();
    d->model.setMode(mode);
    d->sync();
}

ZoomMode ImageView::zoomMode() const { return d->model.mode(); }

double ImageView::zoomFactor() const { return d->model.factor(); }

void ImageView::setZoomFactor(double factor, QPointF anchorInWidget) {
    d->model.setFactor(factor, d->toDevice(anchorInWidget));
    d->sync();
}

void ImageView::zoomBy(double factor, QPointF anchorInWidget) {
    d->model.zoomBy(factor, d->toDevice(anchorInWidget));
    d->sync();
}

void ImageView::zoomBy(double factor) {
    d->model.zoomBy(factor, d->centerDevice());
    d->sync();
}

void ImageView::zoomStep(int steps, QPointF anchorInWidget) {
    d->model.zoomStep(steps, d->toDevice(anchorInWidget));
    d->sync();
}

void ImageView::zoomStep(int steps) {
    d->model.zoomStep(steps, d->centerDevice());
    d->sync();
}

void ImageView::toggleFitActual(QPointF anchorInWidget) {
    d->model.toggleFitActual(d->toDevice(anchorInWidget));
    d->sync();
}

void ImageView::setZoomLocked(bool locked) { d->model.setZoomLocked(locked); }

bool ImageView::zoomLocked() const { return d->model.zoomLocked(); }

void ImageView::panBy(QPointF deltaInWidget) {
    d->model.panBy(d->toDevice(deltaInWidget));
    d->sync();
}

QPointF ImageView::viewCenter() const {
    const QSize img = d->model.imageSize();
    if (!d->image || img.isEmpty())
        return QPointF(-1, -1);
    const QPointF p = d->model.viewportToImage(d->centerDevice());
    return QPointF(p.x() / img.width(), p.y() / img.height());
}

void ImageView::setViewCenter(QPointF relative) {
    const QSize img = d->model.imageSize();
    if (!d->image || img.isEmpty() || relative.x() < 0 || relative.y() < 0)
        return;
    d->syncGeometry();
    const QPointF target(relative.x() * img.width(), relative.y() * img.height());
    const QPointF now = d->model.imageToViewport().map(target);
    d->model.setOffset(d->model.offset() + (d->centerDevice() - now));
    d->sync();
}

void ImageView::scrollToTop() {
    d->model.scrollToTop();
    d->sync();
}

void ImageView::scrollToBottom() {
    d->model.scrollToBottom();
    d->sync();
}

bool ImageView::isZoomed() const { return d->image && d->model.isZoomed(); }

bool ImageView::atEdge(Qt::Edge edge) const { return !d->image || d->model.atEdge(edge); }

void ImageView::setRotation(int quarterTurnsClockwise) {
    d->model.setRotation(quarterTurnsClockwise);
    d->sync();
}

int ImageView::rotation() const { return d->model.rotation(); }

void ImageView::setFlip(bool horizontal, bool vertical) {
    d->model.setFlip(horizontal, vertical);
    d->sync();
}

void ImageView::setBackground(const QString& setting) {
    d->background = setting;
    d->applyBackground();
}

QColor ImageView::backgroundColor() const {
    bool checker = false;
    const QColor base = d->resolveBackground(&checker);
    return checker ? kChecker1 : base;
}

void ImageView::setUpscaleNearest(bool nearest) { d->canvas->setUpscaleNearest(nearest); }

void ImageView::setActualSizeInDevicePixels(bool devicePixels) {
    d->model.setActualSizeInDevicePixels(devicePixels);
    d->sync();
}

void ImageView::setOutputColorSpace(const QColorSpace& colorSpace) {
    d->explicitOutput = colorSpace;
    d->updateOutputColorSpace();
}

QColorSpace ImageView::outputColorSpace() const { return d->canvas->outputColorSpace(); }

QSize ImageView::desiredDecodeSize() const { return d->model.desiredDecodeSize(); }

bool ImageView::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        d->syncGeometry();
        d->applyBackground();
        d->updateOutputColorSpace();
        d->sync();
        break;
    case QEvent::ThemeChange:
    case QEvent::ApplicationPaletteChange:
        d->applyBackground();
        break;
    case QEvent::NativeGesture:
        if (d->handleGesture(static_cast<QNativeGestureEvent*>(event))) {
            event->accept();
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

bool ImageView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == d->canvas) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
            if (d->handleMouse(event)) {
                event->accept();
                return true;
            }
            break;
        case QEvent::Wheel:
            if (d->handleWheel(static_cast<QWheelEvent*>(event))) {
                event->accept();  // otherwise QApplication propagates it to the parent
                return true;
            }
            break;
        case QEvent::NativeGesture:
            if (d->handleGesture(static_cast<QNativeGestureEvent*>(event))) {
                event->accept();
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ImageView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    d->canvas->setGeometry(rect());
    d->card->place(size());
    d->syncGeometry();
    d->sync();
}

void ImageView::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_CapsLock:
    case Qt::Key_unknown:
        QWidget::keyPressEvent(event);
        return;
    default:
        break;
    }
    InputEvent ev = d->makeEvent(InputKind::Key, event->modifiers());
    ev.key = key;
    switch (key) {
    case Qt::Key_Right: ev.atScrollEdge = atEdge(Qt::RightEdge); break;
    case Qt::Key_Left: ev.atScrollEdge = atEdge(Qt::LeftEdge); break;
    case Qt::Key_Up:
    case Qt::Key_PageUp:
    case Qt::Key_Home: ev.atScrollEdge = atEdge(Qt::TopEdge); break;
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_End:
    case Qt::Key_Space: ev.atScrollEdge = atEdge(Qt::BottomEdge); break;
    default: break;
    }
    d->forward(ev, mapFromGlobal(QCursor::pos()));
    event->accept();
}

void ImageView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    d->hookScreen();
    d->updateOutputColorSpace();
    d->applyBackground();
    d->syncGeometry();
    d->sync();
}

} // namespace lf
