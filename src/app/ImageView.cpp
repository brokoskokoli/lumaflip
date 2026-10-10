// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageView.h"

#include "DisplayColorSpace.h"
#include "EdrDisplay.h"
#include "EdrSurface.h"
#include "ImageCanvas.h"
#include "ScreenProfileWatcher.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>

namespace lf {

namespace {

// Trackpad thresholds (logical px accumulated within one gesture).
constexpr double kSwipeThreshold = 50.0;
constexpr double kTrackpadStepThreshold = 80.0;

std::function<QColorSpace(const QWidget*)>& colorSpaceResolver() {
    static std::function<QColorSpace(const QWidget*)> resolver;
    return resolver;
}

std::function<hdr::DisplayState(const QWidget*)>& displayStateProvider() {
    static std::function<hdr::DisplayState(const QWidget*)> provider;
    return provider;
}

// Brightness changes the available EDR headroom without a notification (F-COL-03).
constexpr int kHeadroomPollMs = 250;

bool isFloatFormat(QImage::Format f) {
    switch (f) {
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_RGBX32FPx4:
        return true;
    default:
        return false;
    }
}

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
        setStyleSheet(
            QStringLiteral("#lfErrorCard { background: rgba(20, 20, 20, 225); border-radius: 12px; "
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
        m_action->setFocusPolicy(Qt::NoFocus); // arrow keys keep flipping (F-FMT-02)
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
    QPointer<ScreenProfileWatcher> profileWatcher; // F-COL-02
    bool dragOutDefault = false;                   // F-IN-14 "dragOut"
    bool dragOutEnabled = true;                    // F-GRID-03 compare panes: off
    int quiet = 0;                                 // > 0: no viewTransformChanged()
    QTransform lastViewTransform;

    // HDR (F-COL-03/04/05)
    bool hdrOn = true;
    hdr::Exposure exposure;
    hdr::Decision decision;
    hdr::DisplayState display;
    bool extendedActive = false;
    bool edrFailed = false; // the swapchain cannot be used: widget path only
    double emittedEv = 0.0; // exposure at the last hdrStateChanged()
    EdrSurface* edr = nullptr;
    QWidget* edrContainer = nullptr;
    QTimer* headroomPoll = nullptr;
    EdrDisplayWatcher* edrWatcher = nullptr;
    // Widgets above the image, grabbed for the extended pass (EdrSurface.h).
    QImage overlay;
    QRect overlayDirty; // logical px of the view
    bool overlayScheduled = false;
    bool grabbing = false;

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

    // sync() for changes the user did not make (new image, resize, screen): no
    // viewTransformChanged() (M3 synchronized views).
    void quietSync() {
        ++quiet;
        sync();
        --quiet;
    }

    void sync() {
        const QTransform viewTransform = model.imageToViewport();
        canvas->setViewTransform(viewTransform);
        if (viewTransform != lastViewTransform) {
            lastViewTransform = viewTransform;
            if (!quiet && image)
                emit q->viewTransformChanged();
        }
        const double f = model.factor();
        if (std::abs(f - lastFactor) > 1e-9 || model.mode() != lastMode) {
            lastFactor = f;
            lastMode = model.mode();
            emit q->zoomChanged(f, lastMode);
        }
        if (image) {
            // By size only (smaller than the full size and magnified): DecodedImage::isPreview
            // is no quality marker (it is true for the Screen stage of large files as well).
            const bool need = model.needsHigherResolution(image->image.size());
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
        if (edr)
            edr->setCursor(canvas->cursor()); // the native surface covers the canvas
    }

    // ---- HDR ----

    hdr::DisplayState readDisplay() const {
        const auto& provider = displayStateProvider();
        return provider ? provider(q) : edrDisplayState(q);
    }

    bool imageIsHdr() const {
        return image && hdr::isHdrContent(image->headroom, isFloatFormat(image->image.format()));
    }

    // Recomputes what the current image gets (F-COL-03/04) and applies it to the canvas.
    void updateHdr() {
        display = readDisplay();
        const float peak = image ? image->headroom : 1.0f;
        const bool floatSource = image && isFloatFormat(image->image.format());
        const bool hdrContent = hdr::isHdrContent(peak, floatSource);
        const float exposureMul = hdrContent ? exposure.multiplier() : 1.0f;
        const hdr::Decision next = hdr::decide(peak, floatSource, hdrOn, display, exposureMul);
        canvas->setHdrMapping(exposureMul, next.targetHeadroom);
        const bool useExtended = next.useExtendedSurface && !edrFailed && edrPresentationAvailable() &&
                                 q->isVisible() && !card->isVisible();
        const bool changed = next.presentation != decision.presentation || next.badge != decision.badge ||
                             next.targetHeadroom != decision.targetHeadroom ||
                             useExtended != extendedActive || exposure.ev() != emittedEv;
        emittedEv = exposure.ev();
        decision = next;
        setExtended(useExtended);
        // Follow the brightness while HDR content is shown on an EDR display.
        if (next.useExtendedSurface) {
            if (!headroomPoll) {
                headroomPoll = new QTimer(q);
                headroomPoll->setInterval(kHeadroomPollMs);
                QObject::connect(headroomPoll, &QTimer::timeout, q, [this] {
                    if (hdr::headroomChanged(display.currentHeadroom, readDisplay().currentHeadroom))
                        updateHdr();
                });
            }
            if (!headroomPoll->isActive())
                headroomPoll->start();
        } else if (headroomPoll) {
            headroomPoll->stop();
        }
        if (changed)
            emit q->hdrStateChanged();
    }

    void ensureEdr() {
        if (edr)
            return;
        // The surface is a native child window; its siblings (the canvas, overlays) stay
        // non-native and keep being composited by Qt.
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
        edr = new EdrSurface(canvas);
        edrContainer = QWidget::createWindowContainer(edr, q);
        edrContainer->setObjectName(QStringLiteral("lfEdrSurface"));
        edrContainer->setAttribute(Qt::WA_TransparentForMouseEvents);
        edrContainer->setAttribute(Qt::WA_DontCreateNativeAncestors);
        edrContainer->setFocusPolicy(Qt::NoFocus);
        edrContainer->setGeometry(q->rect());
        edrContainer->hide();
        QObject::connect(edr, &EdrSurface::failed, q, [this] {
            edrFailed = true; // e.g. moved to a screen without EDR support: SDR preview
            updateHdr();
        });
        updateCursor();
    }

    void setExtended(bool on) {
        if (on == extendedActive)
            return;
        extendedActive = on;
        if (on) {
            ensureEdr();
            edrContainer->setGeometry(q->rect());
            canvas->setExtendedPresentation(true, [this] {
                if (edr)
                    edr->requestFrame();
            });
            edrContainer->show();
            overlay = QImage();
            markOverlayDirty(q->rect());
        } else {
            canvas->setExtendedPresentation(false);
            canvas->setExtendedOverlay(QImage(), QRect());
            overlay = QImage();
            if (edrContainer)
                edrContainer->hide();
        }
    }

    // ---- overlays of the extended pass ----

    bool isOverlay(const QObject* o) const {
        if (!o->isWidgetType() || o == canvas || o == edrContainer)
            return false;
        for (auto* w = static_cast<const QWidget*>(o); w; w = w->parentWidget())
            if (w->parentWidget() == q)
                return w != canvas && w != edrContainer;
        return false;
    }

    void watchOverlays(QWidget* w) {
        w->installEventFilter(q); // idempotent (Qt removes duplicates)
        for (QObject* c : w->children())
            if (c->isWidgetType())
                watchOverlays(static_cast<QWidget*>(c));
    }

    void markOverlayDirty(const QRect& viewRect) {
        if (!extendedActive)
            return;
        overlayDirty = overlayDirty.united(viewRect.intersected(q->rect()));
        if (overlayScheduled)
            return;
        overlayScheduled = true;
        QTimer::singleShot(0, q, [this] {
            overlayScheduled = false;
            grabOverlays();
        });
    }

    QList<QWidget*> overlayWidgets() const {
        QList<QWidget*> out;
        for (QObject* c : q->children())
            if (c->isWidgetType() && c != canvas && c != edrContainer &&
                static_cast<QWidget*>(c)->isVisible())
                out << static_cast<QWidget*>(c);
        return out;
    }

    // Paints the visible widgets above the image into `overlay` (changed part only) and
    // hands it to the canvas, which draws it over the image in the extended pass.
    void grabOverlays() {
        if (!extendedActive)
            return;
        const QList<QWidget*> widgets = overlayWidgets();
        for (QWidget* w : widgets)
            watchOverlays(w);
        if (widgets.isEmpty()) {
            overlay = QImage();
            overlayDirty = QRect();
            canvas->setExtendedOverlay(QImage(), QRect());
            return;
        }
        const qreal dpr = q->devicePixelRatioF();
        const QSize px = (QSizeF(q->size()) * dpr).toSize();
        if (overlay.size() != px) {
            overlay = QImage(px, QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            overlayDirty = q->rect();
        }
        overlay.setDevicePixelRatio(dpr);
        const QRect dirty = overlayDirty.intersected(q->rect());
        overlayDirty = QRect();
        if (dirty.isEmpty())
            return;
        grabbing = true;
        {
            QPainter p(&overlay);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(dirty, Qt::transparent);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setClipRect(dirty);
            for (QWidget* w : widgets)
                if (w->geometry().intersects(dirty))
                    w->render(&p, w->pos(), QRegion(), QWidget::DrawChildren);
        }
        grabbing = false;
        const QRect devDirty = QRectF(QRectF(dirty).topLeft() * dpr, QRectF(dirty).size() * dpr)
                                   .toAlignedRect()
                                   .intersected(overlay.rect());
        canvas->setExtendedOverlay(overlay, devDirty);
    }

    // Event of a widget above the image while the extended presentation is active.
    void overlayEvent(QObject* watched, QEvent* event) {
        if (grabbing || !extendedActive || !isOverlay(watched))
            return;
        auto* w = static_cast<QWidget*>(watched);
        switch (event->type()) {
        case QEvent::Paint: {
            const QRect r = static_cast<QPaintEvent*>(event)->rect();
            markOverlayDirty(QRect(w->mapTo(q, r.topLeft()), r.size()));
            break;
        }
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Move:
        case QEvent::Resize:
            markOverlayDirty(q->rect()); // old and new place
            break;
        case QEvent::ChildAdded:
            if (auto* c = static_cast<QChildEvent*>(event)->child(); c && c->isWidgetType())
                watchOverlays(static_cast<QWidget*>(c));
            break;
        default:
            break;
        }
    }

    // Base color of the background setting; `checker` = checkerboard pattern.
    QColor resolveBackground(bool* checker) const {
        QString v = background.trimmed().toLower();
        if (v == QLatin1String("system")) {
            v = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light
                    ? QStringLiteral("light")
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
        if (explicitOutput.isValid()) {
            canvas->setOutputColorSpace(explicitOutput);
            return;
        }
        const auto& resolve = colorSpaceResolver();
        canvas->setOutputColorSpace(resolve ? resolve(q) : windowColorSpace(q));
    }

    void onScreenChanged() {
        updateOutputColorSpace();
        syncGeometry();
        applyBackground();
        quietSync();
        edrFailed = false; // another screen may support it
        updateHdr();
    }

    void hookScreen() {
        QWindow* w = q->window()->windowHandle();
        if (!w || w == hookedWindow)
            return;
        hookedWindow = w;
        // F-COL-02: re-read the output profile when the window moves to another screen
        // (Qt) or the profile of its display changes (macOS notifications).
        QObject::connect(w, &QWindow::screenChanged, q, [this] { onScreenChanged(); });
        delete profileWatcher.data();
        profileWatcher = new ScreenProfileWatcher(q->window(), [this] { onScreenChanged(); }, q);
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
            if (e->phase() == Qt::ScrollBegin || e->phase() == Qt::ScrollEnd) {
                // F-NAV-04: one swipe per gesture; momentum events after the end are ignored below.
                trackpadAccum = {};
                trackpadFired = false;
                if (e->phase() == Qt::ScrollEnd)
                    return true;
            }
            const QPointF delta =
                e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 8.0 : QPointF(e->pixelDelta());
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
        ev.wheelSteps = -whole; // +down / -up
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
            ev.swipeDirection =
                std::abs(e->value() - 180.0) < 1.0 ? -1 : (std::abs(e->value()) < 1.0 ? 1 : 0);
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
                // F-IN-14: ⌥ swaps panning and drag-out.
                altDrag = dragOutEnabled && bool(e->modifiers() & Qt::AltModifier) != dragOutDefault;
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
                if (altDrag) {
                    // F-FILE-07: the drag runs until the drop (nested event loop) and
                    // swallows the release: the gesture is over afterwards.
                    if (image)
                        emit q->dragOutRequested();
                    leftDown = dragging = altDrag = false;
                    updateCursor();
                    return true;
                }
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
                suppressClick = true; // the release after a double click is not a click
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
    if (edrPresentationAvailable())
        d->edrWatcher = new EdrDisplayWatcher([this] { d->onScreenChanged(); }, this);
    d->updateOutputColorSpace();
    d->applyBackground();
    d->syncGeometry();
}

ImageView::~ImageView() {
    // The EDR surface uses the canvas' QRhi: it goes first.
    delete d->edrContainer;
    d->edrContainer = nullptr;
    d->edr = nullptr;
    delete d;
}

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
    if (!sameAsShown)
        d->exposure.imageChanged(); // F-COL-05: per image unless locked
    d->updateHdr();
    d->canvas->setCurrent(d->image, sameAsShown);
    d->quietSync();
}

void ImageView::showAnimationFrame(DecodedImagePtr frame) {
    if (!frame || frame->image.isNull() || !d->image) {
        setImage(std::move(frame), true);
        return;
    }
    const QSize full = frame->fullSize.isEmpty() ? frame->image.size() : frame->fullSize;
    if (full != d->model.imageSize()) {
        setImage(std::move(frame), true);
        return;
    }
    d->image = std::move(frame);
    d->canvas->replaceFrame(d->image);
}

void ImageView::beginCrossfade(int ms) {
    d->canvas->startCrossfade(ms);
}

void ImageView::clear() {
    d->image.reset();
    d->canvas->setCurrent(nullptr, false);
    d->card->hide();
    d->exposure.imageChanged();
    d->updateHdr();
    d->quietSync();
}

void ImageView::showError(const QString& title, const QString& detail, const QString& actionText) {
    d->image.reset();
    d->canvas->setCurrent(nullptr, false);
    d->card->setTexts(title, detail, actionText);
    d->card->place(size());
    d->card->show();
    d->card->raise();
    d->updateHdr();
    d->quietSync();
}

bool ImageView::isShowingError() const {
    return !d->card->isHidden();
}

DecodedImagePtr ImageView::image() const {
    return d->image;
}

void ImageView::setPreloadImages(const QList<DecodedImagePtr>& images) {
    d->canvas->setPreload(images);
}

void ImageView::setZoomMode(ZoomMode mode) {
    d->syncGeometry();
    d->model.setMode(mode);
    d->sync();
}

ZoomMode ImageView::zoomMode() const {
    return d->model.mode();
}

double ImageView::zoomFactor() const {
    return d->model.factor();
}

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

void ImageView::setZoomLocked(bool locked) {
    d->model.setZoomLocked(locked);
}

bool ImageView::zoomLocked() const {
    return d->model.zoomLocked();
}

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

bool ImageView::isZoomed() const {
    return d->image && d->model.isZoomed();
}

bool ImageView::atEdge(Qt::Edge edge) const {
    return !d->image || d->model.atEdge(edge);
}

void ImageView::setRotation(int quarterTurnsClockwise) {
    d->model.setRotation(quarterTurnsClockwise);
    d->sync();
}

int ImageView::rotation() const {
    return d->model.rotation();
}

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

void ImageView::setUpscaleNearest(bool nearest) {
    d->canvas->setUpscaleNearest(nearest);
}

void ImageView::setActualSizeInDevicePixels(bool devicePixels) {
    d->model.setActualSizeInDevicePixels(devicePixels);
    d->quietSync();
}

void ImageView::setOutputColorSpace(const QColorSpace& colorSpace) {
    d->explicitOutput = colorSpace;
    d->updateOutputColorSpace();
}

QColorSpace ImageView::outputColorSpace() const {
    return d->canvas->outputColorSpace();
}

QSize ImageView::desiredDecodeSize() const {
    return d->model.desiredDecodeSize();
}

void ImageView::setDragOutByDefault(bool dragOut) {
    d->dragOutDefault = dragOut;
}

void ImageView::setDragOutEnabled(bool enabled) {
    d->dragOutEnabled = enabled;
}

QTransform ImageView::imageToWidget() const {
    if (!d->image)
        return {};
    const double r = d->dpr();
    return d->model.imageToViewport() * QTransform::fromScale(1.0 / r, 1.0 / r);
}

void ImageView::setColorSpaceResolverForTesting(std::function<QColorSpace(const QWidget*)> resolver) {
    colorSpaceResolver() = std::move(resolver);
}

void ImageView::setHdrEnabled(bool enabled) {
    if (d->hdrOn == enabled)
        return;
    d->hdrOn = enabled;
    d->updateHdr();
    emit hdrStateChanged();
}

bool ImageView::hdrEnabled() const {
    return d->hdrOn;
}

hdr::Decision ImageView::hdrDecision() const {
    return d->decision;
}

bool ImageView::isExtendedPresentationActive() const {
    return d->extendedActive;
}

void ImageView::setExposureEv(double ev) {
    if (d->exposure.setEv(ev))
        d->updateHdr(); // emits hdrStateChanged()
}

void ImageView::stepExposure(int thirds) {
    if (d->exposure.step(thirds))
        d->updateHdr();
}

double ImageView::exposureEv() const {
    return d->exposure.ev();
}

void ImageView::setExposureLocked(bool locked) {
    if (d->exposure.locked() == locked)
        return;
    d->exposure.setLocked(locked);
    emit hdrStateChanged();
}

bool ImageView::exposureLocked() const {
    return d->exposure.locked();
}

bool ImageView::exposureApplies() const {
    return d->imageIsHdr();
}

void ImageView::setDisplayStateForTesting(std::function<hdr::DisplayState(const QWidget*)> provider) {
    displayStateProvider() = std::move(provider);
}

bool ImageView::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        d->syncGeometry();
        d->applyBackground();
        d->updateOutputColorSpace();
        d->quietSync();
        d->edrFailed = false;
        d->updateHdr();
        d->markOverlayDirty(rect());
        break;
    case QEvent::ChildAdded:
        // A widget above the image (toast, rename field, HUD ...): grabbed for the extended pass.
        if (auto* c = static_cast<QChildEvent*>(event)->child(); c && c->isWidgetType() && d->extendedActive)
            d->watchOverlays(static_cast<QWidget*>(c));
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
    if (watched != d->canvas && d->extendedActive)
        d->overlayEvent(watched, event);
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
                event->accept(); // otherwise QApplication propagates it to the parent
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
    if (d->edrContainer)
        d->edrContainer->setGeometry(rect());
    d->card->place(size());
    d->syncGeometry();
    d->quietSync();
    d->markOverlayDirty(rect());
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
    case Qt::Key_Right:
        ev.atScrollEdge = atEdge(Qt::RightEdge);
        break;
    case Qt::Key_Left:
        ev.atScrollEdge = atEdge(Qt::LeftEdge);
        break;
    case Qt::Key_Up:
    case Qt::Key_PageUp:
    case Qt::Key_Home:
        ev.atScrollEdge = atEdge(Qt::TopEdge);
        break;
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_End:
    case Qt::Key_Space:
        ev.atScrollEdge = atEdge(Qt::BottomEdge);
        break;
    default:
        break;
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
    d->quietSync();
    d->updateHdr();
}

void ImageView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    d->updateHdr(); // no extended surface for a hidden view
}

} // namespace lf
