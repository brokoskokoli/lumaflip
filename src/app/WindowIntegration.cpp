// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowIntegration.h"

#include "Application.h"
#include "CustomActionsUi.h"
#include "ImageView.h"
#include "LiveTextOverlay.h"
#include "MacIntegration.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/ContainerPolicy.h"
#include "core/LiveText.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"
#include "core/Wallpaper.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QTransform>
#include <QWindow>

namespace lf {

namespace {

bool isMember(const QString& path) {
    return vpath::isMember(path);
}

} // namespace

WindowIntegration::WindowIntegration(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfWindowIntegration"));
    m_overlay = new LiveTextOverlay(window.view(), this);
    m_session = new LiveTextSession(m_overlay, this);
    m_overlay->setSession(m_session);
    m_session->setAllowed(Settings::instance().value(key::LiveTextEnabled).toBool());
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::LiveTextEnabled))
            m_session->setAllowed(Settings::instance().value(key::LiveTextEnabled).toBool());
    });
    connect(m_session, &LiveTextSession::stateChanged, this, [this] {
        m_overlay->setVisible(m_session->overlayVisible());
        m_window.refreshStatus();
    });
    ImageView* view = window.view();
    // The overlay follows zoom, pan and resizes; HDR presentation blocks it (EDR surface).
    connect(view, &ImageView::zoomChanged, this, [this] { m_overlay->updateGeometry(); });
    connect(view, &ImageView::viewTransformChanged, this, [this] { m_overlay->updateGeometry(); });
    connect(view, &ImageView::hdrStateChanged, this, &WindowIntegration::updateLiveTextBlocking);
    view->installEventFilter(this);
}

WindowIntegration::~WindowIntegration() = default;

WindowIntegration* WindowIntegration::of(ViewerWindow* window) {
    return window ? window->findChild<WindowIntegration*>(QStringLiteral("lfWindowIntegration"),
                                                          Qt::FindDirectChildrenOnly)
                  : nullptr;
}

bool WindowIntegration::liveTextAvailable() {
    return LiveTextOverlay::isAvailable();
}

bool WindowIntegration::liveTextActive() const {
    return m_session->isActive();
}

bool WindowIntegration::rightToLeftActive() const {
    return m_window.collection()->isContainerCollection() &&
           vpath::isComicArchive(m_window.collection()->containerPath()) &&
           Settings::instance().value(key::ArchiveRightToLeft).toBool();
}

Action WindowIntegration::adjustInput(Action action, const InputEvent& event) const {
    return containers::readingDirection(action, event, rightToLeftActive());
}

bool WindowIntegration::refuseInContainer(Action action) {
    const QString cur = m_window.currentPath();
    if (!isMember(cur) || !containers::isBlocked(action))
        return false;
    m_window.showMessage(containers::blockedMessage(), 3500);
    return true;
}

bool WindowIntegration::perform(Action action) {
    const QString cur = m_window.currentPath();
    // F-OPEN-10: "Im Finder zeigen" shows the archive; copying the path copies its path.
    if (action == Action::RevealInFinder && isMember(cur)) {
        m_app.shell().revealInFileManager(vpath::containerOf(cur));
        return true;
    }
    if (refuseInContainer(action))
        return true;
    switch (action) {
    case Action::QuickLook:
        quickLook();
        return true;
    case Action::ToggleLiveText:
        toggleLiveText();
        return true;
    case Action::SetWallpaper:
        askWallpaper();
        return true;
    case Action::ShowActionLog:
        CustomActionsManager::instance().showLog(&m_window);
        return true;
    case Action::ToggleRightToLeft: {
        Settings& st = Settings::instance();
        const bool on = !st.value(key::ArchiveRightToLeft).toBool();
        st.setValue(key::ArchiveRightToLeft, on);
        m_window.showMessage(on ? tr("Comics: von rechts nach links lesen (← = weiter)")
                                : tr("Comics: von links nach rechts lesen"));
        return true;
    }
    default:
        return false;
    }
}

void WindowIntegration::currentChanged(const QString& path) {
    if (!m_servicesInstalled && m_window.windowHandle() && m_window.windowHandle()->handle()) {
        // F-MAC-07: the current image for other apps' services.
        QPointer<ViewerWindow> w(&m_window);
        macint::installServiceResponder(
            &m_window,
            [w] {
                services::Context c;
                if (!w)
                    return c;
                c.path = w->currentPath();
                const auto img = w->view()->image();
                c.imageAvailable = img && !img->image.isNull() && w->shownPath() == c.path;
                return c;
            },
            [w] {
                if (!w || !w->view()->image())
                    return QImage();
                return w->view()->image()->image;
            });
        m_servicesInstalled = true;
    }
    if (macint::quickLookVisible() && !path.isEmpty() && !isMember(path))
        macint::quickLookFollow(&m_window, path);
    updateLiveTextImage();
}

void WindowIntegration::imageShown(const QString& path) {
    Q_UNUSED(path);
    updateLiveTextImage();
    m_overlay->updateGeometry();
}

// ---- Live Text ----

void WindowIntegration::toggleLiveText() {
    // Switching off always works (also after Live Text was disabled in the settings while
    // it was on); the checks below only apply to switching on.
    if (!m_session->isActive() && !liveTextAvailable()) {
        m_window.showMessage(tr("Live Text ist auf diesem Mac nicht verfügbar."), 3000);
        return;
    }
    if (!m_session->isActive() && !Settings::instance().value(key::LiveTextEnabled).toBool()) {
        m_window.showMessage(tr("Live Text ist in den Einstellungen ausgeschaltet."), 3000);
        return;
    }
    const bool on = m_session->toggle();
    updateLiveTextBlocking();
    updateLiveTextImage();
    m_window.showMessage(on ? tr("Live Text an – Text im Bild auswählen und kopieren") : tr("Live Text aus"));
    m_app.windowChanged(&m_window);
}

void WindowIntegration::updateLiveTextBlocking() {
    QString reason;
    if (m_window.view()->isExtendedPresentationActive())
        reason =
            tr("Live Text ist während der HDR-Darstellung nicht verfügbar – mit ⇧⌘H auf SDR umschalten.");
    m_session->setBlocked(reason);
}

void WindowIntegration::updateLiveTextImage() {
    if (!m_session->isActive())
        return;
    const QString cur = m_window.currentPath();
    const auto img = m_window.view()->image();
    if (cur.isEmpty() || m_window.shownPath() != cur || !img) {
        m_session->setImage(cur, QImage());
        return;
    }
    // What the view shows: its rotation and mirroring apply to the analysed image, so the
    // overlay's text boxes sit on the text (the key includes them: a turn analyses again).
    QImage shown = img->image;
    const int turns = m_window.view()->rotation();
    if (turns % 4 != 0)
        shown = shown.transformed(QTransform().rotate(90.0 * turns));
    m_session->setImage(cur + QStringLiteral("|r%1").arg(turns), shown);
}

// ---- Quick Look ----

void WindowIntegration::quickLook() {
    const QString cur = m_window.currentPath();
    if (cur.isEmpty()) {
        m_window.showMessage(tr("Es ist kein Bild geöffnet."));
        return;
    }
    QPointer<ViewerWindow> w(&m_window);
    const bool ok = macint::quickLookToggle(&m_window, cur, [w](int direction) {
        if (w)
            w->perform(direction > 0 ? Action::NextImage : Action::PreviousImage);
    });
    if (!ok)
        m_window.showMessage(tr("Quick Look ist nicht verfügbar."));
}

// ---- Wallpaper ----

void WindowIntegration::askWallpaper() {
    const QString cur = m_window.currentPath();
    if (const QString problem = wallpaper::checkImage(cur); !problem.isEmpty()) {
        m_window.showMessage(problem, 3000);
        return;
    }
    if (m_wallpaperBox)
        return; // already asking
    auto* box =
        new QMessageBox(QMessageBox::Question, tr("Als Schreibtischhintergrund festlegen"),
                        tr("„%1“ als Schreibtischhintergrund festlegen?").arg(vpath::memberFileName(cur)),
                        QMessageBox::NoButton, &m_window);
    box->setObjectName(QStringLiteral("lfWallpaperQuestion"));
    auto* all = new QCheckBox(tr("Auf allen Bildschirmen"), box);
    all->setObjectName(QStringLiteral("lfWallpaperAllScreens"));
    all->setChecked(Settings::instance().value(key::WallpaperAllScreens).toBool());
    box->setCheckBox(all);
    QPushButton* yes = box->addButton(tr("Festlegen"), QMessageBox::AcceptRole);
    yes->setObjectName(QStringLiteral("lfWallpaperConfirm"));
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(yes);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    connect(box, &QMessageBox::buttonClicked, this, [this, yes, all](QAbstractButton* b) {
        if (b != yes)
            return;
        Settings::instance().setValue(key::WallpaperAllScreens, all->isChecked());
        setWallpaper(all->isChecked());
    });
    m_wallpaperBox = box;
    box->open();
}

void WindowIntegration::setWallpaper(bool allScreens) {
    const QString cur = m_window.currentPath();
    auto service = macint::createWallpaperService(&m_window);
    const wallpaper::Result r = wallpaper::apply(
        *service, cur, allScreens ? wallpaper::Target::AllScreens : wallpaper::Target::WindowScreen);
    if (r.ok())
        m_window.showMessage(tr("Als Schreibtischhintergrund festgelegt"));
    else
        m_window.showMessage(tr("Der Schreibtischhintergrund konnte nicht geändert werden: %1")
                                 .arg(r.errors.join(QStringLiteral("; "))),
                             5000);
}

bool WindowIntegration::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window.view() && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        m_overlay->updateGeometry();
    return QObject::eventFilter(watched, event);
}

QString WindowIntegration::statusText() const {
    return m_session->statusText();
}

} // namespace lf
