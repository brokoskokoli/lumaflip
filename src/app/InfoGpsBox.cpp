// SPDX-License-Identifier: GPL-3.0-or-later
#include "InfoGpsBox.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace lf {

namespace {
constexpr int kMapWidth = 240;
constexpr int kMapHeight = 150;
} // namespace

InfoGpsBox::InfoGpsBox(std::unique_ptr<MapSnapshotProvider> provider, QWidget* parent)
    : QWidget(parent), m_provider(std::move(provider)) {
    setObjectName(QStringLiteral("lfGpsBox"));
    m_controller = new MapPreviewController(m_provider.get(), this);
    m_controller->setSize(QSize(kMapWidth, kMapHeight), 2.0);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_coords = new QLabel(this);
    m_coords->setObjectName(QStringLiteral("lfGpsCoordinates"));
    m_coords->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_coords->setWordWrap(true);
    m_coords->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_coords);
    auto* buttons = new QHBoxLayout;
    m_showMap = new QPushButton(this);
    m_showMap->setObjectName(QStringLiteral("lfGpsShowMap"));
    m_showMap->setFocusPolicy(Qt::NoFocus);
    m_hideMap = new QPushButton(this);
    m_hideMap->setObjectName(QStringLiteral("lfGpsHideMap"));
    m_hideMap->setFocusPolicy(Qt::NoFocus);
    m_openMaps = new QPushButton(this);
    m_openMaps->setObjectName(QStringLiteral("lfGpsOpenMaps"));
    m_openMaps->setFocusPolicy(Qt::NoFocus);
    buttons->addWidget(m_showMap);
    buttons->addWidget(m_hideMap);
    buttons->addWidget(m_openMaps);
    buttons->addStretch();
    layout->addLayout(buttons);
    // F-INFO-08: the folder map (its own panel; opening it is the consent, SPEC 10.3).
    auto* folderRow = new QHBoxLayout;
    m_folderMap = new QPushButton(this);
    m_folderMap->setObjectName(QStringLiteral("lfGpsFolderMap"));
    m_folderMap->setFocusPolicy(Qt::NoFocus);
    folderRow->addWidget(m_folderMap);
    folderRow->addStretch();
    layout->addLayout(folderRow);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("lfGpsStatus"));
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);
    m_map = new QLabel(this);
    m_map->setObjectName(QStringLiteral("lfGpsMap"));
    m_map->setFixedSize(kMapWidth, kMapHeight);
    m_map->setScaledContents(false);
    m_map->hide();
    layout->addWidget(m_map);

    connect(m_showMap, &QPushButton::clicked, this, [this] {
        const qreal dpr = devicePixelRatioF();
        m_controller->setSize(QSize(kMapWidth, kMapHeight), dpr > 0 ? dpr : 2.0);
        m_controller->showMap(); // the click is the consent (SPEC 10.3)
    });
    connect(m_hideMap, &QPushButton::clicked, this, [this] { m_controller->hideMap(); });
    connect(m_folderMap, &QPushButton::clicked, this, &InfoGpsBox::folderMapRequested);
    connect(m_openMaps, &QPushButton::clicked, this, [this] {
        if (const auto p = m_controller->location()) {
            const QUrl url = geo::mapsUrl(*p, m_label);
            if (m_opener)
                m_opener(url);
            else
                QDesktopServices::openUrl(url);
        }
    });
    connect(m_coords, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const auto p = m_controller->location();
        if (!p)
            return;
        QMenu menu(this);
        menu.addAction(tr("Koordinaten kopieren"), this,
                       [p] { QGuiApplication::clipboard()->setText(geo::machineText(*p)); });
        menu.exec(m_coords->mapToGlobal(pos));
    });
    connect(m_controller, &MapPreviewController::stateChanged, this, &InfoGpsBox::updateState);
    retranslateUi();
    updateState();
}

InfoGpsBox::~InfoGpsBox() {
    if (m_provider)
        m_provider->cancel();
}

void InfoGpsBox::setLocation(const std::optional<GeoPoint>& p, const QString& label) {
    m_label = label;
    m_controller->setLocation(p);
    updateState();
}

void InfoGpsBox::setPanelVisible(bool visible) {
    m_controller->setActive(visible);
}

void InfoGpsBox::updateState() {
    const auto p = m_controller->location();
    setVisible(p.has_value());
    if (!p)
        return;
    m_coords->setText(geo::formatDms(*p));
    m_coords->setToolTip(geo::formatDecimal(*p));
    using S = MapPreviewController::State;
    const S s = m_controller->state();
    m_showMap->setVisible(s == S::Idle || s == S::Failed);
    m_hideMap->setVisible(m_controller->hasConsent() && (s == S::Loading || s == S::Ready || s == S::Failed));
    m_status->setVisible(s == S::Loading || s == S::Failed);
    if (s == S::Loading)
        m_status->setText(tr("Karte wird geladen …"));
    else if (s == S::Failed)
        m_status->setText(m_controller->error());
    m_map->setVisible(s == S::Ready);
    if (s == S::Ready)
        m_map->setPixmap(QPixmap::fromImage(m_controller->image()));
}

void InfoGpsBox::retranslateUi() {
    m_showMap->setText(tr("Karte anzeigen"));
    m_showMap->setToolTip(tr("Lädt einen Kartenausschnitt von Apple Karten (Internetverbindung)"));
    m_hideMap->setText(tr("Karte ausblenden"));
    m_hideMap->setToolTip(
        tr("Keine Kartenausschnitte mehr laden (bis zum nächsten Klick auf „Karte anzeigen“)"));
    m_openMaps->setText(tr("In Karten öffnen"));
    m_folderMap->setText(tr("Ordnerkarte"));
    m_folderMap->setToolTip(tr("Alle Bilder dieser Ansicht mit Aufnahmeort auf einer Karte zeigen (lädt "
                               "Kartendaten von Apple Karten)"));
    updateState();
}

void InfoGpsBox::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
}

} // namespace lf
