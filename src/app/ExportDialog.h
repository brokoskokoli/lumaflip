// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// "Exportieren …" (F-EDIT-04): the options dialog (format, quality, maximum size, metadata,
// color space, HDR, name pattern, target folder; remembered in the settings) and the run:
// planning and the F-FILE-11 conflict questions, then the export on a worker thread with a
// progress dialog that can cancel between images. The sources are never changed.

#include "core/ImageExport.h"

#include <QDialog>
#include <QPointer>
#include <QProgressDialog>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace lf {

class TrashService;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(const QStringList& sources, QWidget* parent = nullptr);

    exporting::Options options() const;
    void setOptions(const exporting::Options& options);
    // F-EDIT-03 "Zugeschnitten exportieren": only this part (normalized, Options::crop).
    void setCrop(const QRectF& crop);
    QRectF crop() const { return m_crop; }
    QStringList sources() const { return m_sources; }
    QString previewName() const; // file name of the first image with the current options

    static exporting::Options optionsFromSettings();
    static void saveOptions(const exporting::Options& options);

protected:
    void changeEvent(QEvent* event) override;

private:
    void retranslateUi();
    void updateState();
    void chooseFolder();

    QStringList m_sources;
    QRectF m_crop;
    QComboBox* m_format = nullptr;
    QSlider* m_quality = nullptr;
    QLabel* m_qualityValue = nullptr;
    QSpinBox* m_maxEdge = nullptr;
    QComboBox* m_metadata = nullptr;
    QComboBox* m_color = nullptr;
    QComboBox* m_hdr = nullptr;
    QLineEdit* m_pattern = nullptr;
    QCheckBox* m_nextToSource = nullptr;
    QLineEdit* m_folder = nullptr;
    QPushButton* m_chooseFolder = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_header = nullptr;
    QList<QLabel*> m_labels;
};

// Runs an export with conflict questions and progress (GUI side). Deletes itself when done.
class ExportRunner : public QObject {
    Q_OBJECT
public:
    // `done(result)` on the GUI thread (also after cancelling).
    ExportRunner(QWidget* window, TrashService* trash, QStringList sources, exporting::Options options,
                 std::function<void(const exporting::Result&)> done);
    // Destroyed with its window while the worker runs: the worker stops after the current
    // image (it holds only shared copies; nothing is reported any more).
    ~ExportRunner() override;
    void start();
    QProgressDialog* progressDialog() const { return m_progress; } // tests
    void cancel();

private:
    void askNext(int next, bool applyAll, ConflictChoice allChoice);
    void run();

    QPointer<QWidget> m_window;
    TrashService* m_trash;
    QStringList m_sources;
    exporting::Options m_options;
    std::function<void(const exporting::Result&)> m_done;
    QVector<exporting::Item> m_items;
    QVector<int> m_conflicts;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    QPointer<QProgressDialog> m_progress;
};

} // namespace lf
