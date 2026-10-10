// SPDX-License-Identifier: GPL-3.0-or-later
#include "HdrController.h"

#include "ImageView.h"
#include "ViewerWindow.h"
#include "core/InputBindings.h"
#include "core/LocaleFormat.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lf {

namespace {

// Slider positions are thirds of a stop.
constexpr int kThirds = 3;

} // namespace

HdrController::HdrController(ViewerWindow& window)
    : QObject(&window), m_window(window), m_view(window.view()) {
    setObjectName(QStringLiteral("lfHdrController"));
    Settings& st = Settings::instance();
    m_view->setHdrEnabled(st.value(key::HdrEnabled).toBool());
    connect(&st, &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::HdrEnabled))
            m_view->setHdrEnabled(Settings::instance().value(key::HdrEnabled).toBool());
    });
    connect(m_view, &ImageView::hdrStateChanged, this, &HdrController::refresh);
    m_view->installEventFilter(this); // HUD placement on resize
}

HdrController::~HdrController() = default;

HdrController* HdrController::of(ViewerWindow* window) {
    return window ? window->findChild<HdrController*>(QStringLiteral("lfHdrController"),
                                                      Qt::FindDirectChildrenOnly)
                  : nullptr;
}

bool HdrController::perform(Action action) {
    switch (action) {
    case Action::ToggleHdr: {
        Settings& st = Settings::instance();
        const bool on = !st.value(key::HdrEnabled).toBool();
        st.setValue(key::HdrEnabled, on);
        if (m_view->hdrDecision().badge)
            m_window.showMessage(on ? tr("HDR an") : tr("HDR aus – SDR-Vorschau"));
        return true;
    }
    case Action::ExposureIncrease:
    case Action::ExposureDecrease:
    case Action::ExposureReset:
        if (!m_view->exposureApplies()) {
            if (m_view->image())
                m_window.showMessage(tr("Die Belichtung lässt sich nur bei HDR-Bildern einstellen."));
            return true;
        }
        if (action == Action::ExposureReset)
            m_view->setExposureEv(0.0);
        else
            m_view->stepExposure(action == Action::ExposureIncrease ? 1 : -1);
        m_window.showMessage(tr("Belichtung %1").arg(exposureText()));
        return true;
    case Action::ToggleExposureHud:
        setHudRequested(!m_hudRequested);
        return true;
    default:
        return false;
    }
}

void HdrController::setHudRequested(bool on) {
    if (m_hudRequested == on)
        return;
    m_hudRequested = on;
    if (on && !m_view->exposureApplies() && m_view->image())
        m_window.showMessage(tr("Der Belichtungsregler erscheint bei HDR-Bildern."));
    refresh();
}

bool HdrController::isHudVisible() const {
    return m_hud && !m_hud->isHidden();
}

QFrame* HdrController::hud() const {
    return m_hud;
}

QString HdrController::exposureText() const {
    return formatExposureBias(m_view->exposureEv());
}

QString HdrController::badgeText() const {
    const hdr::Decision d = m_view->hdrDecision();
    if (!d.badge)
        return {};
    QString text = d.presentation == hdr::Presentation::Extended
                       ? tr("HDR")
                       //: status bar badge: HDR image shown tone-mapped to SDR
                       : tr("HDR (SDR-Vorschau)");
    if (m_view->exposureEv() != 0.0)
        text = tr("%1 · %2").arg(text, exposureText());
    return text;
}

QString HdrController::badgeToolTip() const {
    const hdr::Decision d = m_view->hdrDecision();
    if (!d.badge)
        return {};
    if (d.presentation == hdr::Presentation::Extended)
        return tr("HDR-Bild, mit erweitertem Dynamikumfang angezeigt (⇧⌘H: SDR-Vorschau)");
    if (!m_view->hdrEnabled())
        return tr("HDR-Bild, als SDR-Vorschau angezeigt – HDR ist ausgeschaltet (⇧⌘H)");
    return tr("HDR-Bild, als SDR-Vorschau angezeigt – dieser Bildschirm kann kein HDR darstellen");
}

void HdrController::attachStatusBar(QStatusBar* bar) {
    if (!bar || m_badge)
        return;
    m_badge = new QLabel(bar);
    m_badge->setObjectName(QStringLiteral("lfStatusHdr"));
    m_badge->setContentsMargins(6, 0, 6, 0);
    bar->addWidget(m_badge);
    refresh();
}

void HdrController::retranslate() {
    if (m_hud) {
        m_hudTitle->setText(tr("Belichtung"));
        m_reset->setToolTip(tr("Belichtung zurücksetzen"));
        m_lock->setText(tr("Beibehalten"));
        m_lock->setToolTip(tr("Belichtung beim Bildwechsel beibehalten"));
    }
    refresh();
}

void HdrController::ensureHud() {
    if (m_hud)
        return;
    m_hud = new QFrame(m_view);
    m_hud->setObjectName(QStringLiteral("lfExposureHud"));
    m_hud->setAttribute(Qt::WA_StyledBackground);
    m_hud->setFocusPolicy(Qt::NoFocus);
    m_hud->setStyleSheet(QStringLiteral(
        "#lfExposureHud { background: rgba(20, 20, 20, 210); border-radius: 10px; "
        "border: 1px solid rgba(255, 255, 255, 40); }"
        "#lfExposureHud QLabel, #lfExposureHud QCheckBox { color: #f2f2f2; background: transparent; }"
        "#lfExposureHud QToolButton { color: #f2f2f2; background: rgba(255, 255, 255, 28); "
        "border: 1px solid rgba(255, 255, 255, 90); border-radius: 5px; padding: 1px 8px; }"));
    auto* layout = new QVBoxLayout(m_hud);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout;
    m_hudTitle = new QLabel(m_hud);
    m_value = new QLabel(m_hud);
    m_value->setObjectName(QStringLiteral("lfExposureValue"));
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_value->setMinimumWidth(m_value->fontMetrics().horizontalAdvance(QStringLiteral("+8,00 EV")));
    top->addWidget(m_hudTitle);
    top->addStretch();
    top->addWidget(m_value);
    layout->addLayout(top);
    m_slider = new QSlider(Qt::Horizontal, m_hud);
    m_slider->setObjectName(QStringLiteral("lfExposureSlider"));
    m_slider->setRange(int(hdr::Exposure::kMinEv) * kThirds, int(hdr::Exposure::kMaxEv) * kThirds);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(kThirds);
    m_slider->setTickPosition(QSlider::TicksBelow);
    m_slider->setTickInterval(kThirds);
    m_slider->setFocusPolicy(Qt::NoFocus); // arrow keys keep flipping images
    m_slider->setMinimumWidth(220);
    layout->addWidget(m_slider);
    auto* bottom = new QHBoxLayout;
    m_lock = new QCheckBox(m_hud);
    m_lock->setObjectName(QStringLiteral("lfExposureLock"));
    m_lock->setFocusPolicy(Qt::NoFocus);
    m_reset = new QToolButton(m_hud);
    m_reset->setObjectName(QStringLiteral("lfExposureReset"));
    m_reset->setText(QStringLiteral("0"));
    m_reset->setFocusPolicy(Qt::NoFocus);
    bottom->addWidget(m_lock);
    bottom->addStretch();
    bottom->addWidget(m_reset);
    layout->addLayout(bottom);
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        if (!m_updating)
            m_view->setExposureEv(double(v) / kThirds);
    });
    connect(m_reset, &QToolButton::clicked, this, [this] { m_view->setExposureEv(0.0); });
    connect(m_lock, &QCheckBox::toggled, this, [this](bool on) { m_view->setExposureLocked(on); });
    retranslate();
}

void HdrController::layoutHud() {
    if (!m_hud)
        return;
    m_hud->adjustSize();
    const QSize s = m_hud->sizeHint();
    m_hud->setGeometry(16, std::max(8, m_view->height() - s.height() - 16), s.width(), s.height());
}

void HdrController::refresh() {
    if (m_badge) {
        const QString text = badgeText();
        m_badge->setText(text);
        m_badge->setToolTip(badgeToolTip());
        m_badge->setVisible(!text.isEmpty());
    }
    const bool showHud = m_hudRequested && m_view->exposureApplies();
    if (showHud)
        ensureHud();
    if (!m_hud)
        return;
    m_updating = true;
    m_slider->setValue(int(std::lround(m_view->exposureEv() * kThirds)));
    m_updating = false;
    m_value->setText(exposureText());
    m_lock->setChecked(m_view->exposureLocked());
    m_reset->setEnabled(m_view->exposureEv() != 0.0);
    if (showHud) {
        layoutHud();
        m_hud->show();
        m_hud->raise();
    } else {
        m_hud->hide();
    }
}

bool HdrController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::Resize)
        layoutHud();
    return QObject::eventFilter(watched, event);
}

} // namespace lf
