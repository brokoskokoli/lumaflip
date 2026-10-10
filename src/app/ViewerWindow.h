// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One viewer window (SPEC 4.15, 5.1): owns a Collection and an ImageView, wires
// them to the shared ImageProvider (staged loading + prefetch, SPEC 7.1), maps
// input to actions (InputBindings, 4.14) and shows title/status (F-NAV-07,
// F-INFO-01, F-SORT-05), error card (F-FMT-02) and the access hint (F-PRIV-01).

#include "app/ZoomModel.h"
#include "core/Collection.h" // WrapMode
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/SortSpec.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointF>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <optional>

class QLabel;
class QPushButton;
class QStatusBar;
class QTimer;

namespace lf {

class AnimationPlayer;
class Application;
class Collection;
class CullingController; // M3: rating, culling mode, filter, quick move
class HdrController;     // M3: HDR toggle, badge, exposure (F-COL-04/05)
class ImageView;
class SlideshowController;
class WindowPanels; // M2 panels (info panel, filmstrip)

QString zoomModeId(ZoomMode mode);          // Settings key::ZoomMode values
ZoomMode zoomModeFromId(const QString& id); // unknown -> Fit

class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ViewerWindow(Application& app, QWidget* parent = nullptr);
    ~ViewerWindow() override;

    // Opening (F-OPEN-02..05). openPaths() classifies: one file -> its folder,
    // several files -> exactly those, one folder -> its contents.
    void openPaths(const QStringList& paths);
    void openFile(const QString& path);
    // `keepAsFileSet`: one file stays a multi-file collection (restored window, F-WIN-06).
    void openFiles(const QStringList& paths, bool keepAsFileSet = false);
    void openFolder(const QString& folderPath, bool selectLast = false);
    // F-WIN-03 ⌥⌘N: same collection (no re-enumeration), image, zoom and view rotation.
    void copyStateFrom(const ViewerWindow& other);
    // ⌥⌘N, after the window is shown: same zoom factor, rotation and visible part.
    void copyViewFrom(const ViewerWindow& other);
    // F-WIN-06: what is needed to restore this window at the next start.
    QVariantMap sessionState() const;
    // Restores a sessionState(); paths not in `existing` are skipped. False: nothing left.
    bool restoreSessionState(const QVariantMap& state, const QStringList& existing);

    bool isEmpty() const; // nothing opened (start screen)
    Collection* collection() const { return m_collection; }
    ImageView* view() const { return m_view; }
    WindowPanels* panels() const { return m_panels; } // F-INFO-02, F-GRID-01
    CullingController* culling() const { return m_culling; } // M3: F-CULL-01..06, F-FILE-08
    HdrController* hdr() const { return m_hdr; }             // M3: F-COL-04/05
    QString currentPath() const;
    QString shownPath() const { return m_shownPath; }
    int displayedImageCount() const { return m_displayCount; } // distinct images shown (benchmarks)

    // Executes an action (menu, keys, mouse). `pos` = cursor position in the view
    // (anchor for zoom), negative = center. Returns false if not applicable.
    bool perform(Action action, QPointF pos = QPointF(-1, -1));
    // Feeds an input event (from the view or tests) through the bindings (4.14),
    // including wheel steps/min interval (F-IN-03).
    void handleInput(InputEvent event, QPointF pos);

    void setZoomMode(ZoomMode mode); // remembered for new windows (F-MAC-05)
    void setSortFollowFinder();      // "Wie Finder" (F-SORT-03)
    void setSortKey(SortKey key);    // manual (F-SORT-02)
    void setSortAscending(bool ascending);
    // The three above are remembered for the shown folder (F-SORT-06); this makes the
    // window's current sort the default for all folders ("Für alle Ordner verwenden").
    void applySortToAllFolders();
    void toggleFullScreen();    // F-VIEW-10
    bool chromeVisible() const; // status bar shown (fullscreen auto-hide)

    QString statusText() const;                          // all status fields joined with " · " (F-INFO-01)
    QString sortText() const;                            // F-SORT-05
    QWidget* accessHint() const { return m_accessHint; } // F-PRIV-01
    bool accessHintIsPrivacy() const { return m_accessPrivacy; } // TCC (vs. POSIX permissions)
    QString toastText() const; // visible transient message (edge hint, errors), else empty
    void showMessage(const QString& text, int ms = 1500);
    void hideMessage();
    QSize decodeTarget() const; // device pixels for Quality::Screen

    // Called by the Application for file operations of any window (F-FILE-01).
    void fileRemoved(const QString& path);
    void fileAdded(const QString& path, bool makeCurrent);
    void fileRenamed(const QString& from, const QString& to);
    void folderChanged(const QString& folderPath);
    void applySetting(const QString& key);
    void refreshActivity(); // front/active state changed: re-prioritise prefetch (F-WIN-09)
    // Shows/hides the window; the status bar is created on the first show (P-01).
    void setVisible(bool visible) override;

    // ---- M2 media (ViewerWindowMedia.cpp): animation F-FMT-03, pages F-FMT-04,
    //      slideshow F-SLIDE-01/02, saving the rotation F-EDIT-02 ----
    AnimationPlayer* animation() const { return m_anim; }
    SlideshowController* slideshow() const { return m_slideshow; }
    bool isSlideshowRunning() const;
    // Fullscreen, interval/options from the settings; `seed` fixes the shuffle order (tests).
    void startSlideshow(std::optional<quint32> seed = std::nullopt);
    void stopSlideshow();
    int currentPage() const { return m_page; }
    int pageCount() const { return m_pageCount; }
    bool goToPage(int page); // F-FMT-04
    void setAnimationSpeed(double factor);
    // "Bild 3/12" (animation) or "Seite 2/5" (multi-page), else empty.
    QString frameStatusText() const;
    void saveRotation(); // F-EDIT-02 (asks before trimming or re-encoding)

signals:
    // An image (or a better stage of it) was handed to the view.
    void imageShown(const QString& path, lf::Quality quality);
    void titleChanged();
    // showMessage() was called (tests: a later message may replace the toast at once).
    void messageShown(const QString& text);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onCurrentChanged(int index, const QString& path);
    void onEntriesChanged();
    void onImageReady(const QString& path, Quality quality, const DecodedImagePtr& image);
    void onImageFailed(const QString& path, const QString& error);
    void showCurrent(const QString& path);
    bool display(const QString& path, Quality quality, const DecodedImagePtr& image);
    void updatePrefetch();
    void refreshPreload();
    void updateTitle();
    void updateStatus();
    void updateEmptyState();
    void updateFolderWatch();
    void navigate(int delta);
    void showEdgeHint(bool atEnd);
    void showAccessHint(const QString& folder, bool privacy);
    void hideAccessHint();
    void fitWindowToImage(QSize imagePixels);
    void scheduleGeometrySave();
    void saveGeometryNow();
    void setChromeVisible(bool visible);
    void layoutOverlays();
    void copyImage();
    void copyPath();
    void trashCurrent();
    void trashNow(const QString& path);
    void switchFolder(int direction); // F-NAV-03 "nextFolder", Action::NextFolder/PreviousFolder
    void applyStatusBarVisibility();
    void updateEmptyStyle(); // start screen readable on any background
    void onStageFailed(const QString& path, Quality quality, const QString& error);
    WrapMode wrapMode() const;
    void createStatusBar();
    void beginOpen();   // a new open request: older pending ones (folder switch) are dropped
    void prepareOpen(); // common part of openFile/openFiles/openFolder
    void applyDefaultSort();
    void rememberFolderSort(const FolderSortChoice& choice);
    static std::optional<FolderSortChoice> rememberedFolderSort(const QString& folder);

    Application& m_app;
    Collection* m_collection = nullptr;
    ImageView* m_view = nullptr;
    QWidget* m_accessHint = nullptr;
    QLabel* m_accessText = nullptr;
    QWidget* m_emptyState = nullptr;
    QLabel* m_emptyText = nullptr;
    QLabel* m_toast = nullptr;
    QTimer* m_toastTimer = nullptr;
    QTimer* m_chromeTimer = nullptr;
    QTimer* m_resizeTimer = nullptr;
    QTimer* m_geometryTimer = nullptr;
    QStatusBar* m_statusBar = nullptr; // created on the first show
    QLabel* m_nameLabel = nullptr;

    QLabel* m_positionLabel = nullptr;
    QLabel* m_pixelLabel = nullptr;
    QLabel* m_bytesLabel = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QLabel* m_sortLabel = nullptr;
    QLabel* m_frameLabel = nullptr; // "Bild 3/12" / "Seite 2/5"

    // ---- M2 media state (ViewerWindowMedia.cpp) ----
    void setupMedia();
    void mediaCurrentChanged(const QString& path);
    bool mediaIgnoresStage(const QString& path, const DecodedImagePtr& image) const;
    void mediaBeforeDisplay(bool sameFile);
    void mediaDisplayed(const QString& path, const DecodedImagePtr& image, bool sameFile);
    bool performMedia(Action action); // true: handled
    bool handleSlideshowInput(const InputEvent& event);
    void mediaVisibilityChanged();
    void mediaEntriesChanged();
    void onFileContentChanged(const QString& path);
    QStringList slideshowPrefetch(int count) const;
    void updateFrameLabel();
    AnimationPlayer* m_anim = nullptr;
    SlideshowController* m_slideshow = nullptr;
    int m_page = 0;
    int m_pageCount = 1;
    bool m_animResume = false;          // playing when the window was hidden/minimized
    bool m_slideshowFullScreen = false; // the slideshow entered fullscreen (leave it at the end)
    bool m_reloadResetsView = false;    // the file was rewritten (saved rotation): reset the view rotation
    bool m_rotationSaving = false;
    QString m_mediaPath; // file the page/animation state belongs to

    QString m_shownPath; // file the view currently shows (or failed for)
    Quality m_shownQuality = Quality::Preview;
    bool m_shownFailed = false;
    bool m_shownStale = false; // the provider invalidated the shown file (replace on the next stage)
    QStringList m_wanted;      // prefetch window, current first
    QString m_watchedFolder;
    int m_direction = 1; // last flip direction (prefetch, SPEC 7.1)
    int m_displayCount = 0;
    bool m_flipH = false;
    bool m_flipV = false;
    int m_wheelAccum = 0;
    QElapsedTimer m_lastWheelFlip;
    bool m_opened = false;
    bool m_closed = false;
    bool m_fitToImagePending = false;
    bool m_accessPrivacy = false;
    bool m_chromeShown = true; // false: hidden in fullscreen until the mouse reaches an edge
    quint64 m_copyToken = 0;   // latest ⌘C request
    quint64 m_folderSwitchToken = 0;
    quint64 m_openToken = 0; // latest open request (openPaths classifies asynchronously)
    bool m_folderSwitchPending = false;
    QPushButton* m_openButton = nullptr;
    // Vector images (SVG): status bar shows the intrinsic size, not the render size.
    static bool isVectorFile(const QString& path);
    static QString zoomText(double factor); // "37 %"
    QString vectorSizeText(const QString& path);
    QString m_vectorSizePath;
    QString m_vectorSizeJob;
    QSize m_vectorSize;
    class StartScreenRecent* m_startRecent = nullptr; // F-OPEN-12
    void openRecent(const QString& path);
    WindowPanels* m_panels = nullptr; // M2: info panel + filmstrip (WindowPanels.cpp)
    CullingController* m_culling = nullptr; // M3 (CullingController.cpp)
    HdrController* m_hdr = nullptr;         // M3 (HdrController.cpp)
    // F-I18N-03: texts of the window after a language switch (ViewerWindowI18n.cpp).
    void retranslateUi();
    QString m_accessFolder; // folder of the access hint (its text is rebuilt)
};

} // namespace lf
