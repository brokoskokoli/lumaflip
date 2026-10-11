// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowEditing.h"

#include "Application.h"
#include "CompareView.h"
#include "CropOverlay.h"
#include "ExportDialog.h"
#include "ImageView.h"
#include "OverviewController.h"
#include "ViewerWindow.h"
#include "WindowExtras.h"
#include "core/Decoder.h"
#include "core/FileOps.h"
#include "core/ImageExport.h"
#include "core/PhotoOverlays.h"
#include "core/Settings.h"
#include "core/UndoBackupStore.h"
#include "core/VirtualPath.h"

#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>

namespace lf {

// ---------------------------------------------------------------------------------------
// AF points overlay
// ---------------------------------------------------------------------------------------

AfPointsOverlay::AfPointsOverlay(ImageView* view) : QWidget(view), m_view(view) {
    setObjectName(QStringLiteral("lfAfPointsOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setGeometry(view->rect());
    view->installEventFilter(this);
    connect(view, &ImageView::viewTransformChanged, this, qOverload<>(&QWidget::update));
    connect(view, &ImageView::zoomChanged, this, qOverload<>(&QWidget::update));
    hide();
}

void AfPointsOverlay::setPoints(const QVector<afpoints::Point>& displayed, const QString& source) {
    m_points = displayed;
    m_source = source;
    update();
}

void AfPointsOverlay::paintEvent(QPaintEvent*) {
    if (!m_view || !m_view->image() || m_points.isEmpty())
        return;
    const auto img = m_view->image();
    const QSizeF full(img->fullSize.isEmpty() ? img->image.size() : img->fullSize);
    const QTransform t = m_view->imageToWidget();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Plain frames first, the selected and in-focus ones on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (const afpoints::Point& pt : std::as_const(m_points)) {
            const bool strong = pt.inFocus || pt.selected;
            if (strong != (pass == 1))
                continue;
            QRectF r = t.mapRect(QRectF(pt.rect.x() * full.width(), pt.rect.y() * full.height(),
                                        pt.rect.width() * full.width(), pt.rect.height() * full.height()));
            if (r.width() < 8)
                r.adjust(-(8 - r.width()) / 2, 0, (8 - r.width()) / 2, 0);
            if (r.height() < 8)
                r.adjust(0, -(8 - r.height()) / 2, 0, (8 - r.height()) / 2);
            p.setPen(QPen(QColor(0, 0, 0, 150), strong ? 4 : 3));
            p.drawRect(r);
            const QColor c =
                pt.inFocus ? QColor(70, 230, 100) : (pt.selected ? Qt::white : QColor(210, 210, 210, 150));
            p.setPen(QPen(c, pt.inFocus ? 2 : 1));
            p.drawRect(r);
        }
    }
}

bool AfPointsOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::Resize)
        setGeometry(m_view->rect());
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------------------
// WindowEditing
// ---------------------------------------------------------------------------------------

WindowEditing::WindowEditing(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfWindowEditing"));
    m_af = new AfPointsOverlay(window.view());
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k.startsWith(QLatin1String("overlay/")))
            applyOverlays();
        if (k == QLatin1String(key::OverlayAfPoints))
            updateAfPoints();
    });
    if (OverviewController* overview = window.extras() ? window.extras()->overview() : nullptr) {
        connect(overview, &OverviewController::modeChanged, this, [this, overview] {
            if (overview->mode() != OverviewController::Mode::Viewer)
                cancelCrop();
            if (CompareView* c = overview->compareView()) {
                // Panes are rebuilt for every comparison: they get the overlays as well.
                connect(c, &CompareView::focusChanged, this, &WindowEditing::applyOverlays,
                        Qt::UniqueConnection);
            }
            applyOverlays();
        });
    }
    applyOverlays();
}

WindowEditing::~WindowEditing() {
    if (m_crop)
        qApp->removeEventFilter(this);
}

WindowEditing* WindowEditing::of(ViewerWindow* window) {
    return window ? window->findChild<WindowEditing*>(QStringLiteral("lfWindowEditing"),
                                                      Qt::FindDirectChildrenOnly)
                  : nullptr;
}

bool WindowEditing::perform(Action action) {
    switch (action) {
    case Action::CropImage:
        if (cropActive())
            cancelCrop();
        else
            startCrop();
        return true;
    case Action::ExportImage:
        showExport();
        return true;
    case Action::ToggleClipping:
        toggleSetting(key::OverlayClipping,
                      tr("Über-/Unterbelichtungswarnung an (rot: Lichter, blau: Schatten)"),
                      tr("Über-/Unterbelichtungswarnung aus"));
        return true;
    case Action::ToggleFocusPeaking:
        toggleSetting(key::OverlayPeaking, tr("Focus Peaking an"), tr("Focus Peaking aus"));
        return true;
    case Action::ToggleAfPoints:
        toggleSetting(key::OverlayAfPoints, tr("Fokuspunkte an"), tr("Fokuspunkte aus"));
        return true;
    default:
        return false;
    }
}

void WindowEditing::currentChanged(const QString& path) {
    if (m_crop && path != m_cropPath)
        cancelCrop(); // flipping ends the crop
    if (path != m_af->property("lfPath").toString()) {
        // Another file: the frames go, and coming back reads them again (A -> B -> A).
        m_af->setPoints({}, {});
        m_af->setProperty("lfPath", QString());
        m_afPath.clear();
        ++m_afToken; // a read still running for the old file is dropped
    }
}

void WindowEditing::imageShown(const QString& path) {
    Q_UNUSED(path);
    if (m_crop && m_crop->imageRect().isEmpty())
        m_crop->reset();
    updateAfPoints();
}

void WindowEditing::toggleSetting(const char* k, const QString& onText, const QString& offText) {
    Settings& st = Settings::instance();
    const bool on = !st.value(k).toBool();
    st.setValue(k, on); // Settings::changed applies it to every window
    m_window.showMessage(on ? onText : offText, 2500);
    m_app.windowChanged(&m_window);
}

// ---- overlays ----

void WindowEditing::applyOverlays() {
    const overlays::Params p = overlays::fromSettings();
    for (ImageView* v : m_window.findChildren<ImageView*>())
        v->setPhotoOverlays(p);
}

QString WindowEditing::statusText() const {
    return Settings::instance().value(key::OverlayAfPoints).toBool() ? m_afStatus : QString();
}

void WindowEditing::updateAfPoints() {
    const bool on = Settings::instance().value(key::OverlayAfPoints).toBool();
    const QString path = m_window.shownPath();
    if (!on || path.isEmpty() || vpath::isMember(path)) {
        m_af->setPoints({}, {});
        m_af->hide();
        m_afStatus.clear();
        ++m_afToken;
        m_window.refreshStatus();
        return;
    }
    // The worker compares the file stamp: a better stage of the same file keeps the frames,
    // a rewritten file (saved rotation, crop, an edit elsewhere) is read again.
    const QString knownStamp = m_af->isVisible() && m_afPath == path ? m_afStamp : QString();
    const quint64 token = ++m_afToken;
    m_af->setProperty("lfPath", path);
    QPointer<WindowEditing> self(this);
    struct Read {
        afpoints::Result result;
        QString stamp;
        bool unchanged = false;
    };
    QtConcurrent::run([path, knownStamp] {
        Read out;
        const QFileInfo fi(path);
        out.stamp = QStringLiteral("%1/%2").arg(fi.lastModified().toMSecsSinceEpoch()).arg(fi.size());
        if (!knownStamp.isEmpty() && out.stamp == knownStamp) {
            out.unchanged = true;
            return out;
        }
        afpoints::Result r = afpoints::read(path);
        QString error;
        int orientation = 1;
        if (r.valid())
            if (auto info = DecoderRegistry::instance().info(path, &error))
                orientation = info->exifOrientation;
        r.points = afpoints::toDisplay(r.points, orientation);
        out.result = r;
        return out;
    }).then(this, [self, token, path](const Read& read) {
        if (!self || token != self->m_afToken || read.unchanged)
            return;
        const afpoints::Result& r = read.result;
        self->m_afPath = path;
        self->m_afStamp = read.stamp;
        self->m_af->setPoints(r.points, r.source);
        self->m_af->show();
        self->m_af->raise();
        if (self->m_crop)
            self->m_crop->raise();
        //: %1 = where the AF points come from ("Canon", "Sony", "EXIF" …)
        self->m_afStatus =
            r.valid() ? tr("Fokuspunkte: %1").arg(r.source) : tr("Keine Fokuspunkte in den Metadaten");
        self->m_window.refreshStatus();
        if (!r.valid())
            self->m_window.showMessage(self->m_afStatus, 2000); // F-INFO-07: say why nothing shows
    });
}

// ---- crop ----

void WindowEditing::startCrop() {
    OverviewController* overview = m_window.extras() ? m_window.extras()->overview() : nullptr;
    if (overview && overview->mode() != OverviewController::Mode::Viewer) {
        m_window.showMessage(tr("Zuschneiden ist nur in der Einzelbildansicht möglich."), 2500);
        return;
    }
    const QString path = m_window.currentPath();
    ImageView* view = m_window.view();
    if (path.isEmpty() || m_window.shownPath() != path || !view->image() || view->isShowingError()) {
        m_window.showMessage(tr("Kein Bild zum Zuschneiden."), 2000);
        return;
    }
    Settings& st = Settings::instance();
    m_cropPath = path;
    ++m_cropToken;
    m_crop = new CropOverlay(view);
    m_crop->setGridShown(st.value(key::CropGrid).toBool());
    m_crop->setPortrait(st.value(key::CropPortrait).toBool());
    m_crop->setAspect(crop::aspectFromId(st.value(key::CropAspect).toString()));
    m_crop->show();
    m_crop->raise();
    connect(m_crop, &CropOverlay::saveRequested, this, &WindowEditing::saveCrop);
    connect(m_crop, &CropOverlay::cancelRequested, this, &WindowEditing::cancelCrop);
    qApp->installEventFilter(this);
    m_window.showMessage(tr("Zuschneiden: Rahmen ziehen – Enter sichert, Esc bricht ab"), 3500);
    m_app.windowChanged(&m_window);
}

void WindowEditing::cancelCrop() {
    if (!m_crop)
        return;
    Settings& st = Settings::instance();
    st.setValue(key::CropAspect, crop::aspectId(m_crop->aspect()));
    st.setValue(key::CropPortrait, m_crop->portrait());
    st.setValue(key::CropGrid, m_crop->gridShown());
    qApp->removeEventFilter(this);
    m_crop->hide();
    m_crop->deleteLater();
    m_crop = nullptr;
    // A plan still running or a question still open belongs to this session: dropped (the
    // token), nothing is written after Esc. A write already under way finishes and reports.
    ++m_cropToken;
    if (!m_committing)
        m_saving = false;
    if (m_question)
        m_question->close();
    if (m_saveDialog)
        m_saveDialog->close();
    m_window.view()->setFocus();
    m_app.windowChanged(&m_window);
}

bool WindowEditing::eventFilter(QObject* watched, QEvent* event) {
    if (!m_crop || (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress))
        return QObject::eventFilter(watched, event);
    // Only keys meant for this window, and not while a question or file dialog is open.
    if (QApplication::activeWindow() != &m_window || m_question || m_saveDialog)
        return QObject::eventFilter(watched, event);
    auto* ke = static_cast<QKeyEvent*>(event);
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
    const bool enter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && mods == Qt::NoModifier;
    const bool escape = ke->key() == Qt::Key_Escape && mods == Qt::NoModifier;
    if (!enter && !escape)
        return QObject::eventFilter(watched, event);
    event->accept(); // before the menu shortcuts (Return = rename, Esc = leave a mode)
    if (event->type() == QEvent::KeyPress && !ke->isAutoRepeat()) {
        if (enter)
            saveCrop(false);
        else
            cancelCrop();
    }
    return true;
}

QRectF WindowEditing::normalizedCrop() const {
    if (!m_crop)
        return {};
    const QSizeF size(m_crop->imageSize());
    const QRect r = m_crop->cropRect();
    if (size.isEmpty() || r.isEmpty())
        return {};
    return QRectF(r.x() / size.width(), r.y() / size.height(), r.width() / size.width(),
                  r.height() / size.height());
}

void WindowEditing::showQuestion(QMessageBox* box) {
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    m_question = box;
    box->open();
}

void WindowEditing::saveCrop(bool saveAs) {
    if (!m_crop || m_saving)
        return;
    if (m_committing) {
        m_window.showMessage(tr("Der vorherige Zuschnitt wird noch gesichert."), 2000);
        return;
    }
    CropRequest r;
    r.path = m_cropPath;
    r.rect = m_crop->cropRect();
    r.keepModificationDate = Settings::instance().value(key::KeepModificationDate).toBool();
    if (r.rect.isEmpty()) {
        m_window.showMessage(tr("Kein Bildbereich ausgewählt."), 2000);
        return;
    }
    if (!saveAs) {
        planAndSave(r);
        return;
    }
    const QFileInfo fi(m_cropPath);
    //: default file name of a cropped copy: "<name>-zugeschnitten.<suffix>"
    const QString suggested =
        fi.dir().filePath(tr("%1-zugeschnitten.%2").arg(fi.completeBaseName(), fi.suffix()));
    auto* dlg = new QFileDialog(&m_window, tr("Zugeschnittenes Bild sichern unter"), suggested);
    dlg->setObjectName(QStringLiteral("lfCropSaveAsDialog"));
    dlg->setAcceptMode(QFileDialog::AcceptSave);
    // The format of the original first; the export formats write a cropped export instead.
    QStringList filters{tr("Wie das Original (*.%1)").arg(fi.suffix())};
    for (const exporting::Format& f : exporting::availableFormats())
        if (f.suffix.compare(fi.suffix(), Qt::CaseInsensitive) != 0)
            filters << QStringLiteral("%1 (*.%2)").arg(f.label, f.suffix);
    dlg->setNameFilters(filters);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_saveDialog = dlg;
    connect(dlg, &QFileDialog::fileSelected, this, &WindowEditing::saveCropAs);
    dlg->open();
}

namespace {

QString canonicalSuffix(const QString& suffix) {
    QString s = suffix.toLower();
    if (s == QLatin1String("jpeg") || s == QLatin1String("jpe"))
        return QStringLiteral("jpg");
    if (s == QLatin1String("tiff"))
        return QStringLiteral("tif");
    return s;
}

} // namespace

void WindowEditing::saveCropAs(const QString& chosen) {
    if (!m_crop || m_saving || m_committing || chosen.isEmpty())
        return;
    QString path = chosen;
    const QString srcSuffix = QFileInfo(m_cropPath).suffix();
    const QString suffix = canonicalSuffix(QFileInfo(path).suffix());
    if (suffix != canonicalSuffix(srcSuffix)) {
        // Another format the export can write: a cropped export into exactly this file.
        for (const exporting::Format& f : exporting::availableFormats())
            if (canonicalSuffix(f.suffix) == suffix) {
                exportCroppedAs(path, f);
                return;
            }
        path += QLatin1Char('.') + srcSuffix; // else the format of the original
    }
    CropRequest r;
    r.path = m_cropPath;
    r.rect = m_crop->cropRect();
    r.saveAs = path;
    // The file dialog confirmed replacing `chosen`; a name with the suffix added is asked
    // here (never replaced silently).
    r.replaceExisting = path == chosen;
    const QFileInfo target(path);
    if (r.replaceExisting || !(target.exists() || target.isSymLink())) {
        planAndSave(r);
        return;
    }
    auto* box =
        new QMessageBox(QMessageBox::Warning, tr("Zuschneiden"),
                        tr("„%1“ existiert bereits. Möchtest du die Datei ersetzen?").arg(target.fileName()),
                        QMessageBox::NoButton, &m_window);
    box->setObjectName(QStringLiteral("lfCropReplaceExisting"));
    box->setInformativeText(
        tr("Das zugeschnittene Bild bekommt die Endung des Originals (.%1).").arg(srcSuffix));
    QPushButton* replace = box->addButton(tr("Ersetzen"), QMessageBox::DestructiveRole);
    replace->setObjectName(QStringLiteral("lfCropReplaceYes"));
    QPushButton* no = box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(no);
    QPointer<WindowEditing> self(this);
    const quint64 token = m_cropToken;
    connect(box, &QMessageBox::buttonClicked, this, [self, r, replace, token](QAbstractButton* b) mutable {
        if (!self || b != replace || token != self->m_cropToken || !self->m_crop)
            return;
        r.replaceExisting = true;
        self->planAndSave(r);
    });
    showQuestion(box);
}

void WindowEditing::exportCroppedAs(const QString& target, const exporting::Format& format) {
    exporting::Options o = ExportDialog::optionsFromSettings();
    o.formatId = format.id;
    o.maxLongEdge = 0;
    o.crop = normalizedCrop();
    exporting::Item item;
    item.source = m_cropPath;
    item.target = target;
    item.onConflict = ConflictChoice::Replace; // the file dialog asked
    m_saving = true;
    QPointer<WindowEditing> self(this);
    TrashService* trash = &m_app.fileOps().trashService();
    const quint64 token = m_cropToken;
    QtConcurrent::run([item, o, trash] {
        const std::atomic_bool never{false};
        return exporting::run({item}, o, never, {}, trash);
    }).then(this, [self, target, token](const exporting::Result& r) {
        if (!self)
            return;
        if (token == self->m_cropToken)
            self->m_saving = false;
        if (r.written == 0) {
            self->m_window.showMessage(r.errors.isEmpty() ? tr("Der Zuschnitt konnte nicht gesichert werden.")
                                                          : r.errors.join(QLatin1Char('\n')),
                                       5000);
            return;
        }
        if (token == self->m_cropToken)
            self->cancelCrop();
        QString text = tr("Zugeschnitten exportiert als „%1“").arg(QFileInfo(target).fileName());
        for (const QString& n : r.notices)
            text += QStringLiteral(" – ") + n;
        self->m_window.showMessage(text, r.notices.isEmpty() ? 2500 : 6000);
    });
}

void WindowEditing::offerCroppedExport(const QString& message) {
    auto* box = new QMessageBox(QMessageBox::Information, tr("Zuschneiden"), message, QMessageBox::NoButton,
                                &m_window);
    box->setObjectName(QStringLiteral("lfCropExportInstead"));
    QPushButton* exportButton = box->addButton(tr("Zugeschnitten exportieren …"), QMessageBox::AcceptRole);
    exportButton->setObjectName(QStringLiteral("lfCropExportInsteadYes"));
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(exportButton);
    QPointer<WindowEditing> self(this);
    const quint64 token = m_cropToken;
    connect(box, &QMessageBox::buttonClicked, this, [self, exportButton, token](QAbstractButton* b) {
        if (!self || b != exportButton || token != self->m_cropToken || !self->m_crop)
            return;
        // After this click handler: the question closes first (window-modal dialogs).
        QTimer::singleShot(0, self, [self, token] {
            if (self && token == self->m_cropToken && self->m_crop)
                self->showExport(self->normalizedCrop());
        });
    });
    showQuestion(box);
}

void WindowEditing::planAndSave(const CropRequest& request) {
    m_saving = true;
    QPointer<WindowEditing> self(this);
    const quint64 token = m_cropToken;
    QtConcurrent::run([request] {
        return planCropSave(request);
    }).then(this, [self, request, token](const CropPlan& plan) {
        if (!self || token != self->m_cropToken)
            return; // cancelled meanwhile (Esc, flipping): nothing is written
        switch (plan.status) {
        case CropPlan::Refused:
            self->m_saving = false;
            if (plan.exportInstead)
                self->offerCroppedExport(plan.message);
            else
                self->m_window.showMessage(plan.message, 5000);
            return;
        case CropPlan::Ready:
            self->commitCrop(request, plan, plan.method);
            return;
        case CropPlan::NeedsReencode:
            break;
        }
        // Never silently lossy (SPEC 4.8): ask first (window-modal, non-blocking).
        auto* box = new QMessageBox(QMessageBox::Question, tr("Zuschneiden"),
                                    tr("Der Zuschnitt kann nur durch Neukodieren gesichert werden."),
                                    QMessageBox::NoButton, &self->m_window);
        box->setObjectName(QStringLiteral("lfCropReencode"));
        box->setInformativeText(plan.message);
        QPushButton* reencode = box->addButton(tr("Neu kodieren"), QMessageBox::AcceptRole);
        reencode->setObjectName(QStringLiteral("lfCropReencodeYes"));
        box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
        box->setDefaultButton(reencode);
        connect(box, &QMessageBox::buttonClicked, self,
                [self, request, plan, reencode, token](QAbstractButton* b) {
                    if (!self || token != self->m_cropToken)
                        return;
                    if (b == reencode)
                        self->commitCrop(request, plan, plan.method);
                    else
                        self->m_saving = false;
                });
        // Closed without a button (cancelCrop): the session is over, m_saving was reset there.
        self->showQuestion(box);
    });
}

void WindowEditing::commitCrop(const CropRequest& request, const CropPlan& plan, CropMethod method) {
    QPointer<WindowEditing> self(this);
    const QString notice = plan.notice;
    const bool snapped = plan.snapped;
    const quint64 token = m_cropToken;
    m_committing = true;
    if (request.saveAs.isEmpty()) {
        std::shared_ptr<FileEdit> edit = makeCropEdit(request, plan, method, UndoBackupStore::shared());
        FileOps::FileWork apply = [edit] { return edit->apply(); };
        FileOps::FileWork revert = [edit] { return edit->revert(); };
        m_app.fileOps().modifyAsync(request.path, tr("Zuschneiden"), apply, revert,
                                    [self, method, notice, snapped, token](bool ok, const QString& error) {
                                        if (!self)
                                            return;
                                        self->m_committing = false;
                                        if (token == self->m_cropToken)
                                            self->m_saving = false;
                                        if (!ok) {
                                            self->m_window.showMessage(error, 5000);
                                            return;
                                        }
                                        if (token == self->m_cropToken)
                                            self->cancelCrop();
                                        const QString text = describeCropMethod(method, snapped);
                                        self->m_window.showMessage(
                                            notice.isEmpty() ? text : text + QStringLiteral(" – ") + notice,
                                            notice.isEmpty() ? 2000 : 6000);
                                    });
        return;
    }
    QtConcurrent::run([request, plan, method] {
        return executeCropSave(request, plan, method);
    }).then(this, [self, request, notice, token](const QString& error) {
        if (!self)
            return;
        self->m_committing = false;
        if (token == self->m_cropToken)
            self->m_saving = false;
        if (!error.isEmpty()) {
            self->m_window.showMessage(error, 5000);
            return;
        }
        if (token == self->m_cropToken)
            self->cancelCrop();
        const QString text = tr("Zugeschnitten gesichert als „%1“").arg(QFileInfo(request.saveAs).fileName());
        self->m_window.showMessage(notice.isEmpty() ? text : text + QStringLiteral(" – ") + notice,
                                   notice.isEmpty() ? 2500 : 6000);
    });
}

// ---- export ----

void WindowEditing::showExport(const QRectF& crop) {
    QStringList sources;
    OverviewController* overview = m_window.extras() ? m_window.extras()->overview() : nullptr;
    if (!crop.isEmpty() && m_crop)
        sources << m_cropPath;
    else if (overview && overview->mode() != OverviewController::Mode::Viewer)
        sources = overview->subjectPaths();
    else if (!m_window.currentPath().isEmpty())
        sources << m_window.currentPath();
    if (sources.isEmpty()) {
        m_window.showMessage(tr("Kein Bild zum Exportieren."), 2000);
        return;
    }
    if (m_exportDialog)
        m_exportDialog->close();
    auto* dlg = new ExportDialog(sources, &m_window);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    if (!crop.isEmpty() && m_crop)
        dlg->setCrop(crop);
    m_exportDialog = dlg;
    QPointer<WindowEditing> self(this);
    connect(dlg, &QDialog::accepted, this, [self, dlg] {
        if (!self)
            return;
        const exporting::Options o = dlg->options();
        ExportDialog::saveOptions(o); // without the crop (not a setting)
        if (!o.crop.isEmpty())
            self->cancelCrop(); // the cropped copy is on its way
        self->runExport(dlg->sources(), o);
    });
    dlg->open();
}

void WindowEditing::runExport(const QStringList& sources, const exporting::Options& options) {
    QPointer<WindowEditing> self(this);
    auto* runner = new ExportRunner(
        &m_window, &m_app.fileOps().trashService(), sources, options, [self](const exporting::Result& r) {
            if (!self)
                return;
            ViewerWindow& w = self->m_window;
            if (r.failed > 0) {
                auto* box = new QMessageBox(
                    QMessageBox::Warning, tr("Exportieren"),
                    tr("%n Bilder konnten nicht exportiert werden.", nullptr, r.failed), QMessageBox::Ok, &w);
                box->setObjectName(QStringLiteral("lfExportErrors"));
                box->setDetailedText(r.errors.join(QLatin1Char('\n')));
                box->setAttribute(Qt::WA_DeleteOnClose);
                box->setWindowModality(Qt::WindowModal);
                box->open();
            }
            QString text = r.cancelled ? tr("Export abgebrochen – %n Bilder exportiert", nullptr, r.written)
                                       : tr("%n Bilder exportiert", nullptr, r.written);
            if (r.skipped > 0)
                text += QStringLiteral(" · ") + tr("%n übersprungen", nullptr, r.skipped);
            for (const QString& n : r.notices)
                text += QStringLiteral(" – ") + n;
            w.showMessage(text, r.notices.isEmpty() ? 3000 : 6000);
        });
    runner->start();
}

} // namespace lf
