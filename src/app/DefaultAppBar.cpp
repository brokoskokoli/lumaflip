// SPDX-License-Identifier: GPL-3.0-or-later
#include "DefaultAppBar.h"

#include "Application.h"
#include "SlideshowController.h"
#include "TitleStrip.h"
#include "ViewerWindow.h"
#include "WindowExtras.h"
#include "WindowModes.h"
#include "core/DefaultAppOffer.h"
#include "core/Settings.h"

#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace lf {

namespace {

constexpr int kResultToastMs = 4000;
constexpr int kErrorToastMs = 8000;

struct QueryResult {
    bool installed = false;
    DefaultViewerStatus status;
};

} // namespace

// ---- the bar ----

DefaultAppBar::DefaultAppBar(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("lfDefaultAppBar"));
    setAttribute(Qt::WA_StyledBackground);
    // Like the access hint (F-PRIV-01), in a calm blue: information, not a warning.
    setStyleSheet(QStringLiteral("#lfDefaultAppBar { background: rgba(10, 132, 255, 40); "
                                 "border-bottom: 1px solid rgba(128, 128, 128, 90); }"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 8, 6);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("lfDefaultAppText"));
    m_text->setWordWrap(true);
    m_set = new QPushButton(this);
    m_set->setObjectName(QStringLiteral("lfDefaultAppSet"));
    m_all = new QPushButton(this);
    m_all->setObjectName(QStringLiteral("lfDefaultAppAll"));
    m_never = new QPushButton(this);
    m_never->setObjectName(QStringLiteral("lfDefaultAppNever"));
    m_close = new QToolButton(this);
    m_close->setObjectName(QStringLiteral("lfDefaultAppClose"));
    m_close->setAutoRaise(true);
    m_close->setText(QStringLiteral("✕")); // symbol, not text
    for (QAbstractButton* b :
         {static_cast<QAbstractButton*>(m_set), static_cast<QAbstractButton*>(m_all),
          static_cast<QAbstractButton*>(m_never), static_cast<QAbstractButton*>(m_close)})
        b->setFocusPolicy(Qt::NoFocus); // the keys stay with the image
    layout->addWidget(m_text, 1);
    layout->addWidget(m_set);
    layout->addWidget(m_all);
    layout->addWidget(m_never);
    layout->addWidget(m_close);
    connect(m_set, &QPushButton::clicked, this, &DefaultAppBar::setRequested);
    connect(m_all, &QPushButton::clicked, this, &DefaultAppBar::allFormatsRequested);
    connect(m_never, &QPushButton::clicked, this, &DefaultAppBar::neverRequested);
    connect(m_close, &QToolButton::clicked, this, &DefaultAppBar::closeRequested);
    retranslate();
}

void DefaultAppBar::setType(const QString& type, const QString& typeName) {
    m_type = type;
    m_typeName = typeName.isEmpty() ? type : typeName;
    retranslate();
}

QString DefaultAppBar::text() const {
    return m_text->text();
}

void DefaultAppBar::setTopInset(int px) {
    QMargins m = layout()->contentsMargins();
    m.setTop(6 + std::max(0, px));
    layout()->setContentsMargins(m);
}

void DefaultAppBar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslate(); // F-I18N-03
    QFrame::changeEvent(event);
}

void DefaultAppBar::retranslate() {
    m_text->setText(tr("Lumaflip ist nicht die Standard-App für den Dateityp „%1“.").arg(m_typeName));
    m_set->setText(tr("Festlegen"));
    m_set->setToolTip(tr("Lumaflip öffnet Dateien dieses Typs künftig per Doppelklick."));
    m_all->setText(tr("Alle Bildformate …"));
    m_all->setToolTip(tr("Wähle die Bildformate, die Lumaflip öffnen soll."));
    m_never->setText(tr("Nicht mehr fragen"));
    m_close->setToolTip(tr("Schließen"));
    m_close->setAccessibleName(tr("Schließen"));
}

// ---- the controller ----

DefaultAppOfferController::DefaultAppOfferController(Application& app, int startDelayMs, int settleMs)
    : QObject(&app), m_app(app), m_startDelayMs(startDelayMs), m_settleMs(settleMs) {
    setObjectName(QStringLiteral("lfDefaultAppOffer"));
    m_pool.setMaxThreadCount(1);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &DefaultAppOfferController::evaluate);
    connect(&app, &Application::windowsChanged, this, &DefaultAppOfferController::watchWindows);
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::OfferDefaultApp) && !defaultapp::isEnabled())
            hideBar(); // switched off in the preferences
    });
    watchWindows();
}

DefaultAppOfferController::~DefaultAppOfferController() {
    m_pool.waitForDone(); // the worker uses the application's shell
}

DefaultAppBar* DefaultAppOfferController::visibleBar() const {
    if (!m_barWindow)
        return nullptr;
    DefaultAppBar* bar = m_bars.value(m_barWindow.data());
    return bar && !bar->isHidden() ? bar : nullptr;
}

void DefaultAppOfferController::watchWindows() {
    for (ViewerWindow* w : m_app.windows()) {
        if (w->property("lfDefaultAppHook").toBool())
            continue; // windowsChanged comes often: one connection per window
        w->setProperty("lfDefaultAppHook", true);
        w->installEventFilter(this);
        // Any stage: a small image may only ever come as a complete preview. The query waits
        // for kStartDelayMs after the first image anyway (P-01).
        connect(w, &ViewerWindow::imageShown, this,
                [this, w](const QString& path, Quality) { imageShown(w, path); });
        connect(w, &QObject::destroyed, this, [this, w] { m_bars.remove(w); });
    }
}

void DefaultAppOfferController::imageShown(ViewerWindow* window, const QString& path) {
    if (visibleBar() ||
        defaultapp::beforeQuery(defaultapp::situationFor(path, false)) != defaultapp::Decision::Show)
        return; // cheap: nothing to schedule
    m_pendingWindow = window;
    m_pendingPath = path;
    if (!m_sinceFirstImage.isValid())
        m_sinceFirstImage.start();
    // Not before kStartDelayMs after the first image (P-01), then once an image stays.
    const qint64 untilStart = m_startDelayMs - m_sinceFirstImage.elapsed();
    m_timer->start(int(std::max<qint64>(m_settleMs, untilStart)));
}

bool DefaultAppOfferController::fullScreenOrSlideshow(const ViewerWindow* window) {
    return window->fullScreenTarget() || window->isFullScreen() || window->isSlideshowRunning();
}

void DefaultAppOfferController::evaluate() {
    ViewerWindow* window = m_pendingWindow.data();
    const QString path = m_pendingPath;
    if (!window || path.isEmpty() || window->currentPath() != path || m_queryRunning || visibleBar())
        return;
    if (defaultapp::beforeQuery(defaultapp::situationFor(path, fullScreenOrSlideshow(window))) !=
        defaultapp::Decision::Show)
        return;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (m_seenSuffixes.contains(suffix))
        return; // asked the system about this kind of file before (this session)
    m_seenSuffixes.insert(suffix);
    m_queryRunning = true;
    ++m_queries;
    Shell* shell = &m_app.shell();
    auto* watcher = new QFutureWatcher<QueryResult>(this);
    QPointer<ViewerWindow> target(window);
    connect(watcher, &QFutureWatcher<QueryResult>::finished, this, [this, watcher, target, path, suffix] {
        const QueryResult r = watcher->result();
        watcher->deleteLater();
        m_queryRunning = false;
        ViewerWindow* w = target.data();
        const defaultapp::Situation s = defaultapp::situationFor(path, w ? fullScreenOrSlideshow(w) : true);
        const defaultapp::Decision d =
            defaultapp::afterQuery(s, r.installed, r.status, defaultapp::askedTypes());
        if (d == defaultapp::Decision::FullScreen)
            m_seenSuffixes.remove(suffix); // ask again once the window is back
        if (d == defaultapp::Decision::Show && w && !visibleBar()) {
            showBar(w, r.status);
            m_barSuffix = suffix;
        }
        emit queryFinished();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [shell, path] {
        QueryResult r;
        r.installed = shell->isInstalledApp();
        if (r.installed)
            r.status = shell->defaultViewerStatus(path);
        return r;
    }));
}

void DefaultAppOfferController::showBar(ViewerWindow* window, const DefaultViewerStatus& status) {
    DefaultAppBar* bar = m_bars.value(window);
    if (!bar) {
        bar = new DefaultAppBar;
        window->insertTopBar(bar);
        m_bars.insert(window, bar);
        QPointer<ViewerWindow> w(window);
        connect(bar, &DefaultAppBar::setRequested, this, [this, w] {
            if (w)
                setDefault(w);
        });
        connect(bar, &DefaultAppBar::allFormatsRequested, this, [this, w] {
            if (!w)
                return;
            answer(m_bars.value(w)->type());
            m_app.makeDefaultViewer(); // F-MAC-04 checklist; its result is shown there
        });
        connect(bar, &DefaultAppBar::neverRequested, this, [this, w] {
            defaultapp::setEnabled(false);
            hideBar();
            if (w)
                w->showMessage(tr("Lumaflip fragt nicht mehr nach der Standard-App – wieder einschalten "
                                  "unter Einstellungen → Allgemein"),
                               kResultToastMs);
        });
        connect(bar, &DefaultAppBar::closeRequested, this, [this, w] {
            if (w)
                answer(m_bars.value(w)->type());
        });
    }
    bar->setType(status.type, status.typeName);
    m_barWindow = window;
    updateInset(window);
    bar->show();
    emit barShown(window);
}

void DefaultAppOfferController::hideBar() {
    if (DefaultAppBar* bar = visibleBar())
        bar->hide();
    m_barWindow = nullptr;
}

void DefaultAppOfferController::answer(const QString& type) {
    defaultapp::markAsked(type);
    hideBar();
}

void DefaultAppOfferController::setDefault(ViewerWindow* window) {
    DefaultAppBar* bar = m_bars.value(window);
    if (!bar)
        return;
    const QString type = bar->type();
    const QString name = bar->typeName();
    answer(type);
    QPointer<ViewerWindow> w(window);
    auto done = [w, name](bool ok, const QString& error) {
        if (!w)
            return;
        if (ok)
            w->showMessage(tr("Lumaflip ist jetzt die Standard-App für „%1“").arg(name), kResultToastMs);
        else
            w->showMessage(tr("Lumaflip konnte nicht als Standard-App festgelegt werden: %1").arg(error),
                           kErrorToastMs);
    };
    auto pending = [w](const QString& message) {
        if (w)
            w->showMessage(message, kErrorToastMs);
    };
    m_app.shell().makeDefaultViewerAsync({type}, window, done, pending);
}

void DefaultAppOfferController::updateInset(ViewerWindow* window) {
    DefaultAppBar* bar = m_bars.value(window);
    if (!bar)
        return;
    // F-VIEW-15: the title strip fades in over the top of the borderless window; the
    // buttons stay below it.
    const bool strip =
        window->extras() && window->extras()->titleStrip() && window->extras()->titleStrip()->active();
    bar->setTopInset(strip ? titleStripHeight(window) : 0);
}

bool DefaultAppOfferController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show) {
        auto* window = qobject_cast<ViewerWindow*>(watched);
        if (window && window == m_barWindow.data()) {
            if (fullScreenOrSlideshow(window)) {
                hideBar(); // never in full screen; not an answer: asked again later
                m_seenSuffixes.remove(m_barSuffix);
            } else
                updateInset(window);
        }
    }
    return false;
}

} // namespace lf
