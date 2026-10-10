// SPDX-License-Identifier: GPL-3.0-or-later
#include "MetadataDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

namespace {

const int kPathRole = Qt::UserRole + 1; // key path of an item ("Exif/FNumber")

void addItems(QTreeWidgetItem* parent, QTreeWidget* view, const MetadataNode& node,
              const QString& parentPath) {
    for (const MetadataNode& c : node.children) {
        const QString keyPath = parentPath.isEmpty() ? c.key : parentPath + u'/' + c.key;
        auto* item = parent ? new QTreeWidgetItem(parent, {c.key, c.value})
                            : new QTreeWidgetItem(view, {c.key, c.value});
        item->setData(0, kPathRole, keyPath);
        item->setToolTip(1, c.value);
        if (c.isGroup()) {
            QFont f = view->font();
            f.setBold(true);
            item->setFont(0, f);
            addItems(item, view, c, keyPath);
        }
    }
}

} // namespace

MetadataDialog::MetadataDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("lfMetadataDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(560, 640);
    auto* layout = new QVBoxLayout(this);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("lfMetadataSearch"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_view = new QTreeWidget(this);
    m_view->setObjectName(QStringLiteral("lfMetadataTree"));
    m_view->setColumnCount(2);
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_view->header()->resizeSection(0, 220);
    m_view->header()->setStretchLastSection(true);
    m_view->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_view, 1);
    auto* row = new QHBoxLayout;
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("lfMetadataCount"));
    m_count->setForegroundRole(QPalette::PlaceholderText);
    m_copy = new QPushButton(this);
    m_copy->setObjectName(QStringLiteral("lfMetadataCopy"));
    m_copyAll = new QPushButton(this);
    m_copyAll->setObjectName(QStringLiteral("lfMetadataCopyAll"));
    m_copyAll->setAutoDefault(false);
    m_copy->setAutoDefault(false);
    row->addWidget(m_count, 1);
    row->addWidget(m_copy);
    row->addWidget(m_copyAll);
    layout->addLayout(row);

    connect(m_search, &QLineEdit::textChanged, this, &MetadataDialog::rebuild);
    connect(m_copy, &QPushButton::clicked, this, &MetadataDialog::copySelection);
    connect(m_copyAll, &QPushButton::clicked, this, &MetadataDialog::copyAll);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MetadataDialog::showContextMenu);
    connect(m_view, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_copy->setEnabled(!m_view->selectedItems().isEmpty()); });
    retranslateUi();
}

void MetadataDialog::setPath(const QString& path) {
    if (path == m_path)
        return;
    m_path = path;
    setWindowTitle(tr("Alle Metadaten – %1").arg(QFileInfo(path).fileName()));
    const quint64 token = ++m_token;
    m_view->clear();
    m_count->setText(tr("Wird geladen …"));
    auto* w = new QFutureWatcher<MetadataNode>(this);
    connect(w, &QFutureWatcher<MetadataNode>::finished, this, [this, w, token, path] {
        w->deleteLater();
        if (token != m_token)
            return; // another image meanwhile
        m_tree = w->result();
        m_loadedPath = path;
        rebuild();
        emit loaded(path);
    });
    w->setFuture(QtConcurrent::run([path] { return readRawMetadata(path); }));
}

void MetadataDialog::rebuild() {
    m_shown = metadatatree::filtered(m_tree, m_search->text());
    m_view->setUpdatesEnabled(false);
    m_view->clear();
    addItems(nullptr, m_view, m_shown, QString());
    const bool searching = !m_search->text().trimmed().isEmpty();
    if (searching)
        m_view->expandAll(); // show where the matches are
    else
        for (int i = 0; i < m_view->topLevelItemCount(); ++i)
            m_view->topLevelItem(i)->setExpanded(true); // groups open, their sub-trees closed
    m_view->setUpdatesEnabled(true);
    if (m_loadedPath != m_path)
        return;
    const int total = metadatatree::leafCount(m_tree);
    if (m_tree.children.isEmpty())
        m_count->setText(tr("Keine Metadaten gefunden."));
    else if (searching)
        m_count->setText(tr("%1 von %n Einträgen", nullptr, total)
                             .arg(m_shown.children.isEmpty() ? 0 : metadatatree::leafCount(m_shown)));
    else
        m_count->setText(tr("%n Einträge", nullptr, total));
    m_copy->setEnabled(!m_view->selectedItems().isEmpty());
}

QString MetadataDialog::selectionText() const {
    const auto sel = m_view->selectedItems();
    if (sel.isEmpty())
        return {};
    const QString keyPath = sel.first()->data(0, kPathRole).toString();
    if (const MetadataNode* n = metadatatree::find(m_shown, keyPath))
        return metadatatree::toText(*n);
    return {};
}

QString MetadataDialog::allText() const {
    return metadatatree::toText(m_shown, false);
}

void MetadataDialog::copySelection() {
    const QString text = selectionText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void MetadataDialog::copyAll() {
    QGuiApplication::clipboard()->setText(allText());
}

void MetadataDialog::showContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_view->itemAt(pos);
    QMenu menu(this);
    if (item) {
        m_view->setCurrentItem(item);
        if (item->childCount() == 0)
            menu.addAction(tr("Wert kopieren"), this,
                           [item] { QGuiApplication::clipboard()->setText(item->text(1)); });
        menu.addAction(item->childCount() ? tr("Gruppe kopieren") : tr("Zeile kopieren"), this,
                       &MetadataDialog::copySelection);
    }
    menu.addAction(tr("Alles kopieren"), this, &MetadataDialog::copyAll);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void MetadataDialog::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy) && !m_search->hasFocus()) {
        copySelection();
        return;
    }
    if (event->matches(QKeySequence::Find)) {
        m_search->setFocus();
        m_search->selectAll();
        return;
    }
    QDialog::keyPressEvent(event);
}

void MetadataDialog::retranslateUi() {
    m_search->setPlaceholderText(tr("Suchen (Feld oder Wert) …"));
    m_view->setHeaderLabels({tr("Feld"), tr("Wert")});
    m_copy->setText(tr("Kopieren"));
    m_copyAll->setText(tr("Alles kopieren"));
    if (!m_path.isEmpty())
        setWindowTitle(tr("Alle Metadaten – %1").arg(QFileInfo(m_path).fileName()));
    else
        setWindowTitle(tr("Alle Metadaten"));
    rebuild();
}

void MetadataDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
}

} // namespace lf
