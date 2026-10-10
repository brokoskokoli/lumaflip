// SPDX-License-Identifier: GPL-3.0-or-later
#include "InfoPanel.h"

#include "DisplayText.h"
#include "HistogramWidget.h"
#include "InfoGpsBox.h"
#include "MapSnapshot.h"
#include "MetadataDialog.h"
#include "core/AppScoped.h"
#include "core/Decoder.h"
#include "core/LocaleFormat.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringListModel>
#include <QThreadPool>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace lf {

namespace {

constexpr int kCacheEntries = 64;
constexpr int kHistogramEntries = 32;
const int kSectionRole = Qt::UserRole + 1;

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::InfoPanel)
};

// Metadata reads (any number of windows) share two low-priority threads; parented to the
// application so they are joined before Qt shuts down.
QThreadPool* infoPool() {
    static AppScoped<QThreadPool> pool([](QObject* parent) {
        auto* p = new QThreadPool(parent);
        p->setMaxThreadCount(2);
        p->setThreadPriority(QThread::LowPriority);
        return p;
    });
    return &pool.get();
}

// Tag writes run one after another (the order of changes must be kept).
QThreadPool* tagWritePool() {
    static AppScoped<QThreadPool> pool([](QObject* parent) {
        auto* p = new QThreadPool(parent);
        p->setMaxThreadCount(1);
        return p;
    });
    return &pool.get();
}

QString formatDate(const QDateTime& dt) {
    return formatDateTime(dt.toLocalTime()); // locale of the UI language (F-I18N-04)
}

QString formatSize(qint64 bytes) {
    return formatByteSizeExact(bytes);
}

// Rows of the metadata group `group` ("camera") in the fixed order of metadataKeys(), with
// the labels in the UI language (F-INFO-02, F-I18N-01).
QList<QPair<QString, QString>> metadataRows(const Metadata& md, const QString& group) {
    QList<QPair<QString, QString>> rows;
    const QString prefix = group + QLatin1Char('.');
    for (const QString& key : metadataKeys()) {
        if (!key.startsWith(prefix))
            continue;
        for (const MetadataEntry& e : md.entries)
            if (e.key == key)
                rows.append({metadataLabel(key), e.value.toString()});
    }
    return rows;
}

constexpr int kMinTreeRows = 6; // metadata rows that stay visible in small windows

// A section title that collapses `body` (state remembered in the settings).
QToolButton* makeSectionHeader(const QString& text, const QString& name, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setText(text);
    b->setCheckable(true);
    b->setChecked(true); // expanded
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setArrowType(Qt::DownArrow);
    b->setIconSize(QSize(9, 9)); // a small disclosure arrow
    b->setToolTip(Tr::tr("Ein- oder ausklappen"));
    QFont bold = b->font();
    bold.setBold(true);
    b->setFont(bold);
    b->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 0px; }"));
    return b;
}

void connectSection(QToolButton* header, QWidget* body, const QString& settingsKey) {
    auto apply = [header, body](bool expanded) {
        header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(expanded);
    };
    const bool expanded = !Settings::instance().value(settingsKey).toBool();
    header->setChecked(expanded);
    apply(expanded);
    QObject::connect(header, &QToolButton::toggled, body, [apply, settingsKey](bool on) {
        apply(on);
        Settings::instance().setValue(settingsKey, !on);
    });
}

// Finder's standard tags (completion), in the UI language.
QStringList standardTags() {
    //: Finder standard tag names: use the names macOS shows in this language
    return {Tr::tr("Rot"),  Tr::tr("Orange"), Tr::tr("Gelb"), Tr::tr("Grün"),
            Tr::tr("Blau"), Tr::tr("Lila"),   Tr::tr("Grau")};
}

} // namespace

InfoData InfoPanel::load(const QString& path, const FileEntry& entry, TagService* tags) {
    InfoData data;
    data.path = path;
    const QFileInfo fi(path);

    InfoSection file{Tr::tr("Datei"), {}};
    file.rows.append({Tr::tr("Name"), fi.fileName()});
    file.rows.append({Tr::tr("Pfad"), fi.absolutePath()});
    const qint64 size = entry.size >= 0 ? entry.size : (fi.exists() ? fi.size() : -1);
    if (size >= 0)
        file.rows.append({Tr::tr("Größe"), formatSize(size)});
    // German type names ("JPEG-Bild"); the system's text (system language) only as fallback.
    QString kind = displayKind(fi.fileName(), entry.kind);
    if (kind.isEmpty())
        kind = QMimeDatabase().mimeTypeForFile(fi).comment();
    if (!kind.isEmpty())
        file.rows.append({Tr::tr("Art"), kind});
    const QDateTime created = entry.created.isValid() ? entry.created : fi.birthTime(QTimeZone::UTC);
    const QDateTime modified = entry.modified.isValid() ? entry.modified : fi.lastModified(QTimeZone::UTC);
    if (created.isValid())
        file.rows.append({Tr::tr("Erstellt"), formatDate(created)});
    if (modified.isValid())
        file.rows.append({Tr::tr("Geändert"), formatDate(modified)});
    if (entry.added.isValid())
        file.rows.append({Tr::tr("Hinzugefügt"), formatDate(entry.added)});
    data.sections.append(file);

    // F-FMT-06: a RAW+JPEG pair shown as one image - both files are described.
    if (!entry.pairedPath.isEmpty()) {
        const QFileInfo partner(entry.pairedPath);
        InfoSection pair{Tr::tr("RAW+JPEG-Paar"), {}};
        pair.rows.append({Tr::tr("Angezeigt"), fi.fileName()});
        pair.rows.append({Tr::tr("Zweite Datei"), partner.fileName()});
        if (partner.exists())
            pair.rows.append({Tr::tr("Größe der zweiten Datei"), formatSize(partner.size())});
        data.sections.append(pair);
    }

    // The decoders' "file" group describes the image.
    const Metadata md = DecoderRegistry::instance().metadata(path);
    if (md.latitude && md.longitude) // F-INFO-04
        data.location = GeoPoint{*md.latitude, *md.longitude};
    const std::pair<const char*, QString> groups[] = {
        {"file", Tr::tr("Bild")},
        {"camera", Tr::tr("Kamera")},
        {"shot", Tr::tr("Aufnahme")},
        {"gps", Tr::tr("GPS")},
    };
    bool hasImageRows = false;
    for (const auto& [group, title] : groups) {
        const auto rows = metadataRows(md, QString::fromLatin1(group));
        if (rows.isEmpty())
            continue;
        hasImageRows = hasImageRows || qstrcmp(group, "file") == 0;
        data.sections.append({title, rows});
    }
    if (!hasImageRows) {
        data.sections.append({Tr::tr("Bild"), {{Tr::tr("Format"), Tr::tr("Nicht lesbar")}}});
        data.unreadable = true;
    }

    if (tags && tags->isSupported()) {
        QString error;
        data.tags = tags->tags(path, &error);
        data.tagError = error;
    } else {
        data.tags = QStringList();
    }
    return data;
}

InfoPanel::InfoPanel(ImageProvider& provider, std::shared_ptr<TagService> tags, QWidget* parent)
    : QWidget(parent), m_provider(provider), m_tagService(std::move(tags)) {
    setObjectName(QStringLiteral("lfInfoPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("lfInfoTree"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(0);
    m_tree->setUniformRowHeights(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setFocusPolicy(Qt::NoFocus); // arrow keys keep flipping images
    // Dates, format names and paths stay readable from the start; the full value is the
    // tooltip (and "Wert kopieren").
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_tree->viewport()->setAutoFillBackground(false);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &InfoPanel::showContextMenu);
    // The metadata have priority: tags and histogram give way (shrink, collapse) first.
    m_tree->setMinimumHeight(m_tree->fontMetrics().height() * kMinTreeRows + 8);
    layout->addWidget(m_tree, 1);

    // ---- GPS (F-INFO-04) and all metadata (F-INFO-05) ----
    m_gps = new InfoGpsBox(createMapSnapshotProvider(), this);
    m_gps->setPanelVisible(false); // SPEC 10.3: maps only while the panel is shown
    m_gps->hide();
    layout->addWidget(m_gps);
    m_allMetadata = new QPushButton(this);
    m_allMetadata->setObjectName(QStringLiteral("lfAllMetadata"));
    m_allMetadata->setText(Tr::tr("Alle Metadaten …"));
    m_allMetadata->setFocusPolicy(Qt::NoFocus);
    m_allMetadata->setEnabled(false);
    connect(m_allMetadata, &QPushButton::clicked, this, &InfoPanel::showAllMetadata);
    layout->addWidget(m_allMetadata, 0, Qt::AlignLeft);

    // ---- Finder tags (F-INFO-06) ----
    m_tagBox = new QWidget(this);
    m_tagBox->setObjectName(QStringLiteral("lfTagBox"));
    auto* tl = new QVBoxLayout(m_tagBox);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(4);
    auto* head = new QHBoxLayout;
    QToolButton* title = makeSectionHeader(Tr::tr("Tags"), QStringLiteral("lfTagSection"), m_tagBox);
    m_tagRemove = new QToolButton(m_tagBox);
    m_tagRemove->setObjectName(QStringLiteral("lfTagRemove"));
    m_tagRemove->setText(QStringLiteral("−"));
    m_tagRemove->setToolTip(Tr::tr("Ausgewählten Tag entfernen"));
    m_tagRemove->setFocusPolicy(Qt::NoFocus);
    m_tagRemove->setAutoRaise(true);
    m_tagRemove->setEnabled(false);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(m_tagRemove);
    tl->addLayout(head);
    m_tagBody = new QWidget(m_tagBox);
    auto* tb = new QVBoxLayout(m_tagBody);
    tb->setContentsMargins(0, 0, 0, 0);
    tb->setSpacing(4);
    m_tagList = new QListWidget(m_tagBody);
    m_tagList->setObjectName(QStringLiteral("lfTagList"));
    m_tagList->setFlow(QListView::LeftToRight);
    m_tagList->setWrapping(true);
    m_tagList->setResizeMode(QListView::Adjust);
    m_tagList->setSpacing(3);
    m_tagList->setFocusPolicy(Qt::NoFocus);
    m_tagList->setFrameShape(QFrame::NoFrame);
    m_tagList->setFixedHeight(m_tagList->fontMetrics().height() * 2 + 16);
    m_tagList->hide(); // shown when the file has tags
    m_tagList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tagList->viewport()->setAutoFillBackground(false);
    tb->addWidget(m_tagList);
    m_tagEdit = new QLineEdit(m_tagBody);
    m_tagEdit->setObjectName(QStringLiteral("lfTagEdit"));
    m_tagEdit->setPlaceholderText(Tr::tr("Tag hinzufügen …"));
    m_tagEdit->setFocusPolicy(Qt::ClickFocus);
    m_tagEdit->setClearButtonEnabled(true);
    auto* completer = new QCompleter(standardTags(), m_tagEdit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_tagEdit->setCompleter(completer);
    m_tagEdit->installEventFilter(this);
    tb->addWidget(m_tagEdit);
    m_tagError = new QLabel(m_tagBody);
    m_tagError->setObjectName(QStringLiteral("lfTagError"));
    m_tagError->setWordWrap(true);
    m_tagError->setForegroundRole(QPalette::PlaceholderText);
    m_tagError->hide();
    tb->addWidget(m_tagError);
    tl->addWidget(m_tagBody);
    layout->addWidget(m_tagBox);
    m_tagBox->setVisible(m_tagService && m_tagService->isSupported());
    connectSection(title, m_tagBody, QStringLiteral("info/tagsCollapsed"));

    connect(m_tagEdit, &QLineEdit::returnPressed, this, [this] {
        const QString t = m_tagEdit->text().trimmed();
        m_tagEdit->clear();
        if (!t.isEmpty())
            addTag(t);
    });
    connect(m_tagList, &QListWidget::itemSelectionChanged, this,
            [this] { m_tagRemove->setEnabled(!m_tagList->selectedItems().isEmpty()); });
    connect(m_tagRemove, &QToolButton::clicked, this, [this] {
        const auto sel = m_tagList->selectedItems();
        if (!sel.isEmpty())
            removeTag(sel.first()->text());
    });
    connect(m_tagList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_tagList->itemAt(pos);
        if (!item)
            return;
        QMenu menu(this);
        const QString tag = item->text();
        menu.addAction(Tr::tr("„%1“ entfernen").arg(tag), this, [this, tag] { removeTag(tag); });
        menu.exec(m_tagList->viewport()->mapToGlobal(pos));
    });

    // ---- histogram (F-INFO-03) ----
    QToolButton* histTitle =
        makeSectionHeader(Tr::tr("Histogramm"), QStringLiteral("lfHistogramSection"), this);
    layout->addWidget(histTitle);
    m_histogram = new HistogramWidget(this);
    layout->addWidget(m_histogram);
    connectSection(histTitle, m_histogram, QStringLiteral("info/histogramCollapsed"));

    connect(&m_provider, &ImageProvider::invalidated, this, [this](const QString& p) {
        m_cache.remove(p);
        m_cacheOrder.removeAll(p);
        m_histograms.remove(p);
        m_histogramOrder.removeAll(p);
        if (p == m_path) {
            m_histogramPath.clear();
            startLoad(); // what is shown stays until the new data arrives
        }
    });
    connect(&m_provider, &ImageProvider::imageFailed, this, [this](const QString& p) {
        if (p == m_path && m_histogramPath != p)
            m_histogram->showNote(Tr::tr("Kein Histogramm")); // unreadable file (F-FMT-02)
    });
}

InfoPanel::~InfoPanel() = default;

void InfoPanel::setFile(const QString& path, const FileEntry* entry) {
    const bool changed = path != m_path;
    m_path = path;
    m_entry = entry ? *entry : FileEntry();
    if (!changed)
        return;
    m_tagError->hide();
    m_allMetadata->setEnabled(!path.isEmpty());
    if (m_metaDialog && !path.isEmpty())
        m_metaDialog->setPath(path); // F-INFO-05: the window follows the image
    if (path.isEmpty()) {
        m_gps->setLocation(std::nullopt);
        m_shownPath.clear();
        m_tree->clear();
        m_tags.clear();
        m_tagsPath.clear();
        m_tagList->clear();
        m_histogram->clear(false);
        m_histogramPath.clear();
        return;
    }
    auto cached = m_cache.constFind(path);
    if (cached != m_cache.cend())
        apply(*cached); // instant when flipping back; refreshed below
    else
        showPlaceholder();
    startLoad();
    refreshHistogram();
}

void InfoPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_gps->setPanelVisible(false);
}

void InfoPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_gps->setPanelVisible(true);
    if (!m_path.isEmpty() && m_shownPath != m_path && m_cache.contains(m_path))
        apply(m_cache.value(m_path));
    startLoad();
    refreshHistogram();
}

void InfoPanel::showPlaceholder() {
    // Name and folder are known at once; the rest follows from the worker.
    const QFileInfo fi(m_path);
    InfoSection file{Tr::tr("Datei"), {{Tr::tr("Name"), fi.fileName()}, {Tr::tr("Pfad"), fi.absolutePath()}}};
    if (m_entry.size >= 0)
        file.rows.append({Tr::tr("Größe"), formatSize(m_entry.size)});
    rebuildTree({file, InfoSection{Tr::tr("Bild"), {{Tr::tr("Wird geladen …"), QString()}}}});
    m_gps->setLocation(std::nullopt);
    m_shownPath.clear();
    m_tags.clear();
    m_tagsPath.clear();
    m_tagList->clear();
    m_tagList->hide();
    m_tagEdit->setEnabled(false);
}

void InfoPanel::startLoad() {
    if (m_path.isEmpty() || !isVisible() || m_loadRunning)
        return; // a running load restarts for the newest file when it is done
    m_loadRunning = true;
    m_loadingPath = m_path;
    const QString path = m_path;
    const FileEntry entry = m_entry;
    const std::shared_ptr<TagService> tags = m_tagService;
    const quint64 tagRevision = m_tagRevision.value(path);
    const int textEpoch = m_textEpoch;
    auto* w = new QFutureWatcher<InfoData>(this);
    connect(w, &QFutureWatcher<InfoData>::finished, this, [this, w, tagRevision, textEpoch] {
        InfoData data = w->result();
        w->deleteLater();
        m_loadRunning = false;
        m_loadingPath.clear();
        if (textEpoch != m_textEpoch) {
            // The language changed while this ran: its labels, values and dates are in the
            // old language. Neither cached nor shown; read again.
            startLoad();
            return;
        }
        const quint64 now = m_tagRevision.value(data.path);
        if (now != tagRevision) {
            // Tags were written meanwhile: what this read saw is stale. Use the stored
            // result of the write, or (still running) the tags shown for it.
            const auto written = m_writtenTags.constFind(data.path);
            if (written != m_writtenTags.cend() && written->revision == now) {
                data.tags = written->tags;
                data.tagError = written->error;
            } else if (data.path == m_tagsPath) {
                data.tags = m_tags;
                data.tagError.clear();
            }
        }
        if (!m_cache.contains(data.path))
            m_cacheOrder.append(data.path);
        m_cache.insert(data.path, data);
        while (m_cacheOrder.size() > kCacheEntries)
            m_cache.remove(m_cacheOrder.takeFirst());
        if (data.path == m_path) {
            apply(data);
            emit loaded(data.path);
        } else {
            startLoad(); // flipped meanwhile: only the newest file is read
        }
    });
    w->setFuture(
        QtConcurrent::run(infoPool(), [path, entry, tags] { return load(path, entry, tags.get()); }));
}

void InfoPanel::apply(const InfoData& data) {
    if (data.path != m_path)
        return;
    QList<InfoSection> current;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = m_tree->topLevelItem(i);
        InfoSection s{top->text(0), {}};
        for (int r = 0; r < top->childCount(); ++r)
            s.rows.append({top->child(r)->text(0), top->child(r)->text(1)});
        current.append(s);
    }
    if (current != data.sections || m_shownPath != data.path)
        rebuildTree(data.sections);
    m_gps->setLocation(data.location, QFileInfo(data.path).fileName()); // map only after a click
    m_shownPath = data.path;
    if (data.unreadable && m_histogramPath != data.path)
        m_histogram->showNote(Tr::tr("Kein Histogramm")); // whenever the failure was reported
    if (m_tagWrites == 0)
        applyTags(data.path, data.tags, data.tagError);
}

void InfoPanel::rebuildTree(const QList<InfoSection>& sections) {
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    for (const InfoSection& s : sections) {
        auto* top = new QTreeWidgetItem(m_tree, {s.title});
        top->setData(0, kSectionRole, true);
        top->setFlags(Qt::ItemIsEnabled);
        QFont f = m_tree->font();
        f.setBold(true);
        top->setFont(0, f);
        top->setFirstColumnSpanned(true);
        for (const auto& [key, value] : s.rows) {
            auto* item = new QTreeWidgetItem(top, {key, value});
            item->setToolTip(1, value);
            item->setForeground(0, palette().brush(QPalette::PlaceholderText));
        }
        top->setExpanded(true);
    }
    m_tree->setUpdatesEnabled(true);
}

bool InfoPanel::isLoading() const {
    return !m_path.isEmpty() && m_shownPath != m_path;
}

bool InfoPanel::isHistogramPending() const {
    return !m_path.isEmpty() && m_histogramPath != m_path;
}

bool InfoPanel::isTagWritePending() const {
    return m_tagWrites > 0;
}

QStringList InfoPanel::sectionTitles() const {
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        out << m_tree->topLevelItem(i)->text(0);
    return out;
}

QString InfoPanel::value(const QString& section, const QString& key) const {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (top->text(0) != section)
            continue;
        for (int r = 0; r < top->childCount(); ++r)
            if (top->child(r)->text(0) == key)
                return top->child(r)->text(1);
    }
    return {};
}

QString InfoPanel::copyText(const QTreeWidgetItem* item, bool wholeRow) {
    if (!item)
        return {};
    if (item->data(0, kSectionRole).toBool()) {
        QStringList lines{item->text(0)};
        for (int r = 0; r < item->childCount(); ++r)
            lines << item->child(r)->text(0) + QStringLiteral(": ") + item->child(r)->text(1);
        return lines.join(QLatin1Char('\n'));
    }
    return wholeRow ? item->text(0) + QStringLiteral(": ") + item->text(1) : item->text(1);
}

void InfoPanel::showContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    QMenu menu(this);
    auto copy = [](const QString& text) { QGuiApplication::clipboard()->setText(text); };
    if (item && !item->data(0, kSectionRole).toBool()) {
        m_tree->setCurrentItem(item);
        menu.addAction(Tr::tr("Wert kopieren"), this, [item, copy] { copy(copyText(item, false)); });
        menu.addAction(Tr::tr("Zeile kopieren"), this, [item, copy] { copy(copyText(item, true)); });
    } else if (item) {
        menu.addAction(Tr::tr("Abschnitt kopieren"), this, [item, copy] { copy(copyText(item, false)); });
    }
    menu.addAction(Tr::tr("Alle Angaben kopieren"), this, [this, copy] {
        QStringList parts;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            parts << copyText(m_tree->topLevelItem(i), false);
        copy(parts.join(QStringLiteral("\n\n")));
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ---- all metadata (F-INFO-05) ----

MetadataDialog* InfoPanel::metadataDialog() const {
    return m_metaDialog;
}

void InfoPanel::showAllMetadata() {
    if (m_path.isEmpty())
        return;
    if (!m_metaDialog) {
        m_metaDialog = new MetadataDialog(window());
        m_metaDialog->setPath(m_path);
    }
    m_metaDialog->show();
    m_metaDialog->raise();
    m_metaDialog->activateWindow();
}

// ---- tags (F-INFO-06) ----

QStringList InfoPanel::tags() const {
    return m_tags;
}

bool InfoPanel::tagsSupported() const {
    return m_tagService && m_tagService->isSupported();
}

void InfoPanel::applyTags(const QString& path, const std::optional<QStringList>& tags, const QString& error) {
    if (path != m_path)
        return;
    m_tagList->clear();
    m_tagsPath = tags ? path : QString();
    m_tags = tags.value_or(QStringList());
    m_tagList->addItems(m_tags);
    m_tagList->setVisible(!m_tags.isEmpty());
    m_tagEdit->setEnabled(tags.has_value());
    m_tagError->setText(error);
    m_tagError->setVisible(!error.isEmpty());
    m_tagRemove->setEnabled(false);
}

void InfoPanel::addTag(const QString& tag) {
    const QString t = tag.trimmed();
    if (t.isEmpty() || m_tags.contains(t, Qt::CaseInsensitive))
        return;
    writeTags(m_tags + QStringList{t});
}

void InfoPanel::removeTag(const QString& tag) {
    QStringList next = m_tags;
    if (next.removeAll(tag) > 0)
        writeTags(next);
}

void InfoPanel::writeTags(const QStringList& tags) {
    if (m_path.isEmpty() || m_tagsPath != m_path || !tagsSupported())
        return; // the current tags are not known yet
    // Shown at once; the worker writes and reads back what the system stored.
    m_tags = tags;
    m_tagList->clear();
    m_tagList->addItems(tags);
    m_tagList->setVisible(!tags.isEmpty());
    ++m_tagWrites;
    const quint64 revision = ++m_tagRevision[m_path];
    struct WriteResult {
        bool ok = false;
        QString error;
        std::optional<QStringList> stored;
    };
    const QString path = m_path;
    const std::shared_ptr<TagService> service = m_tagService;
    auto* w = new QFutureWatcher<WriteResult>(this);
    connect(w, &QFutureWatcher<WriteResult>::finished, this, [this, w, path, revision] {
        const WriteResult r = w->result();
        w->deleteLater();
        --m_tagWrites;
        m_writtenTags.insert(path, {revision, r.stored, r.ok ? QString() : r.error});
        auto it = m_cache.find(path);
        if (it != m_cache.end()) {
            it->tags = r.stored;
            it->tagError = r.ok ? QString() : r.error;
        }
        if (m_tagWrites == 0)
            applyTags(path, r.stored, r.ok ? QString() : r.error);
        emit tagsWritten(path, r.ok);
    });
    w->setFuture(QtConcurrent::run(tagWritePool(), [path, tags, service] {
        WriteResult r;
        r.ok = service->setTags(path, tags, &r.error);
        QString readError;
        r.stored = service->tags(path, &readError);
        if (r.error.isEmpty())
            r.error = readError;
        return r;
    }));
}

void InfoPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
}

void InfoPanel::retranslateUi() {
    for (auto* header : findChildren<QToolButton*>(QRegularExpression(QStringLiteral("^lf.*Section$"))))
        header->setToolTip(Tr::tr("Ein- oder ausklappen"));
    if (auto* tags = findChild<QToolButton*>(QStringLiteral("lfTagSection")))
        tags->setText(Tr::tr("Tags"));
    if (auto* hist = findChild<QToolButton*>(QStringLiteral("lfHistogramSection")))
        hist->setText(Tr::tr("Histogramm"));
    m_tagRemove->setToolTip(Tr::tr("Ausgewählten Tag entfernen"));
    m_tagEdit->setPlaceholderText(Tr::tr("Tag hinzufügen …"));
    m_allMetadata->setText(Tr::tr("Alle Metadaten …"));
    if (auto* c = m_tagEdit->completer())
        c->setModel(new QStringListModel(standardTags(), c));
    // Labels, values and dates were formatted for the old language: read them again (a load
    // running now is dropped when it finishes, see startLoad()).
    ++m_textEpoch;
    m_cache.clear();
    m_cacheOrder.clear();
    if (!m_path.isEmpty()) {
        showPlaceholder();
        startLoad();
    }
}

bool InfoPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tagEdit && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_tagEdit->clear();
        returnFocus();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void InfoPanel::returnFocus() {
    if (QWidget* w = window())
        w->setFocus(Qt::OtherFocusReason); // the window forwards focus to the image
}

// ---- histogram (F-INFO-03) ----

const Histogram& InfoPanel::histogram() const {
    return m_histogram->histogram();
}

void InfoPanel::imageShown(const QString& path, Quality quality) {
    if (path == m_path && quality != Quality::Preview)
        refreshHistogram();
}

void InfoPanel::refreshHistogram() {
    if (m_path.isEmpty() || !isVisible() || m_histogramPath == m_path)
        return;
    auto it = m_histograms.constFind(m_path);
    if (it != m_histograms.cend()) {
        m_histogram->setHistogram(*it);
        m_histogramPath = m_path;
        emit histogramReady(m_path);
        return;
    }
    m_histogram->clear(true);
    // The screen stage (or better) of the image, whatever size the window decoded it at;
    // a preview would give a coarse result (DecodedImage::quality, not isPreview).
    const DecodedImagePtr img = m_provider.cached(m_path, Quality::Preview);
    if (!img || img->quality == Quality::Preview || !m_histogramJob.isEmpty())
        return; // imageShown() / the running job come back here
    const QString path = m_path;
    m_histogramJob = path;
    auto* w = new QFutureWatcher<Histogram>(this);
    connect(w, &QFutureWatcher<Histogram>::finished, this, [this, w, path] {
        const Histogram h = w->result();
        w->deleteLater();
        m_histogramJob.clear();
        if (!m_histograms.contains(path))
            m_histogramOrder.append(path);
        m_histograms.insert(path, h);
        while (m_histogramOrder.size() > kHistogramEntries)
            m_histograms.remove(m_histogramOrder.takeFirst());
        refreshHistogram(); // shows it, or starts the one of the file shown now
    });
    w->setFuture(QtConcurrent::run(infoPool(), [img] { return computeHistogram(img->image); }));
}

} // namespace lf
