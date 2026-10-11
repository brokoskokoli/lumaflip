// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M4 editing and photo tools of one viewer window:
// - F-EDIT-03 crop (⇧C): CropOverlay over the image view; Enter / "Speichern" saves into
//   the file (FileOps, undoable, core/CropSave.h), "Speichern unter …" writes a new file
//   (another export format: a cropped export; an existing file only after asking), Esc
//   cancels - also while the save is still being planned. Lossy saving and JPEG block
//   snapping are asked / told first; what cannot be cropped in place (RAW, animations,
//   archive members, formats without an encoder) is offered as "Zugeschnitten exportieren".
// - F-EDIT-04 export (⇧⌘E): ExportDialog for the current image or the grid/compare
//   selection, run with progress and cancel (ExportRunner).
// - F-INFO-07 photo overlays (Darstellung → Overlays): clipping warning (J) and focus peaking
//   (⇧J) in the image shader of every view of the window, AF points (⇧A) from the maker
//   notes as frames over the image (core/AfPoints.h).

#include "CropOverlay.h"
#include "ExportDialog.h"
#include "core/AfPoints.h"
#include "core/CropSave.h"
#include "core/InputBindings.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QWidget>

namespace lf {

class Application;
class ImageView;
class ViewerWindow;

// AF points drawn over an ImageView (normalized displayed coordinates).
class AfPointsOverlay : public QWidget {
    Q_OBJECT
public:
    explicit AfPointsOverlay(ImageView* view);
    void setPoints(const QVector<afpoints::Point>& displayed, const QString& source);
    QVector<afpoints::Point> points() const { return m_points; }
    QString source() const { return m_source; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<ImageView> m_view;
    QVector<afpoints::Point> m_points;
    QString m_source;
};

class WindowEditing : public QObject {
    Q_OBJECT
public:
    WindowEditing(ViewerWindow& window, Application& app);
    ~WindowEditing() override;

    static WindowEditing* of(ViewerWindow* window); // null if none

    // F-EDIT-03/04, F-INFO-07 actions; true: handled.
    bool perform(Action action);
    void currentChanged(const QString& path);
    void imageShown(const QString& path);

    // ---- crop ----
    bool cropActive() const { return m_crop != nullptr; }
    CropOverlay* cropOverlay() const { return m_crop; }
    void startCrop();
    void cancelCrop();
    // Saves the selection: into the file, or (`saveAs`) asks for a new file name.
    void saveCrop(bool saveAs);
    // "Speichern unter" with the name chosen in the file dialog (which confirmed replacing
    // that very file); tests call it directly.
    void saveCropAs(const QString& path);
    QMessageBox* cropQuestion() const { return m_question; } // tests
    bool cropSaving() const { return m_saving; }

    // ---- export ----
    // `crop` (normalized, exporting::Options::crop): "Zugeschnitten exportieren" of the
    // current image; the crop mode ends when the dialog is accepted.
    void showExport(const QRectF& crop = {});
    ExportDialog* exportDialog() const { return m_exportDialog; } // tests
    // Starts the export of `sources` with `options` (tests call it directly).
    void runExport(const QStringList& sources, const exporting::Options& options);

    // ---- overlays ----
    void applyOverlays();
    AfPointsOverlay* afPointsOverlay() const { return m_af; }
    QString statusText() const; // "Fokuspunkte: keine in den Metadaten" … else empty

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void planAndSave(const CropRequest& request);
    void commitCrop(const CropRequest& request, const CropPlan& plan, CropMethod method);
    void offerCroppedExport(const QString& message);
    void exportCroppedAs(const QString& target, const exporting::Format& format);
    QRectF normalizedCrop() const; // the selection relative to the shown image
    void showQuestion(QMessageBox* box);
    void toggleSetting(const char* key, const QString& onText, const QString& offText);
    void updateAfPoints();

    ViewerWindow& m_window;
    Application& m_app;
    QPointer<CropOverlay> m_crop;
    QString m_cropPath;
    bool m_saving = false;     // a save is planned / asked / written
    bool m_committing = false; // the file is being written (not cancellable any more)
    quint64 m_cropToken = 0;   // a new crop session or cancelling: late plans are dropped
    QPointer<QMessageBox> m_question;
    QPointer<QFileDialog> m_saveDialog;
    QPointer<ExportDialog> m_exportDialog;
    AfPointsOverlay* m_af = nullptr;
    quint64 m_afToken = 0;
    QString m_afStatus;
    // The file the shown AF points belong to and its stamp (modification time, size): a
    // rewritten file is read again, a better stage of the same file is not.
    QString m_afPath;
    QString m_afStamp;
};

} // namespace lf
