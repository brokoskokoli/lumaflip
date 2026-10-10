// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Info panel (F-INFO-02): file, image, camera, exposure and GPS data of the current
// image, its histogram (F-INFO-03) and Finder tags (F-INFO-06). Lives in a side panel
// that stays open while flipping. Everything is read on worker threads; the panel shows
// placeholders until the data of the current file arrives. Loads only while visible.

#include "core/FileEntry.h"
#include "core/Histogram.h"
#include "core/ImageProvider.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lf {

class HistogramWidget;
class TagService;

// One titled group of "key: value" rows.
struct InfoSection {
    QString title;
    QList<QPair<QString, QString>> rows;
    friend bool operator==(const InfoSection&, const InfoSection&) = default;
};

// Result of a background load for one file.
struct InfoData {
    QString path;
    QList<InfoSection> sections;
    std::optional<QStringList> tags; // nullopt: unreadable (tagError)
    QString tagError;
};

class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(ImageProvider& provider, std::shared_ptr<TagService> tags, QWidget* parent = nullptr);
    ~InfoPanel() override;

    // The file to describe. `entry` (from the collection, may be null) gives what is
    // already known without file system access. Loading starts when the panel is visible.
    void setFile(const QString& path, const FileEntry* entry = nullptr);
    QString path() const { return m_path; }
    // A stage of `path` was shown: the histogram is computed from the screen stage.
    void imageShown(const QString& path, Quality quality);

    // Reads everything shown for `path` (blocking, worker threads only). Section order:
    // Datei, Bild, Kamera, Aufnahme, GPS (empty sections are left out).
    static InfoData load(const QString& path, const FileEntry& entry, TagService* tags);

    // State and contents (tests, accessibility).
    bool isLoading() const;            // metadata of the current file not shown yet
    bool isHistogramPending() const;   // histogram of the current file not shown yet
    bool isTagWritePending() const;    // a tag change is being written
    QStringList sectionTitles() const; // shown sections
    QString value(const QString& section, const QString& key) const;
    QStringList tags() const;
    bool tagsSupported() const;
    void addTag(const QString& tag);
    void removeTag(const QString& tag);
    const Histogram& histogram() const;
    QTreeWidget* tree() const { return m_tree; }
    HistogramWidget* histogramWidget() const { return m_histogram; }
    // Text for the clipboard: the value of `item`, or "key: value" lines of a section.
    static QString copyText(const QTreeWidgetItem* item, bool wholeRow);

signals:
    void loaded(const QString& path);
    void histogramReady(const QString& path);
    void tagsWritten(const QString& path, bool ok);

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void startLoad();
    void apply(const InfoData& data);
    void showPlaceholder();
    void rebuildTree(const QList<InfoSection>& sections);
    void applyTags(const QString& path, const std::optional<QStringList>& tags, const QString& error);
    void writeTags(const QStringList& tags);
    void refreshHistogram();
    void showContextMenu(const QPoint& pos);
    void returnFocus();

    ImageProvider& m_provider;
    std::shared_ptr<TagService> m_tagService;
    QString m_path;
    FileEntry m_entry;
    QString m_shownPath; // metadata shown for this path
    bool m_loadRunning = false;
    QString m_loadingPath; // job in flight for this path
    QHash<QString, InfoData> m_cache;
    QStringList m_cacheOrder;

    QHash<QString, Histogram> m_histograms;
    QStringList m_histogramOrder;
    QString m_histogramPath; // histogram shown for this path
    QString m_histogramJob;  // computing for this path

    QStringList m_tags;
    QString m_tagsPath;
    int m_tagWrites = 0;

    QTreeWidget* m_tree = nullptr;
    HistogramWidget* m_histogram = nullptr;
    QWidget* m_tagBox = nullptr;
    QListWidget* m_tagList = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QToolButton* m_tagRemove = nullptr;
    QLabel* m_tagError = nullptr;
};

} // namespace lf
