// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-05: all raw metadata of the current image (EXIF, TIFF, GPS, IPTC, maker notes,
// XMP …) as an expandable tree with search and copy. A non-modal window per viewer window
// that follows the image shown in the info panel. Read on a worker thread.

#include "core/MetadataTree.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lf {

class MetadataDialog : public QDialog {
    Q_OBJECT
public:
    explicit MetadataDialog(QWidget* parent = nullptr);

    void setPath(const QString& path); // reads the metadata of `path` (asynchronously)
    QString path() const { return m_path; }
    bool isLoading() const { return !m_path.isEmpty() && m_loadedPath != m_path; }
    const MetadataNode& tree() const { return m_tree; }

    QLineEdit* searchField() const { return m_search; }
    QTreeWidget* view() const { return m_view; }
    // Clipboard text of the selected row/group, or of the whole (filtered) tree.
    QString selectionText() const;
    QString allText() const;
    void copySelection();
    void copyAll();

signals:
    void loaded(const QString& path);

protected:
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    void retranslateUi();
    void showContextMenu(const QPoint& pos);

    QString m_path;
    QString m_loadedPath;
    quint64 m_token = 0;
    MetadataNode m_tree;
    MetadataNode m_shown; // filtered
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_view = nullptr;
    QLabel* m_count = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_copyAll = nullptr;
};

} // namespace lf
