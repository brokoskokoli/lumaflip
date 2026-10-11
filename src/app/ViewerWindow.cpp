// SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewerWindow.h"

#include "AnimationPlayer.h"
#include "Application.h"
#include "CullingController.h"
#include "DisplayText.h"
#include "DragOut.h"
#include "HdrController.h"
#include "ImageView.h"
#include "OpenWith.h"
#include "QuickMoveController.h"
#include "RecentItems.h"
#include "RenameController.h"
#include "StartScreenRecent.h"
#include "WindowEditing.h"
#include "WindowExtras.h"
#include "WindowIntegration.h"
#include "WindowModes.h"
#include "WindowPanels.h"
#include "WindowTabbing.h"
#include "core/Collection.h"
#include "core/Decoder.h"
#include "core/FileBatch.h"
#include "core/FileOps.h"
#include "core/ImageFormats.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/StartupTrace.h"
#include "core/VirtualPath.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {
// F-VIEW-10: a native full screen animation starts within this after the request (else
// AppKit dropped it) and ends within kFsAnimationMs (it takes about 0.7 s).
constexpr int kFsStartMs = 800;
constexpr int kFsAnimationMs = 3000;
} // namespace

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

// F-OPEN-10: images inside an archive/PDF show as "Band 1.cbz › 012.jpg".
QString fileName(const QString& path) {
    return vpath::displayName(path);
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

QString sortKeyId(SortKey k) {
    static const QHash<SortKey, QString> map = {
        {SortKey::Name, "name"},
        {SortKey::DateModified, "dateModified"},
        {SortKey::DateCreated, "dateCreated"},
        {SortKey::DateAdded, "dateAdded"},
        {SortKey::DateCaptured, "dateCaptured"},
        {SortKey::Size, "size"},
        {SortKey::Kind, "kind"},
        {SortKey::PixelCount, "pixelCount"},
        {SortKey::Rating, "rating"},
    };
    return map.value(k, QStringLiteral("name"));
}

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta || key == Qt::Key_Alt ||
           key == Qt::Key_CapsLock || key == Qt::Key_unknown || key == 0;
}

// F-SORT-06: per-folder value (Settings::folderValue) "finder" or "<key>:asc|desc".
const char* const kFolderSortValue = "sort";

QString encodeFolderSort(const FolderSortChoice& c) {
    if (c.followFinder)
        return QStringLiteral("finder");
    return sortKeyId(c.manual.key) + (c.manual.ascending ? QStringLiteral(":asc") : QStringLiteral(":desc"));
}

std::optional<FolderSortChoice> decodeFolderSort(const QString& s) {
    if (s == QLatin1String("finder"))
        return FolderSortChoice{true, SortSpec{SortKey::Name, true, SortSource::Manual}};
    const qsizetype colon = s.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return std::nullopt;
    const QString dir = s.mid(colon + 1);
    if (dir != QLatin1String("asc") && dir != QLatin1String("desc"))
        return std::nullopt;
    return FolderSortChoice{
        false, SortSpec{sortKeyFromId(s.left(colon)), dir == QLatin1String("asc"), SortSource::Manual}};
}

} // namespace

std::optional<FolderSortChoice> ViewerWindow::rememberedFolderSort(const QString& folder) {
    // Worker threads (Collection jobs): Settings is thread-safe.
    return decodeFolderSort(
        Settings::instance().folderValue(folder, QString::fromLatin1(kFolderSortValue)).toString());
}

void ViewerWindow::applyDefaultSort() {
    // The global sort mode (F-SORT-03, F-SORT-02) for the next folder of this window.
    Settings& st = Settings::instance();
    m_collection->setDefaultSort(st.value(key::FollowFinder).toBool(),
                                 st.value(key::FinderLiveQuery).toBool(),
                                 SortSpec{sortKeyFromId(st.value(key::ManualSortKey).toString()),
                                          st.value(key::ManualSortAscending).toBool(), SortSource::Manual});
}

void ViewerWindow::rememberFolderSort(const FolderSortChoice& choice) {
    if (!m_collection->isFolderCollection() || m_collection->folderPath().isEmpty())
        return; // multi-file windows: for this window only
    Settings::instance().setFolderValue(m_collection->folderPath(), QString::fromLatin1(kFolderSortValue),
                                        encodeFolderSort(choice));
}

void ViewerWindow::applySortToAllFolders() {
    // "Für alle Ordner verwenden" (F-SORT-06): this window's choice becomes the global one.
    const SortSpec s = m_collection->sortSpec();
    const bool finder = s.source != SortSource::Manual;
    if (m_collection->isFolderCollection() && !m_collection->folderPath().isEmpty())
        Settings::instance().setFolderValue(m_collection->folderPath(), QString::fromLatin1(kFolderSortValue),
                                            QVariant()); // follows the global choice again
    Settings& st = Settings::instance();
    if (!finder && s.key != SortKey::FinderIconPosition) {
        st.setValue(key::ManualSortKey, sortKeyId(s.key));
        st.setValue(key::ManualSortAscending, s.ascending);
    }
    st.setValue(key::FollowFinder, finder);
    showMessage(tr("Sortierung gilt jetzt für alle Ordner"));
}

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
    applyDefaultSort();
    // F-SORT-06: a choice made for a folder (Darstellung → Sortieren nach) comes back for it.
    m_collection->setFolderSortMemory(&rememberedFolderSort);

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
    auto* allow = new QPushButton(tr("Zugriff erlauben …"), hint);
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

    trace::once("ViewerWindow: before ImageView");
    m_view = new ImageView(central);
    trace::once("ViewerWindow: ImageView created");
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
    auto* open = new QPushButton(tr("Öffnen …"), m_emptyState);
    open->setObjectName(QStringLiteral("lfOpenButton"));
    open->setFocusPolicy(Qt::NoFocus);
    open->setCursor(Qt::PointingHandCursor);
    m_openButton = open;
    el->addWidget(m_emptyText);
    el->addSpacing(12);
    el->addWidget(open, 0, Qt::AlignHCenter);
    m_startRecent = new StartScreenRecent(m_emptyState); // F-OPEN-12, filled when shown
    el->addSpacing(18);
    el->addWidget(m_startRecent, 0, Qt::AlignHCenter);
    el->addStretch();
    connect(open, &QPushButton::clicked, &m_app, &Application::showOpenDialog);
    connect(m_startRecent, &StartScreenRecent::openRequested, this, &ViewerWindow::openRecent);

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

    trace::once("ViewerWindow: overlays created");
    // The status bar (text, font metrics) is created when the window is shown: the font
    // database loads in the background meanwhile (P-01, see setVisible()).

    m_chromeTimer = new QTimer(this);
    m_chromeTimer->setSingleShot(true);
    m_chromeTimer->setInterval(kChromeHideMs);
    connect(m_chromeTimer, &QTimer::timeout, this, [this] {
        if (!isFullScreen())
            return;
        if ((m_statusBar && m_statusBar->underMouse()) || m_panels->underMouse()) {
            m_chromeTimer->start();
            return;
        }
        setChromeVisible(false);
    });
    m_fsGuard = new QTimer(this);
    m_fsGuard->setSingleShot(true);
    connect(m_fsGuard, &QTimer::timeout, this, [this] {
        // No start (AppKit dropped the request, e.g. while another window's space animation
        // runs) or no end reported: go on from what the native window is.
        m_fsTransition = false;
        settleFullScreen(true);
    });
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(kResizeRequestMs);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        const QString cur = currentPath();
        if (cur.isEmpty() || m_shownFailed)
            return;
        if (auto img = m_app.provider().request(cur, Quality::Screen, decodeTarget(), this, 0, m_page))
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
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            &ViewerWindow::updateEmptyStyle);

    trace::once("ViewerWindow: view settings applied");
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
    connect(m_collection, &Collection::foldersChanged, this, &ViewerWindow::updateFolderWatch); // F-OPEN-08
    ImageProvider& provider = m_app.provider();
    connect(&provider, &ImageProvider::imageReady, this, &ViewerWindow::onImageReady);
    // F-FMT-04: an error card only for the page that failed (other pages may be fine).
    connect(&provider, &ImageProvider::pageFailed, this,
            [this](const QString& path, int page, const QString& error) {
                if (page == m_page)
                    onImageFailed(path, error);
            });
    connect(&provider, &ImageProvider::stageFailed, this, &ViewerWindow::onStageFailed);
    // The shown image is outdated (file changed, RAW setting changed): the provider decodes
    // the wanted stages again and they replace it (F-FMT-05).
    connect(&provider, &ImageProvider::invalidated, this, [this](const QString& path) {
        if (path == m_shownPath && !m_shownFailed)
            m_shownStale = true;
    });
    connect(m_view, &ImageView::userInput, this, &ViewerWindow::handleInput);
    connect(m_view, &ImageView::zoomChanged, this, [this] { updateStatus(); });
    connect(m_view, &ImageView::needsHigherResolution, this, [this] {
        const QString cur = currentPath();
        if (cur.isEmpty() || m_shownFailed || cur != m_shownPath)
            return;
        if (auto img = m_app.provider().request(cur, Quality::Full, {}, this, 0, m_page))
            display(cur, Quality::Full, img);
    });
    connect(m_view, &ImageView::errorActionTriggered, this, [this] { perform(Action::RevealInFinder); });
    // F-FILE-07 / F-IN-14: drag the image out of the window as a file.
    m_view->setDragOutByDefault(st.value(QStringLiteral("input/drag")).toString() ==
                                QLatin1String("dragOut"));
    connect(m_view, &ImageView::dragOutRequested, this, [this] {
        const QString cur = currentPath();
        // Whatever stage is shown (also Full): the GUI thread only samples a thumbnail, the
        // image data for other apps is scaled on a worker (DragOut.h).
        if (vpath::isMember(cur)) { // F-OPEN-10: no file of its own
            perform(Action::DragOut);
            return;
        }
        if (!cur.isEmpty() && cur == m_shownPath && !m_shownFailed && m_view->image())
            startDragOut(m_view, cur, m_view->image()->image);
    });
    connect(&st, &Settings::changed, this, &ViewerWindow::applySetting);
    m_panels = new WindowPanels(*this, provider); // M2: created hidden, shown with the window
    setupMedia(); // animation, pages, slideshow, saving the rotation (ViewerWindowMedia.cpp)
    m_culling = new CullingController(*this, m_app);     // M3: ratings, culling, quick move
    m_extras = new WindowExtras(*this, m_app);           // M3: grid, compare, loupe, window modes …
    m_hdr = new HdrController(*this);                    // M3: HDR toggle, badge, exposure
    m_integration = new WindowIntegration(*this, m_app); // M4: archives, Live Text, Quick Look …
    connect(this, &ViewerWindow::imageShown, m_integration,
            [this](const QString& path) { m_integration->imageShown(path); });
    m_editing = new WindowEditing(*this, m_app); // M4: crop, export, photo overlays
    connect(this, &ViewerWindow::imageShown, m_editing,
            [this](const QString& path) { m_editing->imageShown(path); });
    // F-OPEN-10: an archive/PDF that cannot be read or holds no images.
    connect(m_collection, &Collection::containerFailed, this,
            [this](const QString& container, const QString& error) {
                m_view->showError(tr("„%1“ kann nicht geöffnet werden").arg(QFileInfo(container).fileName()),
                                  error);
                m_shownPath.clear();
                m_shownFailed = true;
                updateTitle();
            });
    // F-OPEN-10: the archive/PDF was rewritten: what was decoded from the old one goes.
    connect(m_collection, &Collection::containerContentChanged, this, [this] {
        const QVector<FileEntry> entries = m_collection->entries();
        for (const FileEntry& e : entries)
            onFileContentChanged(e.path);
    });

    m_app.registerWindow(this);
    updateTitle();
    updateStatus();
    updateEmptyState();
    trace::once("first ViewerWindow constructed");
}

void ViewerWindow::createStatusBar() {
    // F-INFO-01, F-NAV-07, F-SORT-05
    if (m_statusBar)
        return;
    auto* sb = statusBar();
    m_statusBar = sb;
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
    m_frameLabel = makeLabel("lfStatusFrame"); // F-FMT-03/04
    m_pixelLabel = makeLabel("lfStatusPixels");
    m_bytesLabel = makeLabel("lfStatusBytes");
    m_zoomLabel = makeLabel("lfStatusZoom");
    m_sortLabel = makeLabel("lfStatusSort", true);
    m_culling->attachStatusBar(sb); // F-CULL-04 "Filter: ≥ 3 ★ — 12 / 412"
    m_extras->attachStatusBar(sb);  // M3: name filter, "Zufällig", "Gekoppelt"
    m_hdr->attachStatusBar(sb);     // F-COL-04 badge
    sb->setVisible(Settings::instance().value(key::ShowStatusBar).toBool());
    updateStatus();
    applyStatusBarVisibility();
    trace::once("ViewerWindow: status bar created");
}

void ViewerWindow::setVisible(bool visible) {
    // F-WIN-05 / F-WIN-04: tabbing mode and tiling behavior before the window appears (a
    // "preferred" window opens as a tab).
    if (visible && !isVisible())
        configureViewerWindow(this, Settings::instance().value(key::AllowTabs).toBool());
    QMainWindow::setVisible(visible);
    if (visible)
        trace::once("ViewerWindow: native window shown");
    mediaVisibilityChanged(); // F-FMT-03: no playback in hidden windows
    if (visible && m_extras)
        m_extras->windowStateChanged(); // F-VIEW-12/13 on the native window
    if (visible)                        // F-VIEW-10: also transitions the user starts (green button, gesture)
        watchFullScreenTransitions(
            this, [this](bool entering, bool finished) { onFullScreenTransition(entering, finished); });
    // After the native window and the GPU surface exist (show), before the event loop
    // presents the first frame: the first frame already has the status bar.
    if (visible) {
        createStatusBar();
        m_panels->restore(); // M2 panels remembered in the settings
        updateEmptyState();
        trace::once("ViewerWindow: panels restored");
    }
}

ViewerWindow::~ViewerWindow() {
    m_app.provider().removeClient(this);
    if (!m_watchedFolder.isEmpty())
        m_app.setWindowFolder(this, m_watchedFolder, {}, m_watchedTree, false);
    m_app.unregisterWindow(this);
}

// ---------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------

void ViewerWindow::beginOpen() {
    // A newer open request wins over a pending "next folder" lookup (switchFolder).
    ++m_folderSwitchToken;
    ++m_openToken;
}

void ViewerWindow::prepareOpen() {
    hideAccessHint();
    m_opened = true;
    m_fitToImagePending = Settings::instance().value(key::FitWindowToImage).toBool();
    applyDefaultSort(); // F-SORT-06: another folder's manual choice does not carry over
}

void ViewerWindow::openPaths(const QStringList& paths) {
    beginOpen();
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
    const quint64 token = m_openToken;
    Application::classifyPaths(paths, this, [self, token](const ClassifiedPaths& c) {
        if (!self || token != self->m_openToken)
            return; // gone, or something newer was opened meanwhile
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
    if (vpath::isContainerPath(path) || vpath::isMember(path)) {
        openContainer(vpath::containerOf(path),
                      vpath::isMember(path) ? vpath::split(path)->member : QString());
        return;
    }
    beginOpen();
    prepareOpen();
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
        // The copy is shown at once; a check in the background picks up changes the
        // folder watcher cannot report (network volumes without FSEvents) - for every
        // window showing the folder.
        m_app.revalidateFolder(oc->folderPath());
        return;
    }
    m_collection->openFile(path);
}

void ViewerWindow::openContainer(const QString& containerPath, const QString& member) {
    beginOpen();
    prepareOpen();
    m_collection->openContainer(containerPath, member); // F-OPEN-10
}

void ViewerWindow::openFiles(const QStringList& paths, bool keepAsFileSet) {
    // F-OPEN-10: archives/PDFs cannot be part of a file set. Without other files the first
    // opens here like a folder; every other one gets a window of its own (never dropped).
    QStringList files;
    QStringList containers;
    for (const QString& p : paths)
        (vpath::isContainerPath(p) ? containers : files) << p;
    if (files.isEmpty() && !containers.isEmpty()) {
        openFile(containers.takeFirst());
        m_app.openContainersInOwnWindows(containers);
        return;
    }
    beginOpen();
    prepareOpen();
    m_collection->openFiles(files, keepAsFileSet);
    if (!containers.isEmpty())
        m_app.openContainersInOwnWindows(containers);
}

void ViewerWindow::openFolder(const QString& folderPath, bool selectLast) {
    beginOpen();
    prepareOpen();
    m_collection->openFolder(folderPath, selectLast);
}

void ViewerWindow::copyStateFrom(const ViewerWindow& other) {
    beginOpen();
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
        m_view->setZoomFactor(other.m_view->zoomFactor(),
                              QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
    else
        m_view->setZoomMode(other.m_view->zoomMode());
    m_view->setViewCenter(other.m_view->viewCenter());
}

QVariantMap ViewerWindow::sessionState() const {
    QVariantMap m;
    const Collection* c = m_collection;
    if (c->isContainerCollection()) {
        m.insert(QStringLiteral("container"), c->containerPath()); // F-OPEN-10
    } else if (c->isFolderCollection()) {
        m.insert(QStringLiteral("folder"), c->folderPath());
    } else {
        QStringList files;
        for (const FileEntry& e : c->entries())
            files << e.path;
        m.insert(QStringLiteral("files"), files);
    }
    m.insert(QStringLiteral("current"), currentPath());
    if (c->includesSubfolders())
        m.insert(QStringLiteral("subfolders"), true); // F-OPEN-08 (per window and session)
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
    // F-OPEN-10: an archive or PDF, with its current image.
    if (const QString container = state.value(QStringLiteral("container")).toString(); !container.isEmpty()) {
        if (!existing.contains(container))
            return false;
        const auto member = vpath::split(current);
        openContainer(container, member && member->container == container ? member->member : QString());
        return true;
    }
    const bool subfolders = state.value(QStringLiteral("subfolders")).toBool(); // F-OPEN-08
    m_collection->setIncludeSubfolders(subfolders && !folder.isEmpty());
    if (!folder.isEmpty() && subfolders && existing.contains(folder) &&
        QFileInfo(current).absolutePath() != folder) {
        // The current image is in a subfolder: open the folder, then select it.
        openFolder(folder);
        auto* once = new QObject(this);
        connect(m_collection, &Collection::loadingFinished, once, [this, once, current] {
            m_collection->setCurrentPath(current);
            once->deleteLater();
        });
    } else if (!folder.isEmpty()) {
        if (existing.contains(current))
            openFile(current); // the folder, with this image current
        else if (existing.contains(folder))
            openFolder(folder);
        else
            return false;
    } else if (files.size() == 1) {
        openFiles(files, true); // still a multi-file window (one file left), not its folder
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
    mediaCurrentChanged(path);
    if (m_integration)
        m_integration->currentChanged(path);
    if (m_editing)
        m_editing->currentChanged(path);
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
    mediaEntriesChanged();
    updateTitle();
    updateStatus();
    updateEmptyState();
    updateFolderWatch();
}

void ViewerWindow::showCurrent(const QString& path) {
    ImageProvider& provider = m_app.provider();
    if (auto img = provider.request(path, Quality::Screen, decodeTarget(), this, 0, m_page)) {
        display(path, Quality::Screen, img);
        return;
    }
    // A cached lower stage is shown at once (P-04); the better one follows via imageReady.
    if (auto img = provider.cached(path, Quality::Preview, {}, m_page)) {
        display(path, img->quality, img);
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
    if (!image || image->image.isNull() || mediaIgnoresStage(path, image))
        return false;
    const bool same = path == m_shownPath && !m_shownFailed;
    if (same) {
        const DecodedImagePtr cur = m_view->image();
        // An outdated image (file rewritten) takes any fresh stage: a complete small preview may be
        // the only one the provider delivers (it satisfies the better stages).
        if (cur == image || (int(quality) < int(m_shownQuality) && !m_shownStale))
            return false;
        // An outdated image (invalidated, e.g. the RAW setting changed) is replaced by the
        // new decode of the same stage even if that is not larger.
        if (cur && quality == m_shownQuality && longEdge(image->image) <= longEdge(cur->image) &&
            !m_shownStale)
            return false;
    } else {
        // View-only rotation/flip belongs to one image (F-EDIT-01).
        m_view->setRotation(0);
        m_view->setFlip(false, false);
        m_flipH = m_flipV = false;
    }
    if (trace::enabled())
        trace::once(quality == Quality::Preview ? "display Preview" : "display Screen/Full");
    mediaBeforeDisplay(same); // slideshow crossfade
    m_view->setImage(image, same);
    m_shownPath = path;
    m_shownQuality = quality;
    m_shownFailed = false;
    m_shownStale = false;
    if (!same) {
        ++m_displayCount;
        if (m_fitToImagePending) {
            m_fitToImagePending = false;
            fitWindowToImage(image->fullSize.isEmpty() ? image->image.size() : image->fullSize);
        }
    }
    updateStatus();
    updateEmptyState();
    mediaDisplayed(path, image, same); // animation / pages
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
    createStatusBar(); // its height is needed now (only with this option)
    if (!m_statusBar->isHidden())
        chromeHeight += m_statusBar->sizeHint().height();
    if (!m_accessHint->isHidden())
        chromeHeight += m_accessHint->sizeHint().height();
#ifdef Q_OS_WIN
    if (const QWidget* bar = menuWidget(); bar && !bar->isHidden()) // the window's own menu bar
        chromeHeight += bar->sizeHint().height();
#endif
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
    trace::once("onImageReady (GUI thread)");
    if (path == currentPath()) {
        if (!image || image->page == m_page) // F-FMT-04: only the shown page
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
    if (m_statusBar && !m_statusBar->isHidden())
        m_statusBar->showMessage(text, 5000);
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
    // F-SLIDE-01: during a slideshow the prefetch follows the slideshow order.
    const QStringList slideshowNext = slideshowPrefetch(ahead);
    for (const QString& p : slideshowNext)
        if (!list.contains(p))
            list << p;
    if (i >= 0 && slideshowNext.isEmpty()) {
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
    const QString folderSuffix =
        !cur.isEmpty() && Settings::instance().value(key::TitleShowFolder).toBool()
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
    setWindowFilePath(vpath::containerOf(cur)); // proxy icon in the title bar (F-OPEN-10: the archive)
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
    if (!m_statusBar)
        return; // created when shown
    const QString cur = currentPath();
    const int i = m_collection->currentIndex();
    const int n = m_collection->count();
    m_nameLabel->setText(cur.isEmpty() ? QString() : fileName(cur));
    m_positionLabel->setText(i >= 0 && !(m_collection->isLoading() && n <= 1)
                                 ? QStringLiteral("%1 / %2").arg(i + 1).arg(n)
                                 : QString());
    m_frameLabel->setText(frameStatusText());
    const DecodedImagePtr img = m_view->image();
    const bool showsCurrent = !cur.isEmpty() && cur == m_shownPath && !m_shownFailed && img;
    if (showsCurrent && isVectorFile(cur)) {
        // SVG: the decoder renders at a working size; the image's own size is what the
        // info panel shows too (read once per file on a worker).
        m_pixelLabel->setText(vectorSizeText(cur));
        m_zoomLabel->setText(zoomText(m_view->zoomFactor()));
    } else if (showsCurrent) {
        const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
        m_pixelLabel->setText(QStringLiteral("%1 × %2").arg(full.width()).arg(full.height()));
        m_zoomLabel->setText(zoomText(m_view->zoomFactor()));
    } else {
        m_pixelLabel->clear();
        m_zoomLabel->clear();
    }
    // Known once the folder is read (no file system access on the GUI thread).
    const qint64 bytes = i >= 0 ? m_collection->at(i).size : -1;
    m_bytesLabel->setText(bytes >= 0 ? formatByteSize(bytes) : QString()); // UI language (F-I18N-04)
    m_sortLabel->setText(sortText());
    if (m_culling)
        m_culling->updateStatus();
    if (m_extras)
        m_extras->updateStatus();
    for (QLabel* l : {m_nameLabel, m_positionLabel, m_frameLabel, m_pixelLabel, m_bytesLabel, m_zoomLabel})
        l->setVisible(!l->text().isEmpty());
}

QString ViewerWindow::zoomText(double factor) {
    //: zoom factor in the status bar, e.g. "37 %" (use the language's percent style)
    return tr("%1 %").arg(qRound(factor * 100));
}

bool ViewerWindow::isVectorFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("svg") || suffix == QLatin1String("svgz");
}

QString ViewerWindow::vectorSizeText(const QString& path) {
    if (m_vectorSizePath == path)
        return m_vectorSize.isEmpty()
                   ? tr("Vektor")
                   : tr("%1 × %2 (Vektor)").arg(m_vectorSize.width()).arg(m_vectorSize.height());
    if (m_vectorSizeJob != path) {
        m_vectorSizeJob = path;
        auto* w = new QFutureWatcher<QSize>(this);
        connect(w, &QFutureWatcher<QSize>::finished, this, [this, w, path] {
            w->deleteLater();
            if (m_vectorSizeJob == path)
                m_vectorSizeJob.clear();
            m_vectorSizePath = path;
            m_vectorSize = w->result();
            updateStatus();
        });
        w->setFuture(QtConcurrent::run([path] {
            const auto info = DecoderRegistry::instance().info(path, nullptr);
            return info ? info->displaySize : QSize();
        }));
    }
    return tr("Vektor");
}

QString ViewerWindow::statusText() const {
    QStringList parts;
    if (!m_statusBar)
        return {};
    for (QLabel* l :
         {m_nameLabel, m_positionLabel, m_frameLabel, m_pixelLabel, m_bytesLabel, m_zoomLabel, m_sortLabel})
        if (!l->text().isEmpty())
            parts << l->text();
    if (const QString filter = m_culling->filterStatus(); !filter.isEmpty())
        parts << filter;              // F-CULL-04
    parts << m_extras->statusParts(); // M3: name filter, shuffle, coupling
    if (const QString live = m_integration->statusText(); !live.isEmpty())
        parts << live; // F-MAC-06
    if (const QString af = m_editing ? m_editing->statusText() : QString(); !af.isEmpty())
        parts << af; // F-INFO-07
    return parts.join(QStringLiteral(" · "));
}

void ViewerWindow::updateEmptyState() {
    const bool empty = m_collection->count() == 0 && !m_collection->isLoading();
    const QString filtered = m_extras ? m_extras->emptyStateText() : QString();
    if (empty && !filtered.isEmpty()) {
        m_emptyText->setText(filtered); // F-NAV-08: the name filter matches nothing
    } else if (empty) {
        if (m_opened && !m_collection->folderPath().isEmpty())
            m_emptyText->setText(tr("Keine Bilder in „%1“").arg(QDir(m_collection->folderPath()).dirName()));
        else
            m_emptyText->setText(tr("Bild oder Ordner hierher ziehen"));
    }
    // Shown with the window (setVisible): polishing the text overlay before would wait
    // for the font database, and a window opened with a file never needs it (P-01).
    m_emptyState->setVisible(empty && isVisible());
    if (empty && isVisible())
        m_startRecent->refresh();
    if (empty)
        layoutOverlays();
    applyStatusBarVisibility();
}

void ViewerWindow::updateEmptyStyle() {
    // The start screen lies on the image background (dark by default): text and the
    // "Öffnen …" button get colors with enough contrast to it.
    const bool dark = m_view->backgroundColor().lightnessF() < 0.5;
    m_startRecent->setDark(dark);
    m_emptyText->setStyleSheet(dark ? QStringLiteral("color: rgb(200, 200, 200); font-size: 18px;")
                                    : QStringLiteral("color: rgb(70, 70, 70); font-size: 18px;"));
    const QString text = dark ? QStringLiteral("#f2f2f2") : QStringLiteral("#1c1c1c");
    const QString fill =
        dark ? QStringLiteral("rgba(255, 255, 255, 36)") : QStringLiteral("rgba(0, 0, 0, 20)");
    const QString hover =
        dark ? QStringLiteral("rgba(255, 255, 255, 64)") : QStringLiteral("rgba(0, 0, 0, 40)");
    const QString border =
        dark ? QStringLiteral("rgba(255, 255, 255, 120)") : QStringLiteral("rgba(0, 0, 0, 90)");
    m_openButton->setStyleSheet(QStringLiteral("QPushButton#lfOpenButton { color: %1; background-color: %2; "
                                               "border: 1px solid %4; border-radius: 6px; padding: 6px 20px; "
                                               "font-size: 14px; }"
                                               "QPushButton#lfOpenButton:hover { background-color: %3; }"
                                               "QPushButton#lfOpenButton:pressed { background-color: %4; }")
                                    .arg(text, fill, hover, border));
}

void ViewerWindow::applyStatusBarVisibility() {
    // No status bar under the empty start screen (it would only be an empty strip).
    if (!m_statusBar)
        return; // created when shown
    const bool hasContent = m_collection->count() > 0 || m_collection->isLoading();
    m_statusBar->setVisible(m_chromeShown && hasContent &&
                            Settings::instance().value(key::ShowStatusBar).toBool());
}

void ViewerWindow::layoutOverlays() {
    // Before the first show the view has no final size yet (its resize lays the overlays
    // out); laying out the text overlays now would also wait for the fonts (P-01).
    if (!isVisible())
        return;

    m_emptyState->setGeometry(m_view->rect());
    if (!m_toast->isHidden()) {
        const QWidget* host = m_toast->parentWidget() ? m_toast->parentWidget() : m_view;
        m_toast->adjustSize();
        const QSize s = m_toast->size();
        m_toast->move((host->width() - s.width()) / 2, std::max(8, host->height() - s.height() - 32));
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
    emit messageShown(text);
}

void ViewerWindow::setOverlayHost(QWidget* host) {
    if (!host || m_toast->parentWidget() == host)
        return;
    const bool shown = !m_toast->isHidden();
    m_toast->setParent(host);
    m_toast->setVisible(shown);
    if (shown) {
        m_toast->raise();
        layoutOverlays();
    }
}

void ViewerWindow::refreshStatus() {
    updateStatus();
    updateEmptyState();
}

void ViewerWindow::focusContent() {
    if (m_extras)
        m_extras->focusContent();
    else
        m_view->setFocus();
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
    m_accessFolder = folder;
    if (privacy) {
        m_accessText->setText(
            tr("Lumaflip darf den Ordner „%1“ nicht lesen und zeigt deshalb nur die geöffnete "
               "Datei. Erlaube den Zugriff unter Systemeinstellungen → Datenschutz & Sicherheit → "
               "Dateien und Ordner. Festplattenvollzugriff ist dafür nicht nötig.")
                .arg(QDir(folder).dirName()));
    } else {
        m_accessText->setText(
            tr("Für den Ordner „%1“ fehlen dir die Leserechte, deshalb zeigt Lumaflip nur die "
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
    // F-OPEN-10: an archive/PDF is watched through its folder (Collection::folderContentsChanged).
    const QString container = m_collection->containerPath();
    const QString folder = m_closed                             ? QString()
                           : !container.isEmpty()               ? QFileInfo(container).absolutePath()
                           : m_collection->isFolderCollection() ? m_collection->folderPath()
                                                                : QString();
    const bool tree = !folder.isEmpty() && m_collection->includesSubfolders(); // F-OPEN-08
    if (folder == m_watchedFolder && tree == m_watchedTree)
        return;
    m_app.setWindowFolder(this, m_watchedFolder, folder, m_watchedTree, tree);
    m_watchedFolder = folder;
    m_watchedTree = tree;
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
        (m_collection->isFolderCollection() || !m_collection->containerPath().isEmpty()))
        switchFolder(m_direction); // F-NAV-03 / F-IN-16 "Zum nächsten Ordner"
    else
        showEdgeHint(delta > 0);
}

void ViewerWindow::switchFolder(int direction) {
    // F-OPEN-10: inside an archive or PDF the siblings are folders with images and other
    // archives/PDFs next to it.
    const QString container = m_collection->containerPath();
    const bool inContainer = !container.isEmpty();
    if (m_folderSwitchPending ||
        (!inContainer && (!m_collection->isFolderCollection() || m_collection->folderPath().isEmpty())))
        return;
    // Sibling folders are listed and checked for images on a worker thread.
    m_folderSwitchPending = true;
    const quint64 token = ++m_folderSwitchToken;
    const QString from = inContainer ? container : m_collection->folderPath();
    auto* w = new QFutureWatcher<QString>(this);
    connect(w, &QFutureWatcher<QString>::finished, this, [this, w, token, direction, inContainer] {
        const QString next = w->result();
        w->deleteLater();
        m_folderSwitchPending = false;
        if (token != m_folderSwitchToken)
            return;
        if (next.isEmpty()) {
            if (inContainer)
                showMessage(direction > 0 ? tr("Kein weiterer Ordner und kein weiteres Archiv")
                                          : tr("Kein vorheriger Ordner und kein vorheriges Archiv"),
                            1200);
            else
                showMessage(direction > 0 ? tr("Kein weiterer Ordner mit Bildern")
                                          : tr("Kein vorheriger Ordner mit Bildern"),
                            1200);
            return;
        }
        m_direction = direction;
        if (vpath::isContainerPath(next))
            openContainer(next, {});
        else
            openFolder(next, direction < 0); // backwards: continue with the last image
        showMessage(QFileInfo(next).fileName(), 1200);
    });
    w->setFuture(QtConcurrent::run([from, direction, inContainer] {
        auto collator = createNameCollator();
        return inContainer ? collection::siblingFolderOrContainer(from, direction, collator.get())
                           : collection::siblingImageFolder(from, direction, collator.get());
    }));
}

void ViewerWindow::handleInput(InputEvent ev, QPointF pos) {
    if (pos.x() >= 0)
        ev.inLeftHalf = pos.x() < m_view->width() / 2.0;
    ev.animated = ev.animated || m_anim->isActive(); // F-IN-06 (animation)
    ev.multiPage = ev.multiPage || m_pageCount > 1;  // F-IN-04 (pages)
    if (m_culling->interceptKey(ev))
        return; // F-FILE-08: target digit of a quick move/copy (M/C + 1–9)
    // 5.2: digits = stars (culling mode; M3: also in the grid and the compare mode)
    ev.inCullingMode = ev.inCullingMode || m_culling->isActive() || m_extras->ratingKeysActive();
    if (handleSlideshowInput(ev))
        return; // F-SLIDE-01: Space pauses, 1–9 set the interval
    const InputBindings& b = m_app.bindings();
    const Action a = m_integration->adjustInput(b.resolve(ev), ev); // F-OPEN-10: right to left
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
    if (m_app.intercepted(this, a))
        return true;
    if (m_integration && m_integration->perform(a))
        return true; // M4: container guard, Quick Look, Live Text, wallpaper, action log
    if (m_editing && m_editing->perform(a))
        return true; // M4: crop, export, photo overlays
    if (m_extras->perform(a, pos))
        return true; // M3: grid, compare, loupe, go to, shuffle, name filter, window modes
    const QPointF anchor = pos.x() >= 0 ? pos : QPointF(m_view->width() / 2.0, m_view->height() / 2.0);
    const double step = std::max(16.0, m_view->height() * 0.1);
    const double page = std::max(32.0, m_view->height() * 0.9);
    const QString cur = currentPath();
    if (performMedia(a))
        return true; // animation, pages, slideshow, saving the rotation
    if (m_culling->perform(a))
        return true; // M3: rating, culling mode, quick move (CullingController)
    if (m_hdr->perform(a))
        return true; // M3: HDR toggle, exposure (HdrController)
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
        if (!fullScreenTarget())
            return false;
        setFullScreen(false);
        return true;
    case Action::ExitModeOrCloseWindow:
        if (fullScreenTarget())
            setFullScreen(false);
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
    case Action::Rename: // F-FILE-04
        return RenameController::of(this, m_app)->start();
    case Action::OpenInEditor: { // F-FILE-05
        if (cur.isEmpty())
            return false;
        QPointer<ViewerWindow> self(this);
        OpenWithService::instance().openInEditor(cur, m_app.shell(), this, [self](const QString& message) {
            if (self)
                self->showMessage(message, 5000);
        });
        return true;
    }
    // ---- M2 panels (WindowPanels) ----
    case Action::ToggleInfoPanel:
    case Action::ToggleFilmstrip:
        if (a == Action::ToggleInfoPanel)
            m_panels->toggleInfoPanel(); // F-INFO-02
        else
            m_panels->toggleFilmstrip(); // F-GRID-01
        if (isFullScreen()) {
            setChromeVisible(true); // visible now, hides again with the chrome (F-VIEW-10)
            m_chromeTimer->start();
        }
        m_app.windowChanged(this);
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
    // F-SORT-06: remembered for this folder (before the re-sort, which reads it).
    rememberFolderSort(FolderSortChoice{true, SortSpec{SortKey::Name, true, SortSource::Manual}});
    if (m_collection->followsFinder())
        m_collection->reload(); // the folder had its own manual choice: sort again
    else
        m_collection->setFollowFinder(true, Settings::instance().value(key::FinderLiveQuery).toBool());
}

void ViewerWindow::setSortKey(SortKey k) {
    SortSpec s = m_collection->sortSpec();
    const bool ascending = s.key == SortKey::FinderIconPosition ? true : s.ascending;
    const SortSpec spec{k, ascending, SortSource::Manual};
    rememberFolderSort(FolderSortChoice{false, spec});
    m_collection->setSortSpec(spec);
}

void ViewerWindow::setSortAscending(bool ascending) {
    SortSpec s = m_collection->sortSpec();
    const SortKey k = s.key == SortKey::FinderIconPosition ? SortKey::Name : s.key;
    const SortSpec spec{k, ascending, SortSource::Manual};
    rememberFolderSort(FolderSortChoice{false, spec});
    m_collection->setSortSpec(spec);
}

void ViewerWindow::toggleFullScreen() {
    setFullScreen(!fullScreenTarget());
}

void ViewerWindow::setFullScreen(bool on) {
    m_fsTarget = on;
    if (m_fsTransition) {
        m_app.windowChanged(this); // menu check mark; applied when the animation ended
        return;
    }
    applyFullScreen();
}

void ViewerWindow::applyFullScreen() {
    if (!m_fsTarget)
        return;
    const bool on = *m_fsTarget;
    if (on == isFullScreen()) {
        settleFullScreen();
        return;
    }
    // macOS: until the native transition reports its end, further requests wait.
    if (isVisible() && watchFullScreenTransitions(this, [this](bool entering, bool finished) {
            onFullScreenTransition(entering, finished);
        })) {
        m_fsTransition = true;
        m_fsGuard->start(kFsStartMs); // the animation must start soon, else it was dropped
    }
    if (on) {
        showFullScreen();
        setChromeVisible(false); // F-VIEW-10: pure image, chrome appears at the edge
    } else {
        showNormal();
        setChromeVisible(true);
    }
    if (!m_fsTransition)
        settleFullScreen();
    m_app.windowChanged(this);
}

void ViewerWindow::onFullScreenTransition(bool, bool finished) {
    if (!finished) {
        m_fsTransition = true; // also one the user started (green button, gesture)
        m_fsGuard->start(kFsAnimationMs);
        return;
    }
    m_fsTransition = false;
    m_fsGuard->stop();
    // After Qt has handled the same notification (its window state follows the native one).
    QTimer::singleShot(0, this, [this] {
        if (m_extras)
            m_extras->windowStateChanged(); // F-VIEW-12/13: skipped during the animation
        settleFullScreen();
    });
}

void ViewerWindow::settleFullScreen(bool force) {
    if (m_fsTransition || !m_fsTarget)
        return;
    const NativeWindowState native = nativeWindowState(this);
    const bool actual = native.valid ? native.fullScreen : isFullScreen();
    if (isFullScreen() != actual) {
        if (!force) {
            // Qt has not caught up with the native window yet: its WindowStateChange
            // follows; if it never comes, the guard forces it.
            m_fsGuard->start(kFsStartMs);
            return;
        }
        // Qt's state says what was asked, the native window never got there (request
        // dropped): Qt's state back to the native one - no native change, it is there
        // already - so that the request below really reaches AppKit.
        setWindowState(actual ? (windowState() | Qt::WindowFullScreen)
                              : (windowState() & ~Qt::WindowFullScreen));
    }
    if (actual != *m_fsTarget) {
        applyFullScreen(); // requested during the animation (or dropped by AppKit): now
        return;
    }
    m_fsTarget.reset();
    setChromeVisible(!actual);
    m_app.windowChanged(this);
}

void ViewerWindow::setChromeVisible(bool visible) {
    m_chromeShown = visible;
#ifdef Q_OS_WIN
    // The window's own menu bar (Windows) goes with the chrome. It stays "visible"
    // with zero height: the menu shortcuts only work through a visible bar.
    if (QWidget* bar = menuWidget())
        bar->setMaximumHeight(visible ? QWIDGETSIZE_MAX : 0);
#endif
    applyStatusBarVisibility();
    if (m_panels)
        m_panels->setChromeVisible(visible); // M2 panels hide/return with the chrome
}

bool ViewerWindow::chromeVisible() const {
    return m_statusBar && !m_statusBar->isHidden();
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
    auto* box =
        new QMessageBox(QMessageBox::Question, tr("In den Papierkorb legen"),
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
    // F-FMT-06: a RAW+JPEG pair goes as a whole (one undo step, sidecars along).
    if (const QStringList both = m_collection->withPairs({path}); both.size() > 1 && m_culling) {
        QVector<TransferItem> items;
        for (const QString& p : both)
            items.append({p, QString(), ConflictChoice::Skip});
        m_culling->quickMove()->transfer(
            TransferKind::Trash, items, tr("In den Papierkorb legen"),
            [](int n) { return tr("%n Datei(en) in den Papierkorb gelegt", nullptr, n); });
        return;
    }
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
    DecodeRequest request;
    request.maxBytes = m_app.provider().maxDecodeBytes(); // P-08
    w->setFuture(QtConcurrent::run([path, full, request] {
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
            if (auto d = DecoderRegistry::instance().decode(path, request, &error))
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
    // F-OPEN-08: also a (new) subfolder of a collection that includes subfolders. Changes
    // are coalesced; a tree reads only the changed folders again (code review M3 #16).
    m_collection->folderContentsChanged(folderPath);
}

void ViewerWindow::refreshActivity() {
    if (!currentPath().isEmpty())
        updatePrefetch();
}

void ViewerWindow::openRecent(const QString& path) {
    // F-OPEN-12: an item may be gone (deleted, volume not mounted): checked off the GUI
    // thread; then a note here and the entry leaves the list instead of an error card.
    QPointer<ViewerWindow> self(this);
    Application::classifyPaths({path}, this, [self, path](const ClassifiedPaths& c) {
        if (!self)
            return;
        if (!c.missing.isEmpty()) {
            RecentItems::remove(path);
            self->showMessage(tr("„%1“ ist nicht mehr vorhanden.").arg(QFileInfo(path).fileName()), 5000);
            return;
        }
        self->m_app.openPaths({path});
    });
}

void ViewerWindow::applySetting(const QString& k) {
    Settings& st = Settings::instance();
    if (k == RecentItems::settingsKey()) {
        if (m_emptyState->isVisible())
            m_startRecent->refresh(); // F-OPEN-12
        return;
    }
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
    } else if (k == QLatin1String("input/drag")) {
        m_view->setDragOutByDefault(st.value(k).toString() == QLatin1String("dragOut")); // F-IN-14
    } else if (k == QLatin1String(key::AllowTabs)) {
        if (isVisible())
            configureViewerWindow(this, st.value(key::AllowTabs).toBool()); // F-WIN-05
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
        m_app.setWindowFolder(this, m_watchedFolder, {}, m_watchedTree, false);
    m_watchedFolder.clear();
    m_closed = true;
    m_app.unregisterWindow(this);
}

void ViewerWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (isActiveWindow())
            m_app.windowActivated(this);
        // F-FMT-03: also here, not only on WindowStateChange - macOS may report the end of
        // a minimization late or not at all, and a paused animation would never resume.
        mediaVisibilityChanged();
        if (!currentPath().isEmpty())
            updatePrefetch();
    } else if (event->type() == QEvent::WindowStateChange) {
        mediaVisibilityChanged(); // minimized: animation pauses
        if (m_extras && !m_fsTransition)
            m_extras->windowStateChanged(); // F-VIEW-12/13 again after full screen
        setChromeVisible(!isFullScreen());
        if (m_fsTarget && !m_fsTransition)
            QTimer::singleShot(0, this, [this] { settleFullScreen(); }); // F-VIEW-10
        if (!currentPath().isEmpty())
            updatePrefetch(); // minimized windows keep only the current image (F-WIN-09)
        m_app.windowChanged(this);
    } else if (event->type() == QEvent::LanguageChange) {
        retranslateUi(); // F-I18N-03
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
    if (event->type() == QEvent::ShortcutOverride && isSlideshowRunning()) {
        // F-SLIDE-01: during the slideshow 1–9 set the interval instead of the zoom shortcuts.
        const auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier && ke->key() >= Qt::Key_1 &&
            ke->key() <= Qt::Key_9) {
            event->accept();
            return true;
        }
    }

    if (watched == m_view && event->type() == QEvent::Resize)
        layoutOverlays();
    if (event->type() == QEvent::MouseMove && isFullScreen()) {
        const auto* me = static_cast<QMouseEvent*>(event);
        const QPoint p = mapFromGlobal(me->globalPosition().toPoint());
        if (p.y() >= height() - kChromeEdgePx || p.y() <= kChromeEdgePx / 4 ||
            m_panels->isRevealEdge(p, kChromeEdgePx)) {
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
    // P-01: the next launch decodes the opened image for this size before the window exists.
    Settings::instance().setValue(QString::fromLatin1(key::DecodeTargetHint), decodeTarget());
}

} // namespace lf
