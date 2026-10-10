// SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewerWindow.h"

#include "Application.h"
#include "ImageView.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageFormats.h"
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
#include <QFutureWatcher>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QStyleHints>
#include <QtConcurrent/QtConcurrentRun>
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
    auto* open = new QPushButton(tr("Öffnen…"), m_emptyState);
    open->setObjectName(QStringLiteral("lfOpenButton"));
    open->setFocusPolicy(Qt::NoFocus);
    open->setCursor(Qt::PointingHandCursor);
    m_openButton = open;
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
    updateEmptyStyle();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &ViewerWindow::updateEmptyStyle);

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
    connect(&provider, &ImageProvider::stageFailed, this, &ViewerWindow::onStageFailed);
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
    // Image files open at once; anything else is looked at on a worker thread first.
    QStringList images;
    for (const QString& p : paths)
        if (hasImageSuffix(QFileInfo(p).fileName()))
            images << QDir::cleanPath(QFileInfo(p).absoluteFilePath());
    if (!images.isEmpty() && images.size() == paths.size()) {
        if (images.size() == 1)
            openFile(images.first());
        else
            openFiles(images);
        return;
    }
    QPointer<ViewerWindow> self(this);
    Application::classifyPaths(paths, this, [self](const ClassifiedPaths& c) {
        if (!self)
            return;
        if (c.files.size() == 1)
            self->openFile(c.files.first());
        else if (c.files.size() > 1)
            self->openFiles(c.files);
        else if (!c.folders.isEmpty())
            self->openFolder(c.folders.first());
        if (!c.missing.isEmpty())
            self->m_app.showNotFound(c.missing, self);
    });
}

void ViewerWindow::openFile(const QString& path) {
    hideAccessHint();
    m_opened = true;
    m_fitToImagePending = Settings::instance().value(key::FitWindowToImage).toBool();
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
            showAccessHint(oc->folderPath(), other->accessHintIsPrivacy());
        onEntriesChanged();
        onCurrentChanged(m_collection->currentIndex(), m_collection->currentPath());
        return;
    }
    m_collection->openFile(path);
}

void ViewerWindow::openFiles(const QStringList& paths) {
    hideAccessHint();
    m_opened = true;
    m_fitToImagePending = Settings::instance().value(key::FitWindowToImage).toBool();
    m_collection->openFiles(paths);
}

void ViewerWindow::openFolder(const QString& folderPath, bool selectLast) {
    hideAccessHint();
    m_opened = true;
    m_fitToImagePending = Settings::instance().value(key::FitWindowToImage).toBool();
    m_collection->openFolder(folderPath, selectLast);
}

void ViewerWindow::copyStateFrom(const ViewerWindow& other) {
    m_opened = other.m_opened;
    m_view->setZoomMode(other.m_view->zoomMode() == ZoomMode::Custom ? ZoomMode::Fit
                                                                     : other.m_view->zoomMode());
    {
        const QSignalBlocker block(m_collection);
        m_collection->copyFrom(*other.m_collection);
    }
    if (other.m_accessHint->isVisibleTo(&other))
        showAccessHint(other.m_collection->folderPath(), other.m_accessPrivacy);
    onEntriesChanged();
    onCurrentChanged(m_collection->currentIndex(), m_collection->currentPath());
    if (other.m_view->zoomMode() == ZoomMode::Custom && m_shownPath == other.m_shownPath)
        m_view->setZoomFactor(other.m_view->zoomFactor(),
                              QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
}

void ViewerWindow::copyViewFrom(const ViewerWindow& other) {
    if (m_shownPath != other.m_shownPath || !m_view->image() || !other.m_view->image())
        return;
    m_view->setRotation(other.m_view->rotation());
    m_flipH = other.m_flipH;
    m_flipV = other.m_flipV;
    m_view->setFlip(m_flipH, m_flipV);
    if (other.m_view->zoomMode() == ZoomMode::Custom)
        m_view->setZoomFactor(other.m_view->zoomFactor(), QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
    else
        m_view->setZoomMode(other.m_view->zoomMode());
    m_view->setViewCenter(other.m_view->viewCenter());
}

QVariantMap ViewerWindow::sessionState() const {
    QVariantMap m;
    const Collection* c = m_collection;
    if (c->isFolderCollection()) {
        m.insert(QStringLiteral("folder"), c->folderPath());
    } else {
        QStringList files;
        for (const FileEntry& e : c->entries())
            files << e.path;
        m.insert(QStringLiteral("files"), files);
    }
    m.insert(QStringLiteral("current"), currentPath());
    const ZoomMode mode = m_view->zoomMode() == ZoomMode::Custom ? ZoomMode::Fit : m_view->zoomMode();
    m.insert(QStringLiteral("zoom"), zoomModeId(mode));
    m.insert(QStringLiteral("geometry"), saveGeometry());
    return m;
}

bool ViewerWindow::restoreSessionState(const QVariantMap& state, const QStringList& existing) {
    const QByteArray geometry = state.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    m_view->setZoomMode(zoomModeFromId(state.value(QStringLiteral("zoom")).toString()));
    const QString current = state.value(QStringLiteral("current")).toString();
    const QString folder = state.value(QStringLiteral("folder")).toString();
    QStringList files;
    for (const QString& f : state.value(QStringLiteral("files")).toStringList())
        if (existing.contains(f))
            files << f;
    if (!folder.isEmpty()) {
        if (existing.contains(current))
            openFile(current); // the folder, with this image current
        else if (existing.contains(folder))
            openFolder(folder);
        else
            return false;
    } else if (files.size() == 1) {
        openFiles(files);
    } else if (!files.isEmpty()) {
        openFiles(files);
        m_collection->setCurrentPath(current);
    } else {
        return false;
    }
    return true;
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
    if (!same) {
        ++m_displayCount;
        if (m_fitToImagePending) {
            m_fitToImagePending = false;
            fitWindowToImage(image->fullSize.isEmpty() ? image->image.size() : image->fullSize);
        }
    }
    updateStatus();
    updateEmptyState();
    emit imageShown(path, quality);
    return true;
}

void ViewerWindow::fitWindowToImage(QSize imagePixels) {
    // F-WIN-08 option "Fenstergröße an Bild anpassen": 100 % if it fits, else as large
    // as the screen allows (the zoom mode then fits the image).
    if (isFullScreen() || isMaximized() || imagePixels.isEmpty())
        return;
    QScreen* s = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!s)
        return;
    const QRect avail = s->availableGeometry();
    const qreal dpr = s->devicePixelRatio();
    // Chrome around the image (status bar, hint bar); computed from size hints so it
    // also works before the window is shown and laid out.
    int chromeHeight = 0;
    if (!statusBar()->isHidden())
        chromeHeight += statusBar()->sizeHint().height();
    if (!m_accessHint->isHidden())
        chromeHeight += m_accessHint->sizeHint().height();
    const QSize wanted = (QSizeF(imagePixels) / dpr).toSize() + QSize(0, chromeHeight);
    const QSize bounded = wanted.boundedTo(QSize(int(avail.width() * 0.9), int(avail.height() * 0.9)))
                              .expandedTo(QSize(360, 260));
    resize(bounded);
    QRect frame = frameGeometry();
    if (!avail.contains(frame)) {
        frame.moveRight(std::min(frame.right(), avail.right()));
        frame.moveBottom(std::min(frame.bottom(), avail.bottom()));
        frame.moveLeft(std::max(frame.left(), avail.left()));
        frame.moveTop(std::max(frame.top(), avail.top()));
        move(frame.topLeft());
    }
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
        onStageFailed(path, Quality::Full, error);
        return;
    }
    m_shownPath = path;
    m_shownFailed = true;
    // The decoder reason is already user facing (German); generic text only as fallback.
    const QString detail =
        error.isEmpty() ? tr("Die Datei ist beschädigt oder ihr Format wird nicht unterstützt.") : error;
    m_view->showError(fileName(path), detail, tr("Im Finder zeigen"));
    updateStatus();
}

void ViewerWindow::onStageFailed(const QString& path, Quality quality, const QString& error) {
    // A better stage (e.g. full resolution of a huge image) failed while a lower one is
    // shown: a note in the status bar, never an error card over a visible image.
    if (path != currentPath() || quality == Quality::Preview)
        return;
    qDebug("lf: stage %d of %s failed: %s", int(quality), qPrintable(path), qPrintable(error));
    const QString text = tr("Volle Auflösung nicht verfügbar");
    if (!statusBar()->isHidden())
        statusBar()->showMessage(text, 5000);
    else
        showMessage(text, 3000);
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
    // F-WIN-10 option: "IMG_1234.jpg — 23 / 412 — Ordner".
    const QString folderSuffix = !cur.isEmpty() && Settings::instance().value(key::TitleShowFolder).toBool()
                                     ? QStringLiteral(" — ") + QFileInfo(QFileInfo(cur).absolutePath()).fileName()
                                     : QString();
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
        title += folderSuffix;
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
    // Known once the folder is read (no file system access on the GUI thread).
    const qint64 bytes = i >= 0 ? m_collection->at(i).size : -1;
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
    applyStatusBarVisibility();
}

void ViewerWindow::updateEmptyStyle() {
    // The start screen lies on the image background (dark by default): text and the
    // "Öffnen…" button get colors with enough contrast to it.
    const bool dark = m_view->backgroundColor().lightnessF() < 0.5;
    m_emptyText->setStyleSheet(dark ? QStringLiteral("color: rgb(200, 200, 200); font-size: 18px;")
                                    : QStringLiteral("color: rgb(70, 70, 70); font-size: 18px;"));
    const QString text = dark ? QStringLiteral("#f2f2f2") : QStringLiteral("#1c1c1c");
    const QString fill = dark ? QStringLiteral("rgba(255, 255, 255, 36)") : QStringLiteral("rgba(0, 0, 0, 20)");
    const QString hover = dark ? QStringLiteral("rgba(255, 255, 255, 64)") : QStringLiteral("rgba(0, 0, 0, 40)");
    const QString border = dark ? QStringLiteral("rgba(255, 255, 255, 120)") : QStringLiteral("rgba(0, 0, 0, 90)");
    m_openButton->setStyleSheet(QStringLiteral("QPushButton#lfOpenButton { color: %1; background-color: %2; "
                                               "border: 1px solid %4; border-radius: 6px; padding: 6px 20px; "
                                               "font-size: 14px; }"
                                               "QPushButton#lfOpenButton:hover { background-color: %3; }"
                                               "QPushButton#lfOpenButton:pressed { background-color: %4; }")
                                    .arg(text, fill, hover, border));
}

void ViewerWindow::applyStatusBarVisibility() {
    // No status bar under the empty start screen (it would only be an empty strip).
    const bool hasContent = m_collection->count() > 0 || m_collection->isLoading();
    statusBar()->setVisible(m_chromeShown && hasContent && Settings::instance().value(key::ShowStatusBar).toBool());
}


void ViewerWindow::layoutOverlays() {
    m_emptyState->setGeometry(m_view->rect());
    if (!m_toast->isHidden()) {
        m_toast->adjustSize();
        const QSize s = m_toast->size();
        m_toast->move((m_view->width() - s.width()) / 2, std::max(8, m_view->height() - s.height() - 32));
    }
}

void ViewerWindow::hideMessage() {
    m_toastTimer->stop();
    m_toast->hide();
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

void ViewerWindow::showAccessHint(const QString& folder, bool privacy) {
    // F-PRIV-01 / F-PRIV-02: explain, offer the right System Settings pane, never ask
    // for Full Disk Access. Only a macOS privacy denial (TCC) is solved there; missing
    // POSIX permissions are changed in the Finder.
    m_accessPrivacy = privacy;
    if (privacy) {
        m_accessText->setText(tr("Lumaflip darf den Ordner „%1“ nicht lesen und zeigt deshalb nur die geöffnete "
                                 "Datei. Erlaube den Zugriff unter Systemeinstellungen → Datenschutz & Sicherheit → "
                                 "Dateien und Ordner. Festplattenvollzugriff ist dafür nicht nötig.")
                                  .arg(QDir(folder).dirName()));
    } else {
        m_accessText->setText(tr("Für den Ordner „%1“ fehlen dir die Leserechte, deshalb zeigt Lumaflip nur die "
                                 "geöffnete Datei. Die Zugriffsrechte lassen sich im Finder unter Ablage → "
                                 "Informationen → „Teilen & Zugriffsrechte“ ändern.")
                                  .arg(QDir(folder).dirName()));
    }
    if (auto* allow = m_accessHint->findChild<QPushButton*>(QStringLiteral("lfAllowAccess")))
        allow->setVisible(privacy);
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
    if (m_collection->jump(delta, wrapMode()))
        return;
    if (Settings::instance().value(key::WrapMode).toString() == QLatin1String("nextFolder") &&
        m_collection->isFolderCollection())
        switchFolder(m_direction); // F-NAV-03 / F-IN-16 "Zum nächsten Ordner"
    else
        showEdgeHint(delta > 0);
}

void ViewerWindow::switchFolder(int direction) {
    if (!m_collection->isFolderCollection() || m_collection->folderPath().isEmpty() || m_folderSwitchPending)
        return;
    // Sibling folders are listed and checked for images on a worker thread.
    m_folderSwitchPending = true;
    const quint64 token = ++m_folderSwitchToken;
    const QString folder = m_collection->folderPath();
    auto* w = new QFutureWatcher<QString>(this);
    connect(w, &QFutureWatcher<QString>::finished, this, [this, w, token, direction] {
        const QString next = w->result();
        w->deleteLater();
        m_folderSwitchPending = false;
        if (token != m_folderSwitchToken)
            return;
        if (next.isEmpty()) {
            showMessage(direction > 0 ? tr("Kein weiterer Ordner mit Bildern")
                                      : tr("Kein vorheriger Ordner mit Bildern"),
                        1200);
            return;
        }
        m_direction = direction;
        openFolder(next, direction < 0); // backwards: continue with the last image
        showMessage(QDir(next).dirName(), 1200);
    });
    w->setFuture(QtConcurrent::run([folder, direction] {
        auto collator = createNameCollator();
        return collection::siblingImageFolder(folder, direction, collator.get());
    }));
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
    case Action::NextFolder:
        switchFolder(1);
        return true;
    case Action::PreviousFolder:
        switchFolder(-1);
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
        m_app.fileOps().undo();
        return true;
    case Action::Redo:
        m_app.fileOps().redo();
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
    m_chromeShown = visible;
    applyStatusBarVisibility();
}

bool ViewerWindow::chromeVisible() const {
    return !statusBar()->isHidden();
}

void ViewerWindow::trashCurrent() {
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    // F-FILE-01: by default no confirmation (undo is possible); optional question.
    if (!Settings::instance().value(key::ConfirmTrash).toBool()) {
        trashNow(path);
        return;
    }
    if (findChild<QMessageBox*>(QStringLiteral("lfConfirmTrash")))
        return; // already asking
    auto* box = new QMessageBox(QMessageBox::Question, tr("In den Papierkorb legen"),
                                tr("„%1“ in den Papierkorb legen?").arg(fileName(path)), QMessageBox::NoButton, this);
    box->setObjectName(QStringLiteral("lfConfirmTrash"));
    box->setInformativeText(tr("Mit ⌘Z lässt sich das widerrufen."));
    QPushButton* yes = box->addButton(tr("In den Papierkorb legen"), QMessageBox::AcceptRole);
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(yes);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    connect(box, &QMessageBox::buttonClicked, this, [this, yes, path](QAbstractButton* b) {
        if (b == yes)
            trashNow(path);
    });
    box->open();
}

void ViewerWindow::trashNow(const QString& path) {
    // The file system work runs on a worker thread (network volumes); every window is
    // updated when it is done. This window is remembered with the undo command.
    QPointer<ViewerWindow> self(this);
    m_app.fileOps().moveToTrashAsync(path, this, [self](bool ok, const QString& error) {
        if (!ok && self)
            self->showMessage(error, 4000);
    });
}

namespace {
struct ClipboardData {
    QImage image;     // best available full image
    QString mimeType; // of the original file
    QByteArray bytes; // original file (image/* only, up to kMaxClipboardFileBytes)
};
} // namespace

void ViewerWindow::copyImage() {
    const QString path = currentPath();
    if (path.isEmpty())
        return;
    // F-FILE-03: file URL, the original bytes and the full resolution image. Reading and
    // decoding happen on a worker thread; the clipboard is filled when that is done.
    const DecodedImagePtr full = m_app.provider().cached(path, Quality::Full);
    const QImage shown = m_view->image() && path == m_shownPath ? m_view->image()->image : QImage();
    const quint64 token = ++m_copyToken;
    showMessage(tr("Bild wird kopiert …"), 10000);
    auto* w = new QFutureWatcher<ClipboardData>(this);
    connect(w, &QFutureWatcher<ClipboardData>::finished, this, [this, w, token, path, shown] {
        ClipboardData r = w->result();
        w->deleteLater();
        if (token != m_copyToken)
            return; // a newer ⌘C
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(path)});
        const bool screenOnly = r.image.isNull() && !shown.isNull();
        const QImage image = r.image.isNull() ? shown : r.image;
        if (!image.isNull())
            mime->setImageData(image);
        if (!r.bytes.isEmpty())
            mime->setData(r.mimeType, r.bytes);
        QGuiApplication::clipboard()->setMimeData(mime);
        showMessage(screenOnly ? tr("Bild kopiert (Bildschirmauflösung)") : tr("Bild kopiert"));
    });
    w->setFuture(QtConcurrent::run([path, full] {
        ClipboardData r;
        const QFileInfo fi(path);
        if (fi.size() > 0 && fi.size() <= kMaxClipboardFileBytes) {
            const QString type = QMimeDatabase().mimeTypeForFile(fi).name();
            QFile f(path);
            if (type.startsWith(QLatin1String("image/")) && f.open(QIODevice::ReadOnly)) {
                r.mimeType = type;
                r.bytes = f.readAll();
            }
        }
        if (full) {
            r.image = full->image;
        } else {
            QString error;
            if (auto d = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error))
                r.image = d->image;
        }
        // Float (HDR) buffers are converted for other apps.
        if (!r.image.isNull() && (r.image.format() == QImage::Format_RGBA16FPx4 ||
                                  r.image.format() == QImage::Format_RGBA16FPx4_Premultiplied ||
                                  r.image.format() == QImage::Format_RGBA32FPx4 ||
                                  r.image.format() == QImage::Format_RGBA32FPx4_Premultiplied))
            r.image = r.image.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_ARGB32);
        return r;
    }));
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
    // A multi-file window only takes back files it contained (F-OPEN-04); the window
    // that trashed a file shows it again after undo (`makeCurrent`).
    m_collection->insertPath(path, false, makeCurrent);
}

void ViewerWindow::fileRenamed(const QString& from, const QString& to) {
    m_collection->renamePath(from, to);
}

void ViewerWindow::folderChanged(const QString& folderPath) {
    // F-OPEN-07: the app invalidated the shared snapshot once; every window showing the
    // folder re-sorts the one new enumeration (F-WIN-09).
    if (m_collection->isFolderCollection() && m_collection->folderPath() == folderPath)
        m_collection->reload();
}

void ViewerWindow::refreshActivity() {
    if (!currentPath().isEmpty())
        updatePrefetch();
}

void ViewerWindow::applySetting(const QString& k) {
    Settings& st = Settings::instance();
    if (k == QLatin1String(key::Background)) {
        m_view->setBackground(st.value(key::Background).toString());
        updateEmptyStyle();
    } else if (k == QLatin1String(key::UpscaleFilter)) {
        m_view->setUpscaleNearest(st.value(key::UpscaleFilter).toString() == QLatin1String("nearest"));
    } else if (k == QLatin1String(key::ActualSizeUnit)) {
        m_view->setActualSizeInDevicePixels(st.value(key::ActualSizeUnit).toString() !=
                                            QLatin1String("points"));
    } else if (k == QLatin1String(key::ShowStatusBar)) {
        if (!isFullScreen())
            setChromeVisible(true);
    } else if (k == QLatin1String(key::TitleShowFolder)) {
        updateTitle();
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
