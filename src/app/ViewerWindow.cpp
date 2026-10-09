// SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewerWindow.h"

#include "Application.h"
#include "ImageView.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

constexpr int kChromeHideMs = 1500; // SPEC 5.1: hide after 1.5 s idle in fullscreen
constexpr int kChromeEdgePx = 48;   // mouse this close to the bottom/top edge reveals it
constexpr int kResizeRequestMs = 200;
constexpr int kGeometrySaveMs = 400;
constexpr qint64 kMaxClipboardFileBytes = qint64(64) << 20;
const char* const kGeometryKey = "window/geometry";

struct ZoomModeName {
    ZoomMode mode;
    const char* id;
};
const ZoomModeName kZoomModes[] = {
    {ZoomMode::Fit, "fit"},
    {ZoomMode::FitUpscale, "fitUpscale"},
    {ZoomMode::Actual, "actual"},
    {ZoomMode::FillWidth, "fillWidth"},
    {ZoomMode::FillHeight, "fillHeight"},
    {ZoomMode::Fill, "fill"},
};

int longEdge(const QImage& img) {
    return std::max(img.width(), img.height());
}

QString fileName(const QString& path) {
    return QFileInfo(path).fileName();
}

SortKey sortKeyFromId(const QString& id) {
    static const QHash<QString, SortKey> map = {
        {"name", SortKey::Name},
        {"dateModified", SortKey::DateModified},
        {"dateCreated", SortKey::DateCreated},
        {"dateAdded", SortKey::DateAdded},
        {"dateCaptured", SortKey::DateCaptured},
        {"size", SortKey::Size},
        {"kind", SortKey::Kind},
        {"pixelCount", SortKey::PixelCount},
        {"rating", SortKey::Rating},
    };
    return map.value(id, SortKey::Name);
}

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta || key == Qt::Key_Alt ||
           key == Qt::Key_CapsLock || key == Qt::Key_unknown || key == 0;
}

} // namespace

QString zoomModeId(ZoomMode mode) {
    for (const auto& z : kZoomModes)
        if (z.mode == mode)
            return QString::fromLatin1(z.id);
    return QStringLiteral("fit");
}

ZoomMode zoomModeFromId(const QString& id) {
    for (const auto& z : kZoomModes)
        if (id == QLatin1String(z.id))
            return z.mode;
    return ZoomMode::Fit;
}

ViewerWindow::ViewerWindow(Application& app, QWidget* parent) : QMainWindow(parent), m_app(app) {
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true); // F-OPEN-01: drop files/folders onto the window
    setWindowTitle(QStringLiteral("Lumaflip"));

    Settings& st = Settings::instance();
    m_collection = new Collection(this);
    if (st.value(key::FollowFinder).toBool()) {
        m_collection->setFollowFinder(true, st.value(key::FinderLiveQuery).toBool());
    } else {
        m_collection->setSortSpec(SortSpec{sortKeyFromId(st.value(key::ManualSortKey).toString()),
                                           st.value(key::ManualSortAscending).toBool(), SortSource::Manual});
    }

    // ---- central area: access hint bar above the image ----
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* hint = new QFrame(central);
    hint->setObjectName(QStringLiteral("lfAccessHint"));
    hint->setAttribute(Qt::WA_StyledBackground);
    hint->setStyleSheet(QStringLiteral("#lfAccessHint { background: rgba(255, 190, 0, 46); "
                                       "border-bottom: 1px solid rgba(128, 128, 128, 90); }"));
    auto* hl = new QHBoxLayout(hint);
    hl->setContentsMargins(12, 6, 8, 6);
    m_accessText = new QLabel(hint);
    m_accessText->setObjectName(QStringLiteral("lfAccessText"));
    m_accessText->setWordWrap(true);
    auto* allow = new QPushButton(tr("Zugriff erlauben…"), hint);
    allow->setObjectName(QStringLiteral("lfAllowAccess"));
    allow->setFocusPolicy(Qt::NoFocus);
    auto* dismiss = new QPushButton(tr("Ausblenden"), hint);
    dismiss->setObjectName(QStringLiteral("lfDismissAccess"));
    dismiss->setFocusPolicy(Qt::NoFocus);
    hl->addWidget(m_accessText, 1);
    hl->addWidget(allow);
    hl->addWidget(dismiss);
    hint->hide();
    m_accessHint = hint;
    connect(allow, &QPushButton::clicked, this, [this] { m_app.openUrl(Application::privacySettingsUrl()); });
    connect(dismiss, &QPushButton::clicked, this, &ViewerWindow::hideAccessHint);

    m_view = new ImageView(central);
    layout->addWidget(hint);
    layout->addWidget(m_view, 1);
    setCentralWidget(central);
    setFocusProxy(m_view);
    m_view->setFocus();

    // ---- overlays on the image: start screen (drop area) and transient messages ----
    m_emptyState = new QWidget(m_view);
    m_emptyState->setObjectName(QStringLiteral("lfEmptyState"));
    auto* el = new QVBoxLayout(m_emptyState);
    el->addStretch();
    m_emptyText = new QLabel(m_emptyState);
    m_emptyText->setObjectName(QStringLiteral("lfEmptyText"));
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setStyleSheet(QStringLiteral("color: rgba(160, 160, 160, 230); font-size: 18px;"));
    auto* open = new QPushButton(tr("Öffnen…"), m_emptyState);
    open->setObjectName(QStringLiteral("lfOpenButton"));
    open->setFocusPolicy(Qt::NoFocus);
    el->addWidget(m_emptyText);
    el->addSpacing(12);
    el->addWidget(open, 0, Qt::AlignHCenter);
    el->addStretch();
    connect(open, &QPushButton::clicked, &m_app, &Application::showOpenDialog);

    m_toast = new QLabel(m_view);
    m_toast->setObjectName(QStringLiteral("lfToast"));
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setStyleSheet(QStringLiteral("#lfToast { background: rgba(20, 20, 20, 200); color: #f2f2f2; "
                                          "border-radius: 8px; padding: 6px 14px; }"));
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toast->hide();
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);
    m_view->installEventFilter(this);
    // Mouse moves (without buttons) reveal the chrome in fullscreen (F-VIEW-10).
    for (QWidget* child : m_view->findChildren<QWidget*>()) {
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
    m_view->setMouseTracking(true);

    // ---- status bar (F-INFO-01, F-NAV-07, F-SORT-05) ----
    auto* sb = statusBar();
    sb->setSizeGripEnabled(false);
    auto makeLabel = [sb](const char* name, bool permanent = false) {
        auto* l = new QLabel(sb);
        l->setObjectName(QString::fromLatin1(name));
        l->setContentsMargins(6, 0, 6, 0);
        if (permanent)
            sb->addPermanentWidget(l);
        else
            sb->addWidget(l);
        return l;
    };
    m_nameLabel = makeLabel("lfStatusName");
    m_positionLabel = makeLabel("lfStatusPosition");
    m_pixelLabel = makeLabel("lfStatusPixels");
    m_bytesLabel = makeLabel("lfStatusBytes");
    m_zoomLabel = makeLabel("lfStatusZoom");
    m_sortLabel = makeLabel("lfStatusSort", true);
    sb->setVisible(st.value(key::ShowStatusBar).toBool());

    m_chromeTimer = new QTimer(this);
    m_chromeTimer->setSingleShot(true);
    m_chromeTimer->setInterval(kChromeHideMs);
    connect(m_chromeTimer, &QTimer::timeout, this, [this] {
        if (!isFullScreen())
            return;
        if (statusBar()->underMouse()) {
            m_chromeTimer->start();
            return;
        }
        setChromeVisible(false);
    });
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(kResizeRequestMs);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        const QString cur = currentPath();
        if (cur.isEmpty() || m_shownFailed)
            return;
        if (auto img = m_app.provider().request(cur, Quality::Screen, decodeTarget(), this))
            display(cur, Quality::Screen, img);
        updatePrefetch();
    });
    m_geometryTimer = new QTimer(this);
    m_geometryTimer->setSingleShot(true);
    m_geometryTimer->setInterval(kGeometrySaveMs);
    connect(m_geometryTimer, &QTimer::timeout, this, &ViewerWindow::saveGeometryNow);

    // ---- view settings ----
    m_view->setBackground(st.value(key::Background).toString());
    m_view->setUpscaleNearest(st.value(key::UpscaleFilter).toString() == QLatin1String("nearest"));
    m_view->setActualSizeInDevicePixels(st.value(key::ActualSizeUnit).toString() != QLatin1String("points"));
    m_view->setZoomMode(zoomModeFromId(st.value(key::ZoomMode).toString()));

    // ---- wiring ----
    connect(m_collection, &Collection::currentChanged, this, &ViewerWindow::onCurrentChanged);
    connect(m_collection, &Collection::entriesChanged, this, &ViewerWindow::onEntriesChanged);
    connect(m_collection, &Collection::loadingFinished, this, [this] {
        updateTitle();
        updateStatus();
        updateEmptyState();
        updatePrefetch();
    });
    connect(m_collection, &Collection::sortSpecChanged, this, [this] {
        updateStatus();
        m_app.windowChanged(this);
    });
    connect(m_collection, &Collection::accessDenied, this, &ViewerWindow::showAccessHint);
    ImageProvider& provider = m_app.provider();
    connect(&provider, &ImageProvider::imageReady, this, &ViewerWindow::onImageReady);
    connect(&provider, &ImageProvider::imageFailed, this, &ViewerWindow::onImageFailed);
    connect(m_view, &ImageView::userInput, this, &ViewerWindow::handleInput);
    connect(m_view, &ImageView::zoomChanged, this, [this] { updateStatus(); });
    connect(m_view, &ImageView::needsHigherResolution, this, [this] {
        const QString cur = currentPath();
        if (cur.isEmpty() || m_shownFailed || cur != m_shownPath)
            return;
        if (auto img = m_app.provider().request(cur, Quality::Full, {}, this))
            display(cur, Quality::Full, img);
    });
    connect(m_view, &ImageView::errorActionTriggered, this, [this] { perform(Action::RevealInFinder); });
    connect(&st, &Settings::changed, this, &ViewerWindow::applySetting);

    m_app.registerWindow(this);
    updateTitle();
    updateStatus();
    updateEmptyState();
}

ViewerWindow::~ViewerWindow() {
    m_app.provider().removeClient(this);
    if (!m_watchedFolder.isEmpty())
        m_app.setWindowFolder(this, m_watchedFolder, {});
    m_app.unregisterWindow(this);
}

// ---------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------

void ViewerWindow::openPaths(const QStringList& paths) {
    QStringList files;
    QString folder;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            if (folder.isEmpty())
                folder = fi.absoluteFilePath();
        } else if (fi.exists()) {
            files << fi.absoluteFilePath();
        }
    }
    if (files.size() == 1)
        openFile(files.first());
    else if (files.size() > 1)
        openFiles(files);
    else if (!folder.isEmpty())
        openFolder(folder);
}

void ViewerWindow::openFile(const QString& path) {
    hideAccessHint();
    m_opened = true;
    // F-WIN-09: a folder that another window already enumerated is not read again.
    for (ViewerWindow* other : m_app.windows()) {
        Collection* oc = other->collection();
        if (other == this || !oc->isFolderCollection() || oc->isLoading() || !oc->followsFinder() ||
            !m_collection->followsFinder() || oc->indexOf(path) < 0)
            continue;
        const QSignalBlocker block(m_collection);
        m_collection->copyFrom(*oc);
        m_collection->setCurrentPath(path);
        if (other->accessHint()->isVisibleTo(other))
            showAccessHint(oc->folderPath());
        onEntriesChanged();
        onCurrentChanged(m_collection->currentIndex(), m_collection->currentPath());
        return;
    }
    m_collection->openFile(path);
}

void ViewerWindow::openFiles(const QStringList& paths) {
    hideAccessHint();
    m_opened = true;
    m_collection->openFiles(paths);
}

void ViewerWindow::openFolder(const QString& folderPath) {
    hideAccessHint();
    m_opened = true;
    m_collection->openFolder(folderPath);
}

void ViewerWindow::copyStateFrom(const ViewerWindow& other) {
    m_opened = other.m_opened;
    m_view->setZoomMode(other.m_view->zoomMode() == ZoomMode::Custom ? ZoomMode::Fit
                                                                     : other.m_view->zoomMode());
    {
        const QSignalBlocker block(m_collection);
        m_collection->copyFrom(*other.m_collection);
    }
    if (other.m_accessHint->isVisibleTo(&other)) {
        m_accessText->setText(other.m_accessText->text());
        m_accessHint->show();
    }
    onEntriesChanged();
    onCurrentChanged(m_collection->currentIndex(), m_collection->currentPath());
    if (other.m_view->zoomMode() == ZoomMode::Custom && m_shownPath == other.m_shownPath)
        m_view->setZoomFactor(other.m_view->zoomFactor(),
                              QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
}

bool ViewerWindow::isEmpty() const {
    return !m_opened && m_collection->count() == 0;
}

QString ViewerWindow::currentPath() const {
    return m_collection->currentPath();
}

// ---------------------------------------------------------------------------------------
// Image loading (SPEC 7.1)
// ---------------------------------------------------------------------------------------

void ViewerWindow::onCurrentChanged(int, const QString& path) {
    updateTitle();
    updateStatus();
    if (path.isEmpty()) {
        if (m_view->image() || m_view->isShowingError())
            m_view->clear();
        m_shownPath.clear();
        m_shownFailed = false;
        m_wanted.clear();
        m_app.provider().removeClient(this);
        m_view->setPreloadImages({});
        updateEmptyState();
        return;
    }
    if (path != m_shownPath)
        showCurrent(path);
    updatePrefetch();
    updateEmptyState();
}

void ViewerWindow::onEntriesChanged() {
    updateTitle();
    updateStatus();
    updateEmptyState();
    updateFolderWatch();
}

void ViewerWindow::showCurrent(const QString& path) {
    ImageProvider& provider = m_app.provider();
    if (auto img = provider.request(path, Quality::Screen, decodeTarget(), this)) {
        display(path, Quality::Screen, img);
        return;
    }
    // A cached lower stage is shown at once (P-04); the better one follows via imageReady.
    if (auto img = provider.cached(path)) {
        display(path, img->isPreview ? Quality::Preview : Quality::Screen, img);
        return;
    }
    // Nothing yet: keep showing the previous image until the preview arrives, but
    // never an error card that belongs to another file.
    if (m_view->isShowingError()) {
        m_view->clear();
        m_shownPath.clear();
        m_shownFailed = false;
    }
}

bool ViewerWindow::display(const QString& path, Quality quality, const DecodedImagePtr& image) {
    if (!image || image->image.isNull())
        return false;
    const bool same = path == m_shownPath && !m_shownFailed;
    if (same) {
        const DecodedImagePtr cur = m_view->image();
        if (cur == image || int(quality) < int(m_shownQuality))
            return false;
        if (cur && quality == m_shownQuality && longEdge(image->image) <= longEdge(cur->image))
            return false;
    } else {
        // View-only rotation/flip belongs to one image (F-EDIT-01).
        m_view->setRotation(0);
        m_view->setFlip(false, false);
        m_flipH = m_flipV = false;
    }
    m_view->setImage(image, same);
    m_shownPath = path;
    m_shownQuality = quality;
    m_shownFailed = false;
    if (!same)
        ++m_displayCount;
    updateStatus();
    updateEmptyState();
    emit imageShown(path, quality);
    return true;
}

void ViewerWindow::onImageReady(const QString& path, Quality quality, const DecodedImagePtr& image) {
    if (path == currentPath()) {
        display(path, quality, image);
        return;
    }
    if (m_wanted.contains(path))
        refreshPreload();
}

void ViewerWindow::onImageFailed(const QString& path, const QString& error) {
    if (path != currentPath())
        return;
    if (path == m_shownPath && !m_shownFailed && m_view->image()) {
        // Only a higher stage failed; keep what is shown.
        showMessage(tr("Volle Auflösung nicht verfügbar: %1").arg(error), 3000);
        return;
    }
    m_shownPath = path;
    m_shownFailed = true;
    QString detail = tr("Die Datei ist beschädigt oder ihr Format wird nicht unterstützt.");
    if (!error.isEmpty())
        detail += QLatin1Char('\n') + error;
    m_view->showError(fileName(path), detail, tr("Im Finder zeigen"));
    updateStatus();
}

void ViewerWindow::updatePrefetch() {
    const QString cur = m_closed ? QString() : currentPath();
    ImageProvider& provider = m_app.provider();
    if (cur.isEmpty()) {
        provider.removeClient(this);
        m_wanted.clear();
        refreshPreload();
        return;
    }
    Settings& st = Settings::instance();
    const int ahead = st.value(key::PrefetchAhead).toInt();
    const int behind = st.value(key::PrefetchBehind).toInt();
    const int n = m_collection->count();
    const int i = m_collection->currentIndex();
    const bool wrap = wrapMode() == WrapMode::Wrap;
    QStringList list{cur};
    auto add = [&](int idx) {
        if (wrap && n > 0)
            idx = ((idx % n) + n) % n;
        if (idx < 0 || idx >= n || idx == i)
            return;
        const QString& p = m_collection->at(idx).path;
        if (!list.contains(p))
            list << p;
    };
    if (i >= 0) {
        for (int k = 1; k <= ahead; ++k)
            add(i + m_direction * k);
        for (int k = 1; k <= behind; ++k)
            add(i - m_direction * k);
    }
    const bool active = (isActiveWindow() || m_app.frontWindow() == this) && !isMinimized();
    provider.setWanted(this, list, decodeTarget(), active);
    m_wanted = list;
    refreshPreload();
}

void ViewerWindow::refreshPreload() {
    QList<DecodedImagePtr> images;
    ImageProvider& provider = m_app.provider();
    for (int k = 1; k < m_wanted.size(); ++k)
        if (auto img = provider.cached(m_wanted[k]))
            images << img;
    m_view->setPreloadImages(images);
}

QSize ViewerWindow::decodeTarget() const {
    QSize s = m_view->isVisible() ? m_view->size() : size();
    s = s.expandedTo(QSize(800, 600));
    const qreal dpr = windowHandle() ? windowHandle()->devicePixelRatio() : devicePixelRatioF();
    return (QSizeF(s) * dpr).toSize();
}

// ---------------------------------------------------------------------------------------
// Title, status, overlays
// ---------------------------------------------------------------------------------------

void ViewerWindow::updateTitle() {
    const QString cur = currentPath();
    QString title;
    if (cur.isEmpty()) {
        title = QStringLiteral("Lumaflip");
        if (m_collection->isLoading())
            title = tr("Lumaflip – lädt …");
        else if (m_opened && !m_collection->folderPath().isEmpty())
            title = QDir(m_collection->folderPath()).dirName();
    } else {
        const int n = m_collection->count();
        if (m_collection->isLoading() && n <= 1)
            title = fileName(cur); // position follows once the folder is read (F-OPEN-03)
        else
            title = QStringLiteral("%1 — %2 / %3")
                        .arg(fileName(cur))
                        .arg(m_collection->currentIndex() + 1)
                        .arg(n);
    }
    setWindowFilePath(cur); // proxy icon in the title bar
    if (title != windowTitle()) {
        setWindowTitle(title);
        emit titleChanged();
        m_app.windowChanged(this);
    }
}

QString ViewerWindow::sortText() const {
    if (m_collection->count() == 0)
        return {};
    if (m_collection->isLoading())
        return tr("Sortierung wird ermittelt …");
    return describe(m_collection->sortSpec());
}

void ViewerWindow::updateStatus() {
    const QString cur = currentPath();
    const int i = m_collection->currentIndex();
    const int n = m_collection->count();
    m_nameLabel->setText(cur.isEmpty() ? QString() : fileName(cur));
    m_positionLabel->setText(i >= 0 && !(m_collection->isLoading() && n <= 1)
                                 ? QStringLiteral("%1 / %2").arg(i + 1).arg(n)
                                 : QString());
    const DecodedImagePtr img = m_view->image();
    const bool showsCurrent = !cur.isEmpty() && cur == m_shownPath && !m_shownFailed && img;
    if (showsCurrent) {
        const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
        m_pixelLabel->setText(QStringLiteral("%1 × %2").arg(full.width()).arg(full.height()));
        m_zoomLabel->setText(QStringLiteral("%1 %").arg(qRound(m_view->zoomFactor() * 100)));
    } else {
        m_pixelLabel->clear();
        m_zoomLabel->clear();
    }
    qint64 bytes = i >= 0 ? m_collection->at(i).size : -1;
    if (bytes < 0 && !cur.isEmpty() && cur == m_shownPath)
        bytes = QFileInfo(cur).size();
    m_bytesLabel->setText(bytes >= 0 ? QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat)
                                     : QString());
    m_sortLabel->setText(sortText());
    for (QLabel* l : {m_nameLabel, m_positionLabel, m_pixelLabel, m_bytesLabel, m_zoomLabel})
        l->setVisible(!l->text().isEmpty());
}

QString ViewerWindow::statusText() const {
    QStringList parts;
    for (QLabel* l : {m_nameLabel, m_positionLabel, m_pixelLabel, m_bytesLabel, m_zoomLabel, m_sortLabel})
        if (!l->text().isEmpty())
            parts << l->text();
    return parts.join(QStringLiteral(" · "));
}

void ViewerWindow::updateEmptyState() {
    const bool empty = m_collection->count() == 0 && !m_collection->isLoading();
    if (empty) {
        if (m_opened && !m_collection->folderPath().isEmpty())
            m_emptyText->setText(tr("Keine Bilder in „%1“").arg(QDir(m_collection->folderPath()).dirName()));
        else
            m_emptyText->setText(tr("Bild oder Ordner hierher ziehen"));
    }
    m_emptyState->setVisible(empty);
    if (empty)
        layoutOverlays();
}

void ViewerWindow::layoutOverlays() {
    m_emptyState->setGeometry(m_view->rect());
    if (!m_toast->isHidden()) {
        m_toast->adjustSize();
        const QSize s = m_toast->size();
        m_toast->move((m_view->width() - s.width()) / 2, std::max(8, m_view->height() - s.height() - 32));
    }
}

void ViewerWindow::showMessage(const QString& text, int ms) {
    m_toast->setText(text);
    m_toast->show();
    m_toast->raise();
    layoutOverlays();
    m_toastTimer->start(ms);
}

QString ViewerWindow::toastText() const {
    return m_toast->isHidden() ? QString() : m_toast->text();
}

void ViewerWindow::showEdgeHint(bool atEnd) {
    // F-NAV-03: short visual hint when the end of the collection stops flipping.
    showMessage(atEnd ? tr("Letztes Bild") : tr("Erstes Bild"), 900);
}

void ViewerWindow::showAccessHint(const QString& folder) {
    // F-PRIV-01 / F-PRIV-02: explain, offer the right System Settings pane, never ask
    // for Full Disk Access.
    m_accessText->setText(
        tr("Lumaflip darf den Ordner „%1“ nicht lesen und zeigt deshalb nur die geöffnete "
           "Datei. Erlaube den Zugriff unter Systemeinstellungen → Datenschutz & Sicherheit → "
           "Dateien und Ordner. Festplattenvollzugriff ist dafür nicht nötig.")
            .arg(QDir(folder).dirName()));
    m_accessHint->show();
}

void ViewerWindow::hideAccessHint() {
    m_accessHint->hide();
}

void ViewerWindow::updateFolderWatch() {
    const QString folder =
        m_collection->isFolderCollection() && !m_closed ? m_collection->folderPath() : QString();
    if (folder == m_watchedFolder)
        return;
    m_app.setWindowFolder(this, m_watchedFolder, folder);
    m_watchedFolder = folder;
}

// ---------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------

WrapMode ViewerWindow::wrapMode() const {
    return Settings::instance().value(key::WrapMode).toString() == QLatin1String("wrap") ? WrapMode::Wrap
                                                                                         : WrapMode::Stop;
}

void ViewerWindow::navigate(int delta) {
    if (m_collection->count() == 0)
        return;
    m_direction = delta > 0 ? 1 : -1;
    if (!m_collection->jump(delta, wrapMode()))
        showEdgeHint(delta > 0);
}

void ViewerWindow::handleInput(InputEvent ev, QPointF pos) {
    if (pos.x() >= 0)
        ev.inLeftHalf = pos.x() < m_view->width() / 2.0;
    const InputBindings& b = m_app.bindings();
    const Action a = b.resolve(ev);
    if (a == Action::None)
        return;
    if (ev.kind == InputKind::Wheel) {
        const int steps = std::max(1, std::abs(ev.wheelSteps));
        switch (a) {
        case Action::NextImage:
        case Action::PreviousImage: {
            // F-IN-03: notches per image and a minimum interval between image changes.
            const int dir = a == Action::NextImage ? 1 : -1;
            if (m_wheelAccum * dir < 0)
                m_wheelAccum = 0;
            m_wheelAccum += dir * steps;
            if (std::abs(m_wheelAccum) < b.wheelStepsPerImage())
                return;
            m_wheelAccum = 0;
            if (m_lastWheelFlip.isValid() && m_lastWheelFlip.elapsed() < b.wheelMinIntervalMs())
                return;
            m_lastWheelFlip.start();
            perform(a, pos);
            return;
        }
        case Action::ZoomIn:
        case Action::ZoomOut: {
            const double f = std::pow(1.0 + b.zoomStepPercent() / 100.0, steps);
            m_view->zoomBy(a == Action::ZoomIn ? f : 1.0 / f, pos.x() >= 0 ? pos : m_view->rect().center());
            return;
        }
        default:
            for (int k = 0; k < steps; ++k)
                perform(a, pos);
            return;
        }
    }
    perform(a, pos);
}

bool ViewerWindow::perform(Action a, QPointF pos) {
    const QPointF anchor = pos.x() >= 0 ? pos : QPointF(m_view->width() / 2.0, m_view->height() / 2.0);
    const double step = std::max(16.0, m_view->height() * 0.1);
    const double page = std::max(32.0, m_view->height() * 0.9);
    const QString cur = currentPath();
    switch (a) {
    case Action::NextImage:
        navigate(1);
        return true;
    case Action::PreviousImage:
        navigate(-1);
        return true;
    case Action::JumpForward:
        navigate(m_app.bindings().jumpSize());
        return true;
    case Action::JumpBackward:
        navigate(-m_app.bindings().jumpSize());
        return true;
    case Action::FirstImage:
        m_direction = 1;
        if (!m_collection->first() && m_collection->count() > 0)
            showEdgeHint(false);
        return true;
    case Action::LastImage:
        m_direction = -1;
        if (!m_collection->last() && m_collection->count() > 0)
            showEdgeHint(true);
        return true;
    case Action::ZoomIn:
        m_view->zoomStep(1, anchor);
        return true;
    case Action::ZoomOut:
        m_view->zoomStep(-1, anchor);
        return true;
    case Action::ZoomActual:
        setZoomMode(ZoomMode::Actual);
        return true;
    case Action::ZoomFit:
        setZoomMode(ZoomMode::Fit);
        return true;
    case Action::ZoomFillWidth:
        setZoomMode(ZoomMode::FillWidth);
        return true;
    case Action::ToggleFitActualAtCursor:
        m_view->toggleFitActual(anchor);
        return true;
    case Action::ToggleZoomLock:
        m_view->setZoomLocked(!m_view->zoomLocked());
        showMessage(m_view->zoomLocked() ? tr("Zoom fixiert") : tr("Zoom gelöst"));
        m_app.windowChanged(this);
        return true;
    case Action::ScrollUp:
        m_view->panBy(QPointF(0, step));
        return true;
    case Action::ScrollDown:
        m_view->panBy(QPointF(0, -step));
        return true;
    case Action::ScrollLeft:
        m_view->panBy(QPointF(step, 0));
        return true;
    case Action::ScrollRight:
        m_view->panBy(QPointF(-step, 0));
        return true;
    case Action::ScrollPageUp:
        m_view->panBy(QPointF(0, page));
        return true;
    case Action::ScrollPageDown:
        m_view->panBy(QPointF(0, -page));
        return true;
    case Action::ScrollHome:
        m_view->scrollToTop();
        return true;
    case Action::ScrollEnd:
        m_view->scrollToBottom();
        return true;
    case Action::ToggleFullScreen:
        toggleFullScreen();
        return true;
    case Action::ExitModeOrNothing:
        if (!isFullScreen())
            return false;
        toggleFullScreen();
        return true;
    case Action::ExitModeOrCloseWindow:
        if (isFullScreen())
            toggleFullScreen();
        else
            close();
        return true;
    case Action::CloseWindow:
        close();
        return true;
    case Action::CloseAllWindows:
        m_app.closeAllWindows();
        return true;
    case Action::RotateLeft:
        m_view->setRotation((m_view->rotation() + 3) % 4);
        return true;
    case Action::RotateRight:
        m_view->setRotation((m_view->rotation() + 1) % 4);
        return true;
    case Action::FlipHorizontal:
        m_flipH = !m_flipH;
        m_view->setFlip(m_flipH, m_flipV);
        return true;
    case Action::FlipVertical:
        m_flipV = !m_flipV;
        m_view->setFlip(m_flipH, m_flipV);
        return true;
    case Action::Trash:
        trashCurrent();
        return true;
    case Action::RevealInFinder:
        if (cur.isEmpty())
            return false;
        m_app.shell().revealInFileManager(cur);
        return true;
    case Action::CopyImage:
        copyImage();
        return true;
    case Action::CopyPath:
        copyPath();
        return true;
    case Action::Undo:
        m_app.fileOps().undoStack()->undo();
        return true;
    case Action::Redo:
        m_app.fileOps().undoStack()->redo();
        return true;
    case Action::NewWindow:
        m_app.newWindow();
        return true;
    case Action::DuplicateWindow:
        m_app.duplicateWindow(this);
        return true;
    case Action::OpenPreferences:
        m_app.showPreferences();
        return true;
    case Action::ShowShortcuts:
        m_app.showShortcuts();
        return true;
    default:
        return false; // later stages (info panel, culling, slideshow …)
    }
}

void ViewerWindow::setZoomMode(ZoomMode mode) {
    m_view->setZoomMode(mode);
    if (mode != ZoomMode::Custom)
        Settings::instance().setValue(key::ZoomMode, zoomModeId(mode)); // F-MAC-05
    m_app.windowChanged(this);
}

void ViewerWindow::setSortFollowFinder() {
    m_collection->setFollowFinder(true, Settings::instance().value(key::FinderLiveQuery).toBool());
}

void ViewerWindow::setSortKey(SortKey k) {
    SortSpec s = m_collection->sortSpec();
    const bool ascending = s.key == SortKey::FinderIconPosition ? true : s.ascending;
    m_collection->setSortSpec(SortSpec{k, ascending, SortSource::Manual});
}

void ViewerWindow::setSortAscending(bool ascending) {
    SortSpec s = m_collection->sortSpec();
    const SortKey k = s.key == SortKey::FinderIconPosition ? SortKey::Name : s.key;
    m_collection->setSortSpec(SortSpec{k, ascending, SortSource::Manual});
}

void ViewerWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
        setChromeVisible(true);
    } else {
        showFullScreen();
        setChromeVisible(false); // F-VIEW-10: pure image, chrome appears at the edge
    }
    m_app.windowChanged(this);
}

void ViewerWindow::setChromeVisible(bool visible) {
    statusBar()->setVisible(visible && Settings::instance().value(key::ShowStatusBar).toBool());
}

bool ViewerWindow::chromeVisible() const {
    return !statusBar()->isHidden();
}

void ViewerWindow::trashCurrent() {
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    QString error;
    m_app.noteTrashedBy(path, this);
    // F-FILE-01: no confirmation (undo is possible); FileOps notifies every window.
    if (!m_app.fileOps().moveToTrash(path, &error))
        showMessage(tr("„%1“ konnte nicht in den Papierkorb gelegt werden: %2").arg(fileName(path), error),
                    4000);
}

void ViewerWindow::copyImage() {
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    // F-FILE-03: image data + file URL (+ the original file bytes for apps that take them).
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    QImage image;
    if (auto full = m_app.provider().cached(path, Quality::Full))
        image = full->image;
    else if (m_view->image() && path == m_shownPath)
        image = m_view->image()->image;
    if (!image.isNull())
        mime->setImageData(image);
    const QFileInfo fi(path);
    if (fi.size() > 0 && fi.size() <= kMaxClipboardFileBytes) {
        const QString type = QMimeDatabase().mimeTypeForFile(fi).name();
        QFile f(path);
        if (type.startsWith(QLatin1String("image/")) && f.open(QIODevice::ReadOnly))
            mime->setData(type, f.readAll());
    }
    QGuiApplication::clipboard()->setMimeData(mime);
    showMessage(tr("Bild kopiert"));
}

void ViewerWindow::copyPath() {
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(path);
    showMessage(tr("Pfad kopiert"));
}

// ---------------------------------------------------------------------------------------
// Notifications from the Application
// ---------------------------------------------------------------------------------------

void ViewerWindow::fileRemoved(const QString& path) {
    if (m_collection->indexOf(path) < 0)
        return;
    if (path == currentPath())
        m_direction = 1; // F-FILE-01: continue with the next image
    m_collection->removePath(path);
}

void ViewerWindow::fileAdded(const QString& path, bool makeCurrent) {
    m_collection->insertPath(path);
    if (makeCurrent)
        m_collection->setCurrentPath(path);
}

void ViewerWindow::folderChanged(const QString& folderPath) {
    if (m_collection->isFolderCollection() && m_collection->folderPath() == folderPath)
        m_collection->refresh(); // F-OPEN-07
}

void ViewerWindow::refreshActivity() {
    if (!currentPath().isEmpty())
        updatePrefetch();
}

void ViewerWindow::applySetting(const QString& k) {
    Settings& st = Settings::instance();
    if (k == QLatin1String(key::Background)) {
        m_view->setBackground(st.value(key::Background).toString());
    } else if (k == QLatin1String(key::UpscaleFilter)) {
        m_view->setUpscaleNearest(st.value(key::UpscaleFilter).toString() == QLatin1String("nearest"));
    } else if (k == QLatin1String(key::ActualSizeUnit)) {
        m_view->setActualSizeInDevicePixels(st.value(key::ActualSizeUnit).toString() !=
                                            QLatin1String("points"));
    } else if (k == QLatin1String(key::ShowStatusBar)) {
        if (!isFullScreen())
            setChromeVisible(true);
    } else if (k == QLatin1String(key::PrefetchAhead) || k == QLatin1String(key::PrefetchBehind) ||
               k == QLatin1String(key::WrapMode)) {
        updatePrefetch();
    } else if (k == QLatin1String(key::FollowFinder) || k == QLatin1String(key::FinderLiveQuery) ||
               k == QLatin1String(key::ManualSortKey) || k == QLatin1String(key::ManualSortAscending)) {
        if (st.value(key::FollowFinder).toBool())
            m_collection->setFollowFinder(true, st.value(key::FinderLiveQuery).toBool());
        else
            m_collection->setSortSpec(SortSpec{sortKeyFromId(st.value(key::ManualSortKey).toString()),
                                               st.value(key::ManualSortAscending).toBool(),
                                               SortSource::Manual});
    }
}

// ---------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------

void ViewerWindow::closeEvent(QCloseEvent* event) {
    saveGeometryNow();
    QMainWindow::closeEvent(event);
    if (!event->isAccepted())
        return;
    // Leave the registry now (the widget is deleted later): menus, open behavior and
    // file operations must not see a closed window any more.
    m_app.provider().removeClient(this);
    if (!m_watchedFolder.isEmpty())
        m_app.setWindowFolder(this, m_watchedFolder, {});
    m_watchedFolder.clear();
    m_closed = true;
    m_app.unregisterWindow(this);
}

void ViewerWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (isActiveWindow())
            m_app.windowActivated(this);
        if (!currentPath().isEmpty())
            updatePrefetch();
    } else if (event->type() == QEvent::WindowStateChange) {
        setChromeVisible(!isFullScreen());
        if (!currentPath().isEmpty())
            updatePrefetch(); // minimized windows keep only the current image (F-WIN-09)
        m_app.windowChanged(this);
    }
}

void ViewerWindow::dragEnterEvent(QDragEnterEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() &&
        std::all_of(urls.cbegin(), urls.cend(), [](const QUrl& u) { return u.isLocalFile(); }))
        event->acceptProposedAction();
}

void ViewerWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& u : event->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    if (paths.isEmpty())
        return;
    event->acceptProposedAction();
    openPaths(paths); // F-OPEN-01: drop onto the window shows it here
    activateWindow();
}

void ViewerWindow::keyPressEvent(QKeyEvent* event) {
    // Keys that reach the window (focus not on the view) are handled the same way.
    if (isModifierKey(event->key())) {
        QMainWindow::keyPressEvent(event);
        return;
    }
    InputEvent ev;
    ev.kind = InputKind::Key;
    ev.key = event->key();
    ev.modifiers = event->modifiers();
    ev.zoomed = m_view->isZoomed();
    handleInput(ev, QPointF(-1, -1));
    event->accept();
}

void ViewerWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    scheduleGeometrySave();
}

void ViewerWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    scheduleGeometrySave();
    if (!currentPath().isEmpty())
        m_resizeTimer->start();
}

bool ViewerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::Resize)
        layoutOverlays();
    if (event->type() == QEvent::MouseMove && isFullScreen()) {
        const auto* me = static_cast<QMouseEvent*>(event);
        const QPoint p = mapFromGlobal(me->globalPosition().toPoint());
        if (p.y() >= height() - kChromeEdgePx || p.y() <= kChromeEdgePx / 4) {
            setChromeVisible(true);
            m_chromeTimer->start();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void ViewerWindow::scheduleGeometrySave() {
    if (isVisible())
        m_geometryTimer->start();
}

void ViewerWindow::saveGeometryNow() {
    m_geometryTimer->stop();
    if (!isVisible() || isFullScreen() || isMinimized())
        return;
    Settings::instance().setValue(QString::fromLatin1(kGeometryKey), saveGeometry()); // F-MAC-05
}

} // namespace lf
