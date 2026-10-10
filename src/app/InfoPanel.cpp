// SPDX-License-Identifier: GPL-3.0-or-later
#include "InfoPanel.h"

#include "DisplayText.h"
#include "HistogramWidget.h"
#include "core/Decoder.h"
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

QString tr(const char* text) {
    return QCoreApplication::translate("lf::InfoPanel", text);
}

// Metadata reads (any number of windows) share two low-priority threads; parented to the
// application so they are joined before Qt shuts down.
QThreadPool* infoPool() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool(QCoreApplication::instance());
        p->setMaxThreadCount(2);
        p->setThreadPriority(QThread::LowPriority);
        return p;
    }();
    return pool;
}

// Tag writes run one after another (the order of changes must be kept).
QThreadPool* tagWritePool() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool(QCoreApplication::instance());
        p->setMaxThreadCount(1);
        return p;
    }();
    return pool;
}

QString formatDate(const QDateTime& dt) {
    return dt.isValid() ? dt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")) : QString();
}

QString formatSize(qint64 bytes) {
    return formatByteSizeExact(bytes); // German units like the rest of the UI
}

// The decoders deliver stable German keys (Decoder.h); the labels shown are translatable.
// Unknown keys are shown as they are.
const char* const kRowLabels[] = {
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Format"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Pixelmaße"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Farbraum"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Profil"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Bittiefe"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "HDR"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Transparenz"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Einzelbilder"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Seiten"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Ausrichtung"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Bewertung"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Hersteller"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Modell"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Objektiv"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Aufnahmedatum"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Brennweite"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Blende"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Belichtungszeit"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "ISO"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Belichtungskorrektur"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Blitz"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Breite"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Länge"),
    QT_TRANSLATE_NOOP("lf::InfoPanel", "Höhe"),
};

QString rowLabel(const QString& key) {
    for (const char* k : kRowLabels)
        if (key == QString::fromUtf8(k))
            return tr(k);
    return key;
}

// Rows of a decoder group in a fixed, meaningful order; unknown keys follow alphabetically.
QList<QPair<QString, QString>> orderedRows(const QVariantMap& group, const QStringList& order) {
    QList<QPair<QString, QString>> rows;
    for (const QString& k : order)
        if (group.contains(k))
            rows.append({rowLabel(k), group.value(k).toString()});
    for (auto it = group.cbegin(); it != group.cend(); ++it)
        if (!order.contains(it.key()))
            rows.append({rowLabel(it.key()), it.value().toString()});
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
    b->setToolTip(QCoreApplication::translate("lf::InfoPanel", "Ein- oder ausklappen"));
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

const QStringList kStandardTags = {tr("Rot"),  tr("Orange"), tr("Gelb"), tr("Grün"),
                                   tr("Blau"), tr("Lila"),   tr("Grau")};

} // namespace

InfoData InfoPanel::load(const QString& path, const FileEntry& entry, TagService* tags) {
    InfoData data;
    data.path = path;
    const QFileInfo fi(path);

    InfoSection file{tr("Datei"), {}};
    file.rows.append({tr("Name"), fi.fileName()});
    file.rows.append({tr("Pfad"), fi.absolutePath()});
    const qint64 size = entry.size >= 0 ? entry.size : (fi.exists() ? fi.size() : -1);
    if (size >= 0)
        file.rows.append({tr("Größe"), formatSize(size)});
    // German type names ("JPEG-Bild"); the system's text (system language) only as fallback.
    QString kind = displayKind(fi.fileName(), entry.kind);
    if (kind.isEmpty())
        kind = QMimeDatabase().mimeTypeForFile(fi).comment();
    if (!kind.isEmpty())
        file.rows.append({tr("Art"), kind});
    const QDateTime created = entry.created.isValid() ? entry.created : fi.birthTime(QTimeZone::UTC);
    const QDateTime modified = entry.modified.isValid() ? entry.modified : fi.lastModified(QTimeZone::UTC);
    if (created.isValid())
        file.rows.append({tr("Erstellt"), formatDate(created)});
    if (modified.isValid())
        file.rows.append({tr("Geändert"), formatDate(modified)});
    if (entry.added.isValid())
        file.rows.append({tr("Hinzugefügt"), formatDate(entry.added)});
    data.sections.append(file);

    // The decoders deliver German groups (Decoder.h): their "Datei" group describes the image.
    const Metadata md = DecoderRegistry::instance().metadata(path);
    struct Group {
        const char* source;
        QString title;
        QStringList order;
    };
    const Group groups[] = {
        {"Datei",
         tr("Bild"),
         {QStringLiteral("Format"), QStringLiteral("Pixelmaße"), QStringLiteral("Farbraum"),
          QStringLiteral("Profil"), QStringLiteral("Bittiefe"), QStringLiteral("HDR"),
          QStringLiteral("Transparenz"), QStringLiteral("Einzelbilder"), QStringLiteral("Seiten"),
          QStringLiteral("Ausrichtung"), QStringLiteral("Bewertung")}},
        {"Kamera",
         tr("Kamera"),
         {QStringLiteral("Hersteller"), QStringLiteral("Modell"), QStringLiteral("Objektiv")}},
        {"Aufnahme",
         tr("Aufnahme"),
         {QStringLiteral("Aufnahmedatum"), QStringLiteral("Brennweite"), QStringLiteral("Blende"),
          QStringLiteral("Belichtungszeit"), QStringLiteral("ISO"), QStringLiteral("Belichtungskorrektur"),
          QStringLiteral("Blitz")}},
        {"GPS", tr("GPS"), {QStringLiteral("Breite"), QStringLiteral("Länge"), QStringLiteral("Höhe")}},
    };
    for (const Group& g : groups) {
        const QVariantMap m = md.groups.value(QString::fromLatin1(g.source)).toMap();
        if (!m.isEmpty())
            data.sections.append({g.title, orderedRows(m, g.order)});
    }
    if (md.groups.value(QStringLiteral("Datei")).toMap().isEmpty())
        data.sections.append({tr("Bild"), {{tr("Format"), tr("Nicht lesbar")}}});

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

    // ---- Finder tags (F-INFO-06) ----
    m_tagBox = new QWidget(this);
    m_tagBox->setObjectName(QStringLiteral("lfTagBox"));
    auto* tl = new QVBoxLayout(m_tagBox);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(4);
    auto* head = new QHBoxLayout;
    QToolButton* title = makeSectionHeader(tr("Tags"), QStringLiteral("lfTagSection"), m_tagBox);
    m_tagRemove = new QToolButton(m_tagBox);
    m_tagRemove->setObjectName(QStringLiteral("lfTagRemove"));
    m_tagRemove->setText(QStringLiteral("−"));
    m_tagRemove->setToolTip(tr("Ausgewählten Tag entfernen"));
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
    m_tagEdit->setPlaceholderText(tr("Tag hinzufügen …"));
    m_tagEdit->setFocusPolicy(Qt::ClickFocus);
    m_tagEdit->setClearButtonEnabled(true);
    auto* completer = new QCompleter(kStandardTags, m_tagEdit);
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
        menu.addAction(tr("„%1“ entfernen").arg(tag), this, [this, tag] { removeTag(tag); });
        menu.exec(m_tagList->viewport()->mapToGlobal(pos));
    });

    // ---- histogram (F-INFO-03) ----
    QToolButton* histTitle = makeSectionHeader(tr("Histogramm"), QStringLiteral("lfHistogramSection"), this);
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
            m_histogram->showNote(tr("Kein Histogramm")); // unreadable file (F-FMT-02)
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
    if (path.isEmpty()) {
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

void InfoPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_path.isEmpty() && m_shownPath != m_path && m_cache.contains(m_path))
        apply(m_cache.value(m_path));
    startLoad();
    refreshHistogram();
}

void InfoPanel::showPlaceholder() {
    // Name and folder are known at once; the rest follows from the worker.
    const QFileInfo fi(m_path);
    InfoSection file{tr("Datei"), {{tr("Name"), fi.fileName()}, {tr("Pfad"), fi.absolutePath()}}};
    if (m_entry.size >= 0)
        file.rows.append({tr("Größe"), formatSize(m_entry.size)});
    rebuildTree({file, InfoSection{tr("Bild"), {{tr("Wird geladen …"), QString()}}}});
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
    auto* w = new QFutureWatcher<InfoData>(this);
    connect(w, &QFutureWatcher<InfoData>::finished, this, [this, w, tagRevision] {
        InfoData data = w->result();
        w->deleteLater();
        m_loadRunning = false;
        m_loadingPath.clear();
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
    m_shownPath = data.path;
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
        menu.addAction(tr("Wert kopieren"), this, [item, copy] { copy(copyText(item, false)); });
        menu.addAction(tr("Zeile kopieren"), this, [item, copy] { copy(copyText(item, true)); });
    } else if (item) {
        menu.addAction(tr("Abschnitt kopieren"), this, [item, copy] { copy(copyText(item, false)); });
    }
    menu.addAction(tr("Alle Angaben kopieren"), this, [this, copy] {
        QStringList parts;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            parts << copyText(m_tree->topLevelItem(i), false);
        copy(parts.join(QStringLiteral("\n\n")));
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
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
    // The screen stage (or better) of the image; a preview would give a coarse result.
    const DecodedImagePtr img = m_provider.cached(m_path, Quality::Screen);
    if (!img || img->isPreview || !m_histogramJob.isEmpty())
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
