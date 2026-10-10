// SPDX-License-Identifier: GPL-3.0-or-later
#include "ExportDialog.h"

#include "QuickMoveController.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

// ---------------------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------------------

ExportDialog::ExportDialog(const QStringList& sources, QWidget* parent)
    : QDialog(parent), m_sources(sources) {
    setObjectName(QStringLiteral("lfExportDialog"));
    auto* outer = new QVBoxLayout(this);
    m_header = new QLabel(this);
    m_header->setWordWrap(true);
    outer->addWidget(m_header);
    auto* form = new QFormLayout;
    outer->addLayout(form);
    auto row = [&](QWidget* field) {
        auto* label = new QLabel(this);
        m_labels << label;
        form->addRow(label, field);
        return label;
    };

    m_format = new QComboBox(this);
    m_format->setObjectName(QStringLiteral("lfExportFormat"));
    for (const exporting::Format& f : exporting::availableFormats())
        m_format->addItem(f.label, f.id);
    row(m_format);

    auto* qualityBox = new QWidget(this);
    auto* qualityRow = new QHBoxLayout(qualityBox);
    qualityRow->setContentsMargins(0, 0, 0, 0);
    m_quality = new QSlider(Qt::Horizontal, qualityBox);
    m_quality->setObjectName(QStringLiteral("lfExportQuality"));
    m_quality->setRange(1, 100);
    m_qualityValue = new QLabel(qualityBox);
    m_qualityValue->setMinimumWidth(40);
    qualityRow->addWidget(m_quality, 1);
    qualityRow->addWidget(m_qualityValue);
    row(qualityBox);

    m_maxEdge = new QSpinBox(this);
    m_maxEdge->setObjectName(QStringLiteral("lfExportMaxEdge"));
    m_maxEdge->setRange(0, 65535);
    m_maxEdge->setSingleStep(256);
    row(m_maxEdge);

    m_metadata = new QComboBox(this);
    m_metadata->setObjectName(QStringLiteral("lfExportMetadata"));
    row(m_metadata);
    m_color = new QComboBox(this);
    m_color->setObjectName(QStringLiteral("lfExportColor"));
    row(m_color);
    m_hdr = new QComboBox(this);
    m_hdr->setObjectName(QStringLiteral("lfExportHdr"));
    row(m_hdr);

    m_pattern = new QLineEdit(this);
    m_pattern->setObjectName(QStringLiteral("lfExportPattern"));
    row(m_pattern);

    auto* folderBox = new QWidget(this);
    auto* folderRow = new QHBoxLayout(folderBox);
    folderRow->setContentsMargins(0, 0, 0, 0);
    m_nextToSource = new QCheckBox(folderBox);
    m_nextToSource->setObjectName(QStringLiteral("lfExportNextToSource"));
    m_folder = new QLineEdit(folderBox);
    m_folder->setObjectName(QStringLiteral("lfExportFolder"));
    m_chooseFolder = new QPushButton(folderBox);
    m_chooseFolder->setObjectName(QStringLiteral("lfExportChooseFolder"));
    folderRow->addWidget(m_nextToSource);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(m_chooseFolder);
    row(folderBox);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("lfExportPreview"));
    m_preview->setWordWrap(true);
    m_preview->setTextFormat(Qt::PlainText);
    outer->addWidget(m_preview);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("lfExportButtons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QComboBox* c : {m_format, m_metadata, m_color, m_hdr})
        connect(c, &QComboBox::currentIndexChanged, this, &ExportDialog::updateState);
    connect(m_quality, &QSlider::valueChanged, this, &ExportDialog::updateState);
    connect(m_maxEdge, &QSpinBox::valueChanged, this, &ExportDialog::updateState);
    connect(m_pattern, &QLineEdit::textChanged, this, &ExportDialog::updateState);
    connect(m_folder, &QLineEdit::textChanged, this, &ExportDialog::updateState);
    connect(m_nextToSource, &QCheckBox::toggled, this, &ExportDialog::updateState);
    connect(m_chooseFolder, &QPushButton::clicked, this, &ExportDialog::chooseFolder);

    retranslateUi();
    setOptions(optionsFromSettings());
}

void ExportDialog::setCrop(const QRectF& crop) {
    m_crop = crop;
    retranslateUi();
}

void ExportDialog::retranslateUi() {
    setWindowTitle(m_crop.isEmpty() ? tr("Exportieren") : tr("Zugeschnitten exportieren"));
    if (!m_crop.isEmpty() && m_sources.size() == 1)
        m_header->setText(tr("Den Ausschnitt von „%1“ als neue Datei exportieren. Das Original bleibt "
                             "unverändert.")
                              .arg(QFileInfo(m_sources.first()).fileName()));
    else
        m_header->setText(
            m_sources.size() == 1
                ? tr("„%1“ als neue Datei exportieren. Das Original bleibt unverändert.")
                      .arg(QFileInfo(m_sources.first()).fileName())
                : tr("%n Bilder als neue Dateien exportieren. Die Originale bleiben unverändert.", nullptr,
                     int(m_sources.size())));
    const QStringList labels{tr("Format:"),   tr("Qualität:"), tr("Maximale Größe:"), tr("Metadaten:"),
                             tr("Farbraum:"), tr("HDR:"),      tr("Dateiname:"),      tr("Zielordner:")};
    for (int i = 0; i < m_labels.size() && i < labels.size(); ++i)
        m_labels[i]->setText(labels.at(i));
    m_maxEdge->setSpecialValueText(tr("Originalgröße"));
    //: suffix of the maximum size (long edge in pixels)
    m_maxEdge->setSuffix(tr(" px (lange Kante)"));
    auto fill = [](QComboBox* box, const QList<QPair<QString, QString>>& items) {
        const QString current = box->currentData().toString();
        const QSignalBlocker block(box);
        box->clear();
        for (const auto& [id, text] : items)
            box->addItem(text, id);
        box->setCurrentIndex(std::max(0, box->findData(current)));
    };
    fill(m_metadata, {{QStringLiteral("all"), tr("Alle behalten")},
                      {QStringLiteral("noGps"), tr("Ohne Ortsdaten (GPS)")},
                      {QStringLiteral("none"), tr("Alle entfernen")}});
    fill(m_color, {{QStringLiteral("keep"), tr("Beibehalten")},
                   {QStringLiteral("srgb"), tr("In sRGB umwandeln")},
                   {QStringLiteral("p3"), tr("In Display P3 umwandeln")}});
    fill(m_hdr, {{QStringLiteral("keep"), tr("Gain-Map behalten (wo das Format es kann)")},
                 {QStringLiteral("sdr"), tr("Als SDR (Tone Mapping)")}});
    m_pattern->setToolTip(tr("Platzhalter: {name} = Name des Originals, {n} = laufende Nummer, {format} = "
                             "Dateiendung"));
    m_nextToSource->setText(tr("Neben dem Original"));
    m_chooseFolder->setText(tr("Wählen …"));
    updateState();
}

void ExportDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QDialog::changeEvent(event);
}

exporting::Options ExportDialog::options() const {
    exporting::Options o;
    o.formatId = m_format->currentData().toString();
    o.quality = m_quality->value();
    o.maxLongEdge = m_maxEdge->value();
    o.metadata = exporting::metadataModeFromId(m_metadata->currentData().toString());
    o.color = exporting::colorModeFromId(m_color->currentData().toString());
    o.hdr = exporting::hdrModeFromId(m_hdr->currentData().toString());
    o.pattern = m_pattern->text();
    o.folder = m_nextToSource->isChecked() ? QString() : m_folder->text().trimmed();
    o.crop = m_crop;
    return o;
}

void ExportDialog::setOptions(const exporting::Options& o) {
    m_format->setCurrentIndex(std::max(0, m_format->findData(o.formatId)));
    m_quality->setValue(o.quality);
    m_maxEdge->setValue(o.maxLongEdge);
    m_metadata->setCurrentIndex(std::max(0, m_metadata->findData(exporting::metadataModeId(o.metadata))));
    m_color->setCurrentIndex(std::max(0, m_color->findData(exporting::colorModeId(o.color))));
    m_hdr->setCurrentIndex(std::max(0, m_hdr->findData(exporting::hdrModeId(o.hdr))));
    m_pattern->setText(o.pattern);
    m_nextToSource->setChecked(o.folder.isEmpty());
    m_folder->setText(o.folder);
    updateState();
}

QString ExportDialog::previewName() const {
    if (m_sources.isEmpty())
        return {};
    return exporting::targetFileName(m_sources.first(), options(), 0, int(m_sources.size()));
}

void ExportDialog::updateState() {
    const exporting::Format f = exporting::formatById(m_format->currentData().toString());
    m_quality->setEnabled(f.lossy);
    m_qualityValue->setText(f.lossy ? QStringLiteral("%1").arg(m_quality->value()) : QString());
    m_hdr->setEnabled(f.gainMap);
    m_folder->setEnabled(!m_nextToSource->isChecked());
    m_chooseFolder->setEnabled(!m_nextToSource->isChecked());
    const exporting::Options o = options();
    const QString folder = m_sources.isEmpty() ? QString() : exporting::targetFolder(m_sources.first(), o);
    m_preview->setText(tr("Erste Datei: %1").arg(QDir(folder).filePath(previewName())));
}

void ExportDialog::chooseFolder() {
    auto* dlg = new QFileDialog(this, tr("Zielordner für den Export"), m_folder->text());
    dlg->setFileMode(QFileDialog::Directory);
    dlg->setOption(QFileDialog::ShowDirsOnly);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::fileSelected, this, [this](const QString& folder) {
        m_folder->setText(folder);
        m_nextToSource->setChecked(false);
    });
    dlg->open();
}

exporting::Options ExportDialog::optionsFromSettings() {
    Settings& st = Settings::instance();
    exporting::Options o;
    o.formatId = st.value(key::ExportFormat).toString();
    bool available = false;
    for (const exporting::Format& f : exporting::availableFormats())
        available = available || f.id == o.formatId;
    if (!available)
        o.formatId = QStringLiteral("jpeg");
    o.quality = st.value(key::ExportQuality).toInt();
    o.maxLongEdge = st.value(key::ExportMaxEdge).toInt();
    o.metadata = exporting::metadataModeFromId(st.value(key::ExportMetadata).toString());
    o.color = exporting::colorModeFromId(st.value(key::ExportColor).toString());
    o.hdr = exporting::hdrModeFromId(st.value(key::ExportHdr).toString());
    o.pattern = st.value(key::ExportPattern).toString();
    o.folder = st.value(key::ExportFolder).toString();
    return o;
}

void ExportDialog::saveOptions(const exporting::Options& o) {
    Settings& st = Settings::instance();
    st.setValue(key::ExportFormat, o.formatId);
    st.setValue(key::ExportQuality, o.quality);
    st.setValue(key::ExportMaxEdge, o.maxLongEdge);
    st.setValue(key::ExportMetadata, exporting::metadataModeId(o.metadata));
    st.setValue(key::ExportColor, exporting::colorModeId(o.color));
    st.setValue(key::ExportHdr, exporting::hdrModeId(o.hdr));
    st.setValue(key::ExportPattern, o.pattern);
    st.setValue(key::ExportFolder, o.folder);
}

// ---------------------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------------------

ExportRunner::ExportRunner(QWidget* window, TrashService* trash, QStringList sources,
                           exporting::Options options, std::function<void(const exporting::Result&)> done)
    : QObject(window), m_window(window), m_trash(trash), m_sources(std::move(sources)),
      m_options(std::move(options)), m_done(std::move(done)) {
    setObjectName(QStringLiteral("lfExportRunner"));
}

ExportRunner::~ExportRunner() {
    m_cancel->store(true);
}

void ExportRunner::start() {
    // Planning checks which names exist: on a worker (network volumes).
    const QStringList sources = m_sources;
    const exporting::Options options = m_options;
    QPointer<ExportRunner> self(this);
    QtConcurrent::run([sources, options] {
        auto items = exporting::planItems(sources, options);
        return std::make_pair(items, exporting::conflicts(items));
    }).then(this, [self](std::pair<QVector<exporting::Item>, QVector<int>> planned) {
        if (!self)
            return;
        self->m_items = planned.first;
        self->m_conflicts = planned.second;
        self->askNext(0, false, ConflictChoice::Skip);
    });
}

void ExportRunner::askNext(int next, bool applyAll, ConflictChoice allChoice) {
    while (next < m_conflicts.size() && applyAll)
        m_items[m_conflicts.at(next++)].onConflict = allChoice;
    if (next >= m_conflicts.size() || !m_window) {
        run();
        return;
    }
    const exporting::Item& item = m_items.at(m_conflicts.at(next));
    const QFileInfo target(item.target);
    QPointer<ExportRunner> self(this);
    QuickMoveController::askConflict(m_window, {target.fileName()}, target.absolutePath(),
                                     int(m_conflicts.size() - next),
                                     [self, next](std::optional<ConflictChoice> choice, bool forAll) {
                                         if (!self)
                                             return;
                                         if (!choice) { // cancelled: nothing is exported
                                             exporting::Result r;
                                             r.cancelled = true;
                                             if (self->m_done)
                                                 self->m_done(r);
                                             self->deleteLater();
                                             return;
                                         }
                                         self->m_items[self->m_conflicts.at(next)].onConflict = *choice;
                                         self->askNext(next + 1, forAll, *choice);
                                     });
}

void ExportRunner::cancel() {
    m_cancel->store(true);
}

void ExportRunner::run() {
    const int total = int(m_items.size());
    if (m_window) {
        m_progress = new QProgressDialog(m_window);
        m_progress->setObjectName(QStringLiteral("lfExportProgress"));
        m_progress->setWindowTitle(tr("Exportieren"));
        m_progress->setLabelText(tr("Exportiere …"));
        m_progress->setCancelButtonText(tr("Abbrechen"));
        m_progress->setRange(0, total);
        m_progress->setMinimumDuration(400);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setAutoClose(false);
        m_progress->setAutoReset(false);
        connect(m_progress, &QProgressDialog::canceled, this, &ExportRunner::cancel);
    }
    const auto items = m_items;
    const auto options = m_options;
    const auto cancelFlag = m_cancel;
    TrashService* trash = m_trash;
    QPointer<ExportRunner> self(this);
    QPointer<QProgressDialog> progress = m_progress;
    auto report = [progress](int done, int all, const QString& current) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [progress, done, all, current] {
                if (!progress)
                    return;
                progress->setValue(done);
                if (!current.isEmpty())
                    progress->setLabelText(
                        QCoreApplication::translate("lf::ExportRunner", "Exportiere %1 von %2: %3")
                            .arg(done + 1)
                            .arg(all)
                            .arg(QFileInfo(current).fileName()));
            },
            Qt::QueuedConnection);
    };
    QtConcurrent::run([items, options, cancelFlag, report, trash] {
        return exporting::run(items, options, *cancelFlag, report, trash);
    }).then(this, [self](const exporting::Result& r) {
        if (!self)
            return;
        if (self->m_progress)
            self->m_progress->deleteLater();
        if (self->m_done)
            self->m_done(r);
        self->deleteLater();
    });
}

} // namespace lf
