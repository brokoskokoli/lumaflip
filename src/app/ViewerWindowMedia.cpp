// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerWindow, M2 media part: animation playback (F-FMT-03), multi-page files (F-FMT-04),
// slideshow (F-SLIDE-01/02) and saving the view rotation (F-EDIT-02). Kept apart from
// ViewerWindow.cpp, which only calls the hooks below.

#include "AnimationPlayer.h"
#include "Application.h"
#include "CullingController.h"
#include "ImageView.h"
#include "SlideshowController.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Language.h"
#include "core/LosslessTransform.h"
#include "core/Settings.h"
#include "core/SlideshowInput.h"
#include "core/ThumbnailCache.h"
#include "core/UndoBackupStore.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace lf {

namespace {
constexpr int kCrossfadeMs = 300; // F-SLIDE-02: subtle
} // namespace

void ViewerWindow::setupMedia() {
    m_anim = new AnimationPlayer(this);
    connect(m_anim, &AnimationPlayer::frameChanged, this, [this](int, const DecodedImagePtr& frame) {
        if (!frame || m_anim->path() != m_shownPath || m_shownFailed)
            return;
        m_view->showAnimationFrame(frame);
        updateFrameLabel();
    });
    connect(m_anim, &AnimationPlayer::stateChanged, this, [this] {
        updateFrameLabel();
        m_app.windowChanged(this); // menu check marks
    });
    connect(m_anim, &AnimationPlayer::failed, this,
            [this](const QString& error) { showMessage(tr("Animation angehalten: %1").arg(error), 4000); });

    m_slideshow = new SlideshowController(this);
    connect(m_slideshow, &SlideshowController::showRequested, this, [this](const QString& path) {
        m_direction = 1;
        m_collection->setCurrentPath(path);
    });
    connect(m_slideshow, &SlideshowController::finished, this, [this] {
        stopSlideshow();
        showMessage(tr("Diashow beendet"));
    });
    connect(m_slideshow, &SlideshowController::stateChanged, this, [this] { m_app.windowChanged(this); });

    connect(&m_app.fileOps(), &FileOps::fileChanged, this, &ViewerWindow::onFileContentChanged);
}

// ---------------------------------------------------------------------------------------
// Hooks called by ViewerWindow.cpp
// ---------------------------------------------------------------------------------------

void ViewerWindow::mediaCurrentChanged(const QString& path) {
    if (m_anim->isActive() && m_anim->path() != path)
        m_anim->stop(); // another image: the animation stops (F-FMT-03)
    if (path != m_mediaPath) {
        m_mediaPath = path;
        m_page = 0;
        m_pageCount = 1;
    }
    if (isSlideshowRunning())
        m_slideshow->sync(path);
}

void ViewerWindow::mediaEntriesChanged() {
    if (!isSlideshowRunning())
        return;
    QStringList paths;
    for (const FileEntry& e : m_collection->entries())
        paths << e.path;
    m_slideshow->updatePaths(paths);
}

bool ViewerWindow::mediaIgnoresStage(const QString& path, const DecodedImagePtr& image) const {
    if (path == m_mediaPath && image->page != m_page)
        return true; // a stage of another page
    // A playing animation keeps its frames; better stages of frame 0 would interrupt it.
    return m_anim->isActive() && m_anim->path() == path && path == m_shownPath && !m_shownFailed;
}

void ViewerWindow::mediaBeforeDisplay(bool sameFile) {
    if (!sameFile && isSlideshowRunning() && m_view->image() &&
        Settings::instance().value(key::SlideshowCrossfade).toBool())
        m_view->beginCrossfade(kCrossfadeMs);
}

void ViewerWindow::mediaDisplayed(const QString& path, const DecodedImagePtr& image, bool sameFile) {
    m_mediaPath = path;
    m_pageCount = std::max(1, image->pageCount);
    if (sameFile && m_reloadResetsView) {
        // The saved rotation is in the file now (F-EDIT-02): the view shows it upright.
        m_reloadResetsView = false;
        m_view->setRotation(0);
        m_flipH = m_flipV = false;
        m_view->setFlip(false, false);
    }
    if (image->frameCount > 1 && image->quality != Quality::Preview &&
        (!m_anim->isActive() || m_anim->path() != path)) {
        const bool visible = isVisible() && !isMinimized();
        // P-08/P-09: frames within the provider's per-image cap and a share of its budget.
        m_anim->setMemoryBudget(m_app.provider().memoryBudget());
        m_anim->setMaxDecodeBytes(m_app.provider().maxDecodeBytes());
        m_anim->open(path, image, image->frameCount, visible);
        m_animResume = !visible;
    }
    updateFrameLabel();
}

void ViewerWindow::mediaVisibilityChanged() {
    if (!m_anim)
        return; // during construction
    const bool hidden = !isVisible() || isMinimized();
    if (hidden && m_anim->isPlaying()) {
        m_animResume = true;
        m_anim->pause();
    } else if (!hidden && m_animResume) {
        m_animResume = false;
        m_anim->play();
    }
}

bool ViewerWindow::performMedia(Action a) {
    switch (a) {
    case Action::ToggleAnimation:
        if (!m_anim->isActive())
            return false;
        m_anim->togglePlay();
        showMessage(m_anim->isPlaying() ? tr("Wiedergabe") : tr("Pause"), 800);
        return true;
    case Action::NextFrame:
    case Action::PreviousFrame:
        if (!m_anim->isActive())
            return false;
        m_anim->step(a == Action::NextFrame ? 1 : -1);
        return true;
    case Action::NextPage:
    case Action::PreviousPage:
        if (m_pageCount <= 1)
            return false;
        goToPage(m_page + (a == Action::NextPage ? 1 : -1));
        return true;
    case Action::ToggleSlideshow:
        isSlideshowRunning() ? stopSlideshow() : startSlideshow();
        return true;
    case Action::SaveRotation:
        saveRotation();
        return true;
    case Action::NextImage:
    case Action::PreviousImage:
        if (!isSlideshowRunning())
            return false;
        // ←/→ step in slideshow order (F-SLIDE-01) and restart the interval.
        if (!(a == Action::NextImage ? m_slideshow->next() : m_slideshow->previous()))
            showEdgeHint(a == Action::NextImage);
        return true;
    case Action::ExitModeOrNothing:
    case Action::ExitModeOrCloseWindow:
        if (!isSlideshowRunning())
            return false;
        stopSlideshow(); // Esc ends the slideshow
        return true;
    default:
        return false;
    }
}

bool ViewerWindow::handleSlideshowInput(const InputEvent& ev) {
    if (!isSlideshowRunning() || ev.kind != InputKind::Key)
        return false;
    const InputBindings& b = m_app.bindings();
    // F-SLIDE-03: rating stays possible (P/X/U always; the digits where rating keys are
    // active anyway, SPEC 5.2). Feedback: the rating toast or the culling overlay.
    const bool ratingKeys =
        m_culling->isActive() || b.choice(QStringLiteral("ratingKeys")) == QLatin1String("always");
    const slideshowinput::Result r = slideshowinput::dispatch(b, ev, ratingKeys);
    switch (r.kind) {
    case slideshowinput::Kind::TogglePause:
        m_slideshow->togglePause();
        showMessage(m_slideshow->isPaused() ? tr("Diashow angehalten") : tr("Diashow läuft weiter"), 900);
        return true;
    case slideshowinput::Kind::SetInterval:
        m_slideshow->setIntervalMs(r.seconds * 1000);
        Settings::instance().setValue(key::SlideshowIntervalSec, r.seconds);
        showMessage(tr("Intervall: %n Sekunde(n)", nullptr, r.seconds), 900);
        return true;
    case slideshowinput::Kind::Rate:
        m_culling->rate(r.rating);
        return true;
    case slideshowinput::Kind::None:
        break;
    }
    return false;
}

QStringList ViewerWindow::slideshowPrefetch(int count) const {
    return isSlideshowRunning() ? m_slideshow->upcoming(count) : QStringList();
}

void ViewerWindow::onFileContentChanged(const QString& path) {
    // F-EDIT-02: the file was rewritten (saved rotation, undo, redo): every cached stage
    // is outdated; windows showing it get the new decode.
    m_app.provider().invalidate(path);
    ThumbnailCache::instance().invalidate(path); // the key may not change (modification date kept)
    if (path == m_shownPath) {
        m_reloadResetsView = true;
        if (m_anim->isActive() && m_anim->path() == path)
            m_anim->stop();
    }
}

// ---------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------

QString ViewerWindow::frameStatusText() const {
    if (m_anim && m_anim->isActive() && m_anim->path() == currentPath()) {
        QString s = tr("Bild %1/%2").arg(m_anim->currentFrame() + 1).arg(m_anim->frameCount());
        if (std::abs(m_anim->speed() - 1.0) > 1e-6)
            s += QStringLiteral(" · %1×").arg(formatLocale().toString(m_anim->speed()));
        return s;
    }
    if (m_pageCount > 1 && m_mediaPath == currentPath())
        return tr("Seite %1/%2").arg(m_page + 1).arg(m_pageCount);
    return {};
}

void ViewerWindow::updateFrameLabel() {
    if (!m_frameLabel)
        return; // status bar not created yet
    const QString text = frameStatusText();
    if (m_frameLabel->text() != text)
        m_frameLabel->setText(text);
    m_frameLabel->setVisible(!text.isEmpty());
}

// ---------------------------------------------------------------------------------------
// Animation and pages
// ---------------------------------------------------------------------------------------

void ViewerWindow::setAnimationSpeed(double factor) {
    m_anim->setSpeed(factor);
    if (m_anim->isActive())
        showMessage(tr("Geschwindigkeit: %1×").arg(formatLocale().toString(m_anim->speed())), 900);
}

bool ViewerWindow::goToPage(int page) {
    const QString cur = currentPath();
    if (cur.isEmpty() || m_pageCount <= 1)
        return false;
    if (page < 0 || page >= m_pageCount) {
        showMessage(page < 0 ? tr("Erste Seite") : tr("Letzte Seite"), 900);
        return false;
    }
    if (page == m_page)
        return true;
    m_page = page;
    // The next page is shown like a new image (fitted, no rotation of the previous page).
    m_shownPath.clear();
    ImageProvider& provider = m_app.provider();
    if (auto img = provider.request(cur, Quality::Screen, decodeTarget(), this, 0, m_page))
        display(cur, Quality::Screen, img);
    else if (auto lower = provider.cached(cur, Quality::Preview, {}, m_page))
        display(cur, lower->quality, lower);
    updateStatus();
    return true;
}

// ---------------------------------------------------------------------------------------
// Slideshow
// ---------------------------------------------------------------------------------------

bool ViewerWindow::isSlideshowRunning() const {
    return m_slideshow && m_slideshow->isRunning();
}

void ViewerWindow::startSlideshow(std::optional<quint32> seed) {
    if (m_collection->count() == 0 || currentPath().isEmpty())
        return;
    Settings& st = Settings::instance();
    SlideshowController::Options o;
    o.intervalMs = st.value(key::SlideshowIntervalSec).toInt() * 1000;
    o.repeat = st.value(key::SlideshowRepeat).toBool();
    o.shuffle = st.value(key::SlideshowShuffle).toBool();
    o.seed = seed ? *seed : QRandomGenerator::global()->generate();
    QStringList paths;
    for (const FileEntry& e : m_collection->entries())
        paths << e.path;
    m_slideshowFullScreen = !isFullScreen();
    if (m_slideshowFullScreen)
        toggleFullScreen();
    m_slideshow->start(paths, currentPath(), o);
    showMessage(tr("Diashow – Leertaste: Pause · 1–9: Sekunden · Esc: Ende"), 2500);
    updatePrefetch();
}

void ViewerWindow::stopSlideshow() {
    // Also after the controller ended by itself (end without repeat): leave fullscreen.
    if (!isSlideshowRunning() && !m_slideshowFullScreen)
        return;
    m_slideshow->stop();
    if (m_slideshowFullScreen && isFullScreen())
        toggleFullScreen();
    m_slideshowFullScreen = false;
    updatePrefetch();
}

// ---------------------------------------------------------------------------------------
// Saving the rotation (F-EDIT-02)
// ---------------------------------------------------------------------------------------

void ViewerWindow::saveRotation() {
    const QString path = currentPath();
    if (path.isEmpty() || m_shownFailed || m_rotationSaving)
        return;
    Settings& st = Settings::instance();
    RotationRequest r;
    r.path = path;
    r.quarterTurnsCW = m_view->rotation();
    r.flipH = m_flipH;
    r.flipV = m_flipV;
    r.strategy = st.value(key::RotationSaveStrategy).toString() == QLatin1String("lossless")
                     ? RotationStrategy::LosslessJpeg
                     : RotationStrategy::ExifOrientation;
    r.keepModificationDate = st.value(key::KeepModificationDate).toBool();
    if (r.quarterTurnsCW == 0 && !r.flipH && !r.flipV) {
        showMessage(tr("Keine Drehung zu speichern – zuerst mit ⌘L oder ⌘R drehen"), 2500);
        return;
    }
    m_rotationSaving = true;
    QPointer<ViewerWindow> self(this);

    // Carries out `method` through FileOps (undoable). RotationEdit keeps the undo backups
    // (UndoBackupStore: byte budget, clones) and refuses undo/redo after outside changes.
    auto commit = [self, r](const RotationPlan& plan, RotationMethod method) {
        if (!self)
            return;
        auto edit = std::make_shared<RotationEdit>(r, plan, method, UndoBackupStore::shared());
        FileOps::FileWork apply = [edit] { return edit->apply(); };
        FileOps::FileWork revert = [edit] { return edit->revert(); };
        const QString notice = plan.notice;
        self->m_app.fileOps().modifyAsync(
            r.path, tr("Drehung speichern"), apply, revert,
            [self, method, notice](bool ok, const QString& error) {
                if (!self)
                    return;
                self->m_rotationSaving = false;
                if (!ok) {
                    self->showMessage(error, 5000);
                    return;
                }
                const QString text = describeRotationMethod(method);
                self->showMessage(notice.isEmpty() ? text : text + QStringLiteral(" – ") + notice,
                                  notice.isEmpty() ? 1800 : 6000);
            });
    };

    auto decide = [self, commit](const RotationPlan& plan) {
        if (!self)
            return;
        switch (plan.status) {
        case RotationPlan::Refused:
            self->m_rotationSaving = false;
            self->showMessage(plan.message, 5000);
            return;
        case RotationPlan::Ready:
            if (plan.method == RotationMethod::None) {
                self->m_rotationSaving = false;
                self->showMessage(describeRotationMethod(plan.method));
                return;
            }
            commit(plan, plan.method);
            return;
        case RotationPlan::NeedsTrim:
        case RotationPlan::NeedsReencode:
            break;
        }
        // Never silently lossy (F-EDIT-02): ask first (non-blocking, window-modal).
        const bool trim = plan.status == RotationPlan::NeedsTrim;
        auto* box = new QMessageBox(QMessageBox::Question, tr("Drehung speichern"),
                                    trim ? tr("Verlustfrei drehen geht nur mit beschnittenem Rand.")
                                         : tr("Die Drehung kann nur durch Neukodieren gespeichert werden."),
                                    QMessageBox::NoButton, self);
        box->setObjectName(trim ? QStringLiteral("lfRotationTrim") : QStringLiteral("lfRotationReencode"));
        box->setInformativeText(plan.message);
        QPushButton* primary =
            box->addButton(trim ? tr("Rand abschneiden") : tr("Neu kodieren"), QMessageBox::AcceptRole);
        QPushButton* exif =
            trim ? box->addButton(tr("Nur EXIF-Ausrichtung setzen"), QMessageBox::ActionRole) : nullptr;
        if (exif)
            exif->setObjectName(QStringLiteral("lfRotationExif"));
        primary->setObjectName(QStringLiteral("lfRotationPrimary"));
        box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
        box->setDefaultButton(exif ? exif : primary);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::WindowModal);
        QObject::connect(box, &QMessageBox::buttonClicked, self,
                         [self, commit, plan, primary, exif](QAbstractButton* b) {
                             if (b == primary)
                                 commit(plan, plan.method);
                             else if (exif && b == exif)
                                 commit(plan, RotationMethod::MetadataCopy);
                             else if (self)
                                 self->m_rotationSaving = false;
                         });
        box->open();
    };

    QtConcurrent::run([r] { return planRotationSave(r); }).then(this, decide);
}

} // namespace lf
