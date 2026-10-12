// SPDX-License-Identifier: GPL-3.0-or-later
#include "NavArrows.h"

#include "ImageView.h"
#include "Loupe.h"
#include "NavigationExtras.h"
#include "OverviewController.h"
#include "SlideshowController.h"
#include "TitleStrip.h"
#include "ViewerWindow.h"
#include "WindowEditing.h"
#include "WindowExtras.h"
#include "WindowIntegration.h"
#include "core/Collection.h"
#include "core/NavArrows.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

constexpr int kShadow = 4; // room around the circle for the shadow

} // namespace

// ---------------------------------------------------------------------------------------
// NavArrowButton
// ---------------------------------------------------------------------------------------

NavArrowButton::NavArrowButton(Side side, QWidget* parent) : QAbstractButton(parent), m_side(side) {
    setObjectName(side == Side::Left ? QStringLiteral("lfNavArrowLeft") : QStringLiteral("lfNavArrowRight"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents); // until fully faded in
    setFocusPolicy(Qt::NoFocus);                    // keyboard users keep ←/→
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor); // not the pan cursor of the image below
    resize(sizeHint());
    m_fade = new QVariantAnimation(this);
    m_fade->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_fade, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { setOpacity(v.toReal()); });
    connect(m_fade, &QVariantAnimation::finished, this, [this] {
        if (!m_target)
            hide();
        else
            setAttribute(Qt::WA_TransparentForMouseEvents, false);
    });
    hide();
}

QSize NavArrowButton::sizeHint() const {
    const int s = NavArrows::kDiameter + 2 * kShadow;
    return {s, s};
}

void NavArrowButton::setOpacity(qreal value) {
    m_opacity = value;
    update();
}

void NavArrowButton::fadeTo(bool shown, int ms) {
    const qreal target = shown ? 1.0 : 0.0;
    if (shown == m_target && (m_fade->state() == QAbstractAnimation::Running || m_opacity == target))
        return;
    m_target = shown;
    m_fade->stop();
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setDown(false);
    if (shown) {
        show();
        raise();
    }
    if (ms <= 0 || m_opacity == target) {
        setOpacity(target);
        if (!shown)
            hide();
        else
            setAttribute(Qt::WA_TransparentForMouseEvents, false);
        return;
    }
    m_fade->setStartValue(m_opacity);
    m_fade->setEndValue(target);
    m_fade->setDuration(std::max(1, int(std::lround(ms * std::abs(target - m_opacity)))));
    m_fade->start();
}

bool NavArrowButton::hitButton(const QPoint& pos) const {
    return rect().contains(pos); // the whole square: at least kDiameter in both directions
}

void NavArrowButton::paintEvent(QPaintEvent*) {
    if (m_opacity <= 0.0)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    const QRectF circle(kShadow, kShadow - 1, NavArrows::kDiameter, NavArrows::kDiameter);
    // Soft shadow: a few transparent rings, slightly below the circle.
    p.setPen(Qt::NoPen);
    for (int i = 1; i <= 3; ++i) {
        p.setBrush(QColor(0, 0, 0, 22));
        p.drawEllipse(circle.adjusted(-i, -i, i, i).translated(0, 1.5));
    }
    // Dark translucent circle: readable on bright and on dark photos, in light and dark mode.
    const bool hover = underMouse() && m_opacity >= 1.0;
    const int alpha = isDown() ? 190 : hover ? 160 : 120;
    p.setBrush(QColor(0, 0, 0, alpha));
    QPen ring(QColor(255, 255, 255, 46), 1.0);
    p.setPen(ring);
    p.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
    // White chevron.
    const QPointF c = circle.center();
    const qreal h = NavArrows::kDiameter * 0.19; // half height of the chevron
    const qreal w = h * 0.55;
    const qreal dir = m_side == Side::Left ? -1.0 : 1.0;
    QPainterPath chevron;
    chevron.moveTo(c.x() - dir * w + dir * 1.0, c.y() - h);
    chevron.lineTo(c.x() + dir * w + dir * 1.0, c.y());
    chevron.lineTo(c.x() - dir * w + dir * 1.0, c.y() + h);
    QPen pen(QColor(255, 255, 255, 240), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(chevron);
}

bool NavArrowButton::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Wheel:
    case QEvent::NativeGesture:
        // Zoom, pan and flipping with the wheel keep working over the button.
        if (auto* view = qobject_cast<ImageView*>(parentWidget()); view && view->canvasWidget())
            return QCoreApplication::sendEvent(view->canvasWidget(), event);
        break;
    case QEvent::Enter:
    case QEvent::Leave:
        update(); // hover look
        break;
    default:
        break;
    }
    return QAbstractButton::event(event);
}

void NavArrowButton::mouseDoubleClickEvent(QMouseEvent* event) {
    // The second click of a double click is another click on the button: never the image's
    // double click (full screen, F-IN-10).
    QAbstractButton::mousePressEvent(event);
    event->accept();
}

// ---------------------------------------------------------------------------------------
// NavArrows
// ---------------------------------------------------------------------------------------

NavArrows::NavArrows(ViewerWindow& window) : QObject(&window), m_window(window) {
    setObjectName(QStringLiteral("lfNavArrows"));
    ImageView* view = window.view();
    m_left = new NavArrowButton(NavArrowButton::Side::Left, view);
    m_right = new NavArrowButton(NavArrowButton::Side::Right, view);
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(kIdleMs);
    connect(m_idle, &QTimer::timeout, this, &NavArrows::onIdle);
    for (NavArrowButton* b : {m_left, m_right}) {
        connect(b, &QAbstractButton::clicked, this, [this, b] { activate(b); });
        b->installEventFilter(this);
    }
    view->installEventFilter(this);
    window.installEventFilter(this);
    // Every mouse move of the window passes its QWindow, whichever widget is below.
    if (QWindow* handle = window.windowHandle())
        handle->installEventFilter(this);

    Collection* c = window.collection();
    connect(c, &Collection::currentChanged, this, &NavArrows::sync);
    connect(c, &Collection::entriesChanged, this, &NavArrows::sync);
    connect(c, &Collection::loadingFinished, this, &NavArrows::sync);
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::ShowNavArrows) || k == QLatin1String(key::WrapMode) ||
            k == QLatin1String(key::ArchiveRightToLeft))
            sync();
    });
    if (WindowExtras* extras = window.extras()) {
        connect(extras->overview(), &OverviewController::modeChanged, this, &NavArrows::sync);
        connect(extras->navigation(), &NavigationExtras::shuffleChanged, this, &NavArrows::sync);
    }
    if (SlideshowController* show = window.slideshow())
        connect(show, &SlideshowController::stateChanged, this, &NavArrows::sync);
    retranslate();
}

bool NavArrows::allowedNow() const {
    navarrows::Context c;
    c.enabled = Settings::instance().value(key::ShowNavArrows).toBool();
    c.hasImage = m_window.collection()->count() > 0;
    const OverviewController* overview = m_window.extras() ? m_window.extras()->overview() : nullptr;
    c.viewShown = m_window.view()->isVisible() &&
                  !(overview && (overview->isGridShown() || overview->isCompareShown()));
    c.cropping = m_window.editing() && m_window.editing()->cropActive();
    c.loupe = m_window.extras() && m_window.extras()->loupe()->isActive();
    c.slideshow = m_window.isSlideshowRunning();
    c.liveText = m_window.integration() && m_window.integration()->liveTextActive();
    return navarrows::allowed(c);
}

void NavArrows::sync() {
    const Collection* coll = m_window.collection();
    navarrows::Position p;
    p.count = coll->count();
    p.index = coll->currentIndex();
    p.end = navarrows::endBehaviorFromId(Settings::instance().value(key::WrapMode).toString());
    p.canSwitchFolder = coll->isFolderCollection() || !coll->containerPath().isEmpty();
    p.shuffle = m_window.extras() && m_window.extras()->navigation()->shuffle();
    p.rightToLeft = m_window.integration() && m_window.integration()->rightToLeftActive();
    const navarrows::Buttons b = navarrows::buttons(p);
    const bool swapped = b.left != m_leftAction;
    m_leftAction = b.left;
    m_rightAction = b.right;
    if (swapped)
        retranslate();
    const bool allowed = allowedNow();
    m_leftWanted = allowed && b.leftShown;
    m_rightWanted = allowed && b.rightShown;
    if (!allowed)
        m_idle->stop();
    // Without a target (or in a mode without buttons) at once; nothing new appears here,
    // only a mouse move reveals.
    if (!m_leftWanted)
        m_left->fadeTo(false, 0);
    if (!m_rightWanted)
        m_right->fadeTo(false, 0);
}

void NavArrows::reveal() {
    sync();
    if (!m_leftWanted && !m_rightWanted)
        return;
    place();
    if (m_leftWanted)
        m_left->fadeTo(true, kFadeInMs);
    if (m_rightWanted)
        m_right->fadeTo(true, kFadeInMs);
    m_idle->start();
}

void NavArrows::onIdle() {
    const bool hovered =
        (m_left->isVisible() && m_left->underMouse()) || (m_right->isVisible() && m_right->underMouse());
    if (hovered || QGuiApplication::mouseButtons() != Qt::NoButton) {
        m_idle->start();
        return;
    }
    m_left->fadeTo(false, kFadeOutMs);
    m_right->fadeTo(false, kFadeOutMs);
}

void NavArrows::place() {
    ImageView* view = m_window.view();
    // In the borderless window the title strip lies over the top of the image: centre the
    // buttons in the part below it (F-VIEW-15).
    int top = 0;
    if (WindowExtras* extras = m_window.extras();
        extras && extras->titleStrip() && extras->titleStrip()->active()) {
        const TitleStrip* strip = extras->titleStrip()->strip();
        const QPoint stripBottom =
            view->mapFrom(m_window.window(), strip->mapTo(m_window.window(), QPoint(0, strip->height())));
        top = std::clamp(stripBottom.y(), 0, view->height() / 3);
    }
    const QSize s = m_left->sizeHint();
    const int y = top + (view->height() - top - s.height()) / 2;
    const int margin = kEdgeMargin - kShadow; // the circle keeps kEdgeMargin from the edge
    m_left->setGeometry(margin, y, s.width(), s.height());
    m_right->setGeometry(view->width() - margin - s.width(), y, s.width(), s.height());
}

void NavArrows::activate(NavArrowButton* button) {
    const Action a = button == m_left ? m_leftAction : m_rightAction;
    m_window.perform(a); // like ←/→: wrap, next folder, archives, coupled windows
    reveal();            // stay for the next click; a button without target goes
}

void NavArrows::retranslate() {
    // The labels of the actions (same texts as the menu and the shortcut editor).
    const auto apply = [](NavArrowButton* b, Action a, const QString& key) {
        const QString name = actionLabel(a);
        b->setAccessibleName(name);
        //: Tooltip of the arrow buttons at the image edge: action name and its key (←/→)
        b->setToolTip(NavArrows::tr("%1 (%2)").arg(name, key));
    };
    apply(m_left, m_leftAction, QStringLiteral("←"));
    apply(m_right, m_rightAction, QStringLiteral("→"));
}

bool NavArrows::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseMove:
        if (watched == m_window.windowHandle()) {
            const auto* me = static_cast<QMouseEvent*>(event);
            ImageView* view = m_window.view();
            if (me->buttons() == Qt::NoButton &&
                view->rect().contains(view->mapFromGlobal(me->globalPosition().toPoint())))
                reveal();
        }
        break;
    case QEvent::Show:
    case QEvent::WinIdChange:
        if (watched == &m_window) {
            if (QWindow* handle = m_window.windowHandle())
                handle->installEventFilter(this); // idempotent
        }
        break;
    case QEvent::Resize:
        if (watched == m_window.view() && (m_left->isVisible() || m_right->isVisible()))
            place();
        break;
    case QEvent::LanguageChange:
        if (watched == m_left)
            retranslate();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace lf
