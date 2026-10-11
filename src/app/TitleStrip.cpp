// SPDX-License-Identifier: GPL-3.0-or-later
#include "TitleStrip.h"

#include "ViewerWindow.h"
#include "WindowTabbing.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>
#include <QWindow>

#include <cmath>
#include <utility>

namespace lf {

namespace {

constexpr int kStripAlpha = 150; // semi-transparent black (about 60 %)
#ifdef Q_OS_WIN
constexpr int kButtonWidth = 46; // Windows 11 caption buttons
#else
constexpr int kButtonWidth = 40;
#endif

bool isAutoHideSetting() {
    return Settings::instance().value(QString::fromLatin1(key::TitleBar)).toString() !=
           QLatin1String("always");
}

} // namespace

// ---------------------------------------------------------------------------------------
// TitleStrip
// ---------------------------------------------------------------------------------------

TitleStrip::TitleStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("lfTitleStrip"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents); // until fully faded in
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    // The theme's title font (macOS: the system font of window titles).
    setFont(QFontDatabase::systemFont(QFontDatabase::TitleFont));
    m_fade = new QVariantAnimation(this);
    m_fade->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_fade, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { setOpacity(v.toReal()); });
    connect(m_fade, &QVariantAnimation::finished, this, [this] {
        if (!m_target) {
            hide();
            m_hover = TitleButton::None;
        } else {
            setAttribute(Qt::WA_TransparentForMouseEvents, false);
        }
    });
    hide();
}

void TitleStrip::setTitle(const QString& title) {
    if (title == m_title)
        return;
    m_title = title;
    update();
}

void TitleStrip::setButtonSide(TitleButtonSide side) {
    if (side == m_side)
        return;
    m_side = side;
    update();
}

void TitleStrip::setMenuButton(bool on) {
    if (on == m_menuButton)
        return;
    m_menuButton = on;
    update();
}

void TitleStrip::setReservedLeft(int px) {
    if (px == m_reservedLeft)
        return;
    m_reservedLeft = px;
    update();
}

void TitleStrip::setOpacity(qreal value) {
    m_opacity = value;
    update();
    emit opacityChanged();
}

void TitleStrip::fadeTo(bool shown, int ms) {
    const qreal target = shown ? 1.0 : 0.0;
    if (shown == m_target && (m_fade->state() == QAbstractAnimation::Running || m_opacity == target))
        return;
    m_target = shown;
    m_fade->stop();
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_pressed = TitleButton::None;
    m_dragPending = false;
    if (shown)
        show();
    if (ms <= 0 || m_opacity == target) {
        setOpacity(target);
        if (!shown) {
            hide();
            m_hover = TitleButton::None;
        } else {
            setAttribute(Qt::WA_TransparentForMouseEvents, false);
        }
        return;
    }
    m_fade->setStartValue(m_opacity);
    m_fade->setEndValue(target);
    m_fade->setDuration(std::max(1, int(std::lround(ms * std::abs(target - m_opacity)))));
    m_fade->start();
}

TitleStripLayout TitleStrip::stripLayout() const {
    TitleStripSpec spec;
    spec.width = width();
    spec.height = height();
    spec.ownButtons = !titleStripUsesSystemButtons();
    spec.side = m_side;
    spec.menuButton = m_menuButton;
    spec.buttonWidth = kButtonWidth;
    spec.reservedLeft = m_reservedLeft;
    return layoutTitleStrip(spec);
}

TitleButton TitleStrip::buttonAt(QPoint pos) const {
    return titleButtonAt(stripLayout(), pos);
}

TitleStripHit TitleStrip::nativeHit(QPoint pos) const {
    if (!takesMouse() || !rect().contains(pos))
        return TitleStripHit::Client;
    switch (buttonAt(pos)) {
    case TitleButton::None:
        return TitleStripHit::Caption;
    case TitleButton::Maximize:
        return TitleStripHit::Maximize;
    case TitleButton::Menu:
    case TitleButton::Minimize:
    case TitleButton::Close:
        break;
    }
    return TitleStripHit::Client; // the strip handles the click
}

void TitleStrip::setHover(TitleButton button) {
    if (button == m_hover)
        return;
    m_hover = button;
    update();
}

void TitleStrip::paintGlyph(QPainter& p, TitleButton button, const QRect& r) const {
    const QPointF c = QRectF(r).center();
    constexpr qreal s = 5.0; // half the glyph size
    p.save();
    QPen pen(Qt::white, 1.0);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (button) {
    case TitleButton::Minimize:
        p.drawLine(QPointF(c.x() - s, c.y()), QPointF(c.x() + s, c.y()));
        break;
    case TitleButton::Maximize:
        if (window()->isMaximized()) {
            p.drawRect(QRectF(c.x() - s, c.y() - s + 2, 2 * s - 2, 2 * s - 2));
            p.drawLine(QPointF(c.x() - s + 2, c.y() - s), QPointF(c.x() + s, c.y() - s));
            p.drawLine(QPointF(c.x() + s, c.y() - s), QPointF(c.x() + s, c.y() + s - 2));
        } else {
            p.drawRect(QRectF(c.x() - s, c.y() - s, 2 * s, 2 * s));
        }
        break;
    case TitleButton::Close:
        p.setRenderHint(QPainter::Antialiasing);
        p.drawLine(QPointF(c.x() - s, c.y() - s), QPointF(c.x() + s, c.y() + s));
        p.drawLine(QPointF(c.x() - s, c.y() + s), QPointF(c.x() + s, c.y() - s));
        break;
    case TitleButton::Menu:
        for (int i = -1; i <= 1; ++i)
            p.drawLine(QPointF(c.x() - s, c.y() + i * 4), QPointF(c.x() + s, c.y() + i * 4));
        break;
    case TitleButton::None:
        break;
    }
    p.restore();
}

void TitleStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setOpacity(m_opacity);
    p.fillRect(rect(), QColor(0, 0, 0, kStripAlpha));
    const TitleStripLayout l = stripLayout();
    const std::pair<TitleButton, QRect> buttons[] = {{TitleButton::Menu, l.menu},
                                                     {TitleButton::Minimize, l.minimize},
                                                     {TitleButton::Maximize, l.maximize},
                                                     {TitleButton::Close, l.close}};
    for (const auto& [button, r] : buttons) {
        if (r.isEmpty())
            continue;
        if (button == m_hover) {
            const bool pressed = button == m_pressed;
            const QColor bg = button == TitleButton::Close ? QColor(0xC4, 0x2B, 0x1C, pressed ? 200 : 255)
                                                           : QColor(255, 255, 255, pressed ? 60 : 36);
            p.fillRect(r, bg);
        }
        paintGlyph(p, button, r);
    }
    if (m_title.isEmpty())
        return;
#ifdef Q_OS_WIN
    const QRect area = l.titleArea; // Windows: the title starts at the left
    const Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter;
#else
    const QRect area = l.centeredTitle.isEmpty() ? l.titleArea : l.centeredTitle;
    const Qt::Alignment align = Qt::AlignCenter;
#endif
    if (area.width() <= 0)
        return;
    p.setPen(Qt::white);
    p.setFont(font());
    p.drawText(area, int(align), fontMetrics().elidedText(m_title, Qt::ElideMiddle, area.width()));
}

void TitleStrip::mousePressEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->position().toPoint();
    m_pressed = buttonAt(pos);
    if (m_pressed != TitleButton::None) {
        update();
        return;
    }
#ifdef Q_OS_MACOS
    // AppKit starts the window drag from the mouse-down event.
    if (QWindow* handle = window()->windowHandle())
        handle->startSystemMove();
#else
    // Elsewhere the system takes the mouse over: wait for a real drag, so that a double
    // click still arrives.
    m_dragPending = true;
    m_pressPos = pos;
#endif
}

void TitleStrip::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    const QPoint pos = event->position().toPoint();
    if (m_dragPending && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragPending = false;
        if (QWindow* handle = window()->windowHandle())
            handle->startSystemMove();
        return;
    }
    if (event->buttons() == Qt::NoButton)
        setHover(buttonAt(pos));
}

void TitleStrip::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    m_dragPending = false;
    const TitleButton pressed = std::exchange(m_pressed, TitleButton::None);
    update();
    if (event->button() == Qt::LeftButton && pressed != TitleButton::None &&
        buttonAt(event->position().toPoint()) == pressed)
        activate(pressed);
}

void TitleStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    event->accept();
    m_dragPending = false;
    if (event->button() == Qt::LeftButton && buttonAt(event->position().toPoint()) == TitleButton::None)
        titleStripDoubleClick(window());
}

void TitleStrip::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    setHover(TitleButton::None);
}

void TitleStrip::activate(TitleButton button) {
    QWidget* w = window();
    switch (button) {
    case TitleButton::Close:
        w->close();
        break;
    case TitleButton::Minimize:
        w->showMinimized();
        break;
    case TitleButton::Maximize:
        w->isMaximized() ? w->showNormal() : w->showMaximized();
        break;
    case TitleButton::Menu:
        emit menuRequested(mapToGlobal(stripLayout().menu.bottomLeft()));
        break;
    case TitleButton::None:
        break;
    }
}

// ---------------------------------------------------------------------------------------
// TitleStripController
// ---------------------------------------------------------------------------------------

TitleStripController::TitleStripController(ViewerWindow& window, QWidget* host)
    : QObject(&window), m_window(window), m_host(host) {
    setObjectName(QStringLiteral("lfTitleStripController"));
    m_strip = new TitleStrip(host);
    m_strip->setTitle(window.windowTitle());
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(kIdleMs);
    connect(m_idle, &QTimer::timeout, this, &TitleStripController::onIdle);
    connect(m_strip, &TitleStrip::menuRequested, this, &TitleStripController::showMenu);
    m_borderless = isAutoHideSetting();
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::TitleBar))
            setBorderless(isAutoHideSetting()); // also open windows follow
    });
    detectTitleButtonSide(this, [strip = QPointer<TitleStrip>(m_strip)](TitleButtonSide side) {
        if (strip)
            strip->setButtonSide(side);
    });
    window.installEventFilter(this);
    host->installEventFilter(this);
    // The image goes under the title strip: Qt (6.9+) would otherwise keep the layout out of
    // the window's safe area, i.e. below the transparent title bar.
    window.setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    // Borderless from the start (before the window is placed and its saved geometry is
    // restored): Qt's frame margins are then those of the borderless window, and cascading
    // and "fit window to image" compute with the right frame (F-WIN-08).
    if (m_borderless)
        setWindowMinimalChrome(&window, true);
}

bool TitleStripController::holdsMenuBar() const {
    return m_active && m_window.menuWidget() != nullptr;
}

void TitleStripController::setBorderless(bool on) {
    if (on == m_borderless)
        return;
    m_borderless = on;
    apply();
    if (m_active && m_window.isVisible())
        reveal(); // show where the title went
}

bool TitleStripController::wantActive() const {
    return m_borderless && !m_window.isFullScreen() && tabGroupSize(&m_window) <= 1;
}

void TitleStripController::apply() {
    if (m_window.isFullScreen() || m_window.fullScreenAnimating()) {
        // Full screen has its own chrome (F-VIEW-10). The native window is left alone
        // during the animation (a style change there breaks it); applied again at its end.
        m_active = false;
        m_idle->stop();
        m_strip->fadeTo(false, 0);
        return;
    }
    if (!m_window.windowHandle())
        return; // applied when the window is shown
    m_active = wantActive();
    setWindowMinimalChrome(&m_window, m_active);
    watchNativeWindow();
    if (m_active) {
        setWindowButtonsVisible(&m_window, m_strip->shownTarget(), 0);
    } else {
        m_idle->stop();
        m_strip->fadeTo(false, 0);
        setWindowButtonsVisible(&m_window, true, 0);
    }
    m_strip->setReservedLeft(titleStripReservedLeft(&m_window));
    m_strip->setMenuButton(holdsMenuBar());
    if (QWidget* bar = m_window.menuWidget()) // Windows: the menus move into the strip
        bar->setMaximumHeight(holdsMenuBar() ? 0 : QWIDGETSIZE_MAX);
    place();
}

void TitleStripController::watchNativeWindow() {
    QWindow* handle = m_window.windowHandle();
    if (handle == m_handle.data())
        return;
    if (m_handle)
        m_handle->removeEventFilter(this);
    m_handle = handle;
    if (!handle)
        return;
    // Every mouse move of the window passes its QWindow, whichever widget is below.
    handle->installEventFilter(this);
    setTitleStripHitTest(&m_window, [self = QPointer<TitleStripController>(this)](QPoint p) {
        if (!self || !self->m_active)
            return TitleStripHit::Client;
        TitleStrip* strip = self->m_strip;
        if (p.x() < 0) { // the mouse left the non-client area
            strip->setHover(TitleButton::None);
            return TitleStripHit::Client;
        }
        const QPoint sp = strip->mapFrom(&self->m_window, p);
        const TitleStripHit hit = strip->nativeHit(sp);
        if (hit != TitleStripHit::Client) {
            // Over the caption Qt gets no mouse moves: keep the strip and the hover here.
            strip->setHover(strip->buttonAt(sp));
            self->m_idle->start();
        }
        return hit;
    });
}

void TitleStripController::place() {
    if (!m_host)
        return;
    m_strip->setGeometry(0, 0, m_host->width(), titleStripHeight(&m_window));
}

void TitleStripController::reveal() {
    if (!m_active)
        return;
    if (!m_strip->shownTarget()) {
        place();
        m_strip->fadeTo(true, kFadeInMs);
        m_strip->raise();
        emit raised();
        setWindowButtonsVisible(&m_window, true, kFadeInMs);
    }
    m_idle->start();
}

void TitleStripController::onIdle() {
    if (!m_active)
        return;
    const bool menuOpen = m_menu && m_menu->isVisible();
    if (menuOpen || (m_strip->isVisible() && m_strip->underMouse()) ||
        QGuiApplication::mouseButtons() != Qt::NoButton) {
        m_idle->start();
        return;
    }
    m_strip->fadeTo(false, kFadeOutMs);
    setWindowButtonsVisible(&m_window, false, kFadeOutMs);
}

void TitleStripController::windowStateChanged() {
    apply();
}

void TitleStripController::showMenu(QPoint globalPos) {
    auto* bar = qobject_cast<QMenuBar*>(m_window.menuWidget());
    if (!bar)
        return;
    auto* menu = new QMenu(&m_window);
    menu->setObjectName(QStringLiteral("lfTitleStripMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (QAction* a : bar->actions())
        menu->addAction(a); // the window's menus (not owned here)
    m_menu = menu;
    menu->popup(globalPos);
}

bool TitleStripController::handleResizeEdges(QEvent* event) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    Q_UNUSED(event);
    return false; // the system's frame resizes
#else
    constexpr int kResizeMargin = 5; // frameless windows resize this close to the edge
    if (!m_handle || m_window.isMaximized() || m_window.isFullScreen())
        return false;
    const auto* me = static_cast<QMouseEvent*>(event);
    const QPointF p = me->position();
    Qt::Edges edges;
    if (p.x() < kResizeMargin)
        edges |= Qt::LeftEdge;
    if (p.x() >= m_handle->width() - kResizeMargin)
        edges |= Qt::RightEdge;
    if (p.y() < kResizeMargin)
        edges |= Qt::TopEdge;
    if (p.y() >= m_handle->height() - kResizeMargin)
        edges |= Qt::BottomEdge;
    if (event->type() == QEvent::MouseMove) {
        if (me->buttons() != Qt::NoButton)
            return false;
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (edges.testFlags(Qt::LeftEdge | Qt::TopEdge) || edges.testFlags(Qt::RightEdge | Qt::BottomEdge))
            shape = Qt::SizeFDiagCursor;
        else if (edges.testFlags(Qt::RightEdge | Qt::TopEdge) ||
                 edges.testFlags(Qt::LeftEdge | Qt::BottomEdge))
            shape = Qt::SizeBDiagCursor;
        else if (edges.testAnyFlags(Qt::LeftEdge | Qt::RightEdge))
            shape = Qt::SizeHorCursor;
        else if (edges.testAnyFlags(Qt::TopEdge | Qt::BottomEdge))
            shape = Qt::SizeVerCursor;
        if (edges != Qt::Edges()) {
            m_handle->setCursor(shape);
            m_edgeCursor = true;
        } else if (m_edgeCursor) {
            m_edgeCursor = false;
            // Back to the cursor of the widget below (it gets no new enter event here).
            QWidget* below = m_window.childAt(p.toPoint());
            m_handle->setCursor(below ? below->cursor() : m_window.cursor());
        }
        return false;
    }
    if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton && edges != Qt::Edges())
        return m_handle->startSystemResize(edges); // false (no window manager): Qt goes on
    return false;
#endif
}

bool TitleStripController::eventFilter(QObject* watched, QEvent* event) {
    if (m_handle && watched == m_handle.data()) {
        switch (event->type()) {
        case QEvent::MouseMove:
            if (m_active) {
                if (handleResizeEdges(event))
                    return true;
                reveal();
            }
            break;
        case QEvent::MouseButtonPress:
            if (m_active && handleResizeEdges(event))
                return true;
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
    if (watched == &m_window) {
        switch (event->type()) {
        case QEvent::Show: // after the native window exists, before it appears
            apply();
            break;
        case QEvent::WinIdChange:
            if (m_window.isVisible())
                apply();
            break;
        case QEvent::WindowStateChange:
            apply(); // full screen: the strip goes at once
            break;
        case QEvent::WindowTitleChange:
            m_strip->setTitle(m_window.windowTitle());
            break;
        case QEvent::ActivationChange:
            if (m_window.isActiveWindow())
                reveal(); // keyboard users see the title briefly
            break;
        case QEvent::Resize:
            // macOS tabs: the tab bar appears or goes (the content view changes size).
            if (m_borderless && m_window.isVisible() && !m_window.isFullScreen() &&
                !m_window.fullScreenAnimating() && wantActive() != m_active)
                apply();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
    if (m_host && watched == m_host.data() && event->type() == QEvent::Resize)
        place();
    return QObject::eventFilter(watched, event);
}

} // namespace lf
