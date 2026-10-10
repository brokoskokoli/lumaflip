// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Info panel (F-INFO-02): file, image, camera, exposure and GPS data of the current
// image, its histogram (F-INFO-03) and Finder tags (F-INFO-06). Lives in a side panel
// that stays open while flipping. Everything is read on worker threads; the panel shows
// placeholders until the data of the current file arrives. Loads only while visible.

#include "core/FileEntry.h"
#include "core/GeoLocation.h"
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
class QPushButton;
class QListWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lf {

class HistogramWidget;
class InfoGpsBox;     // F-INFO-04
class MetadataDialog; // F-INFO-05
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
    bool unreadable = false;          // no decoder can read the image (no histogram either)
    std::optional<GeoPoint> location; // F-INFO-04
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
    // F-INFO-04: GPS position, mini map (after a click) and "In Karten öffnen".
    InfoGpsBox* gpsBox() const { return m_gps; }
    // F-INFO-05: "Alle Metadaten …" opens the raw metadata window (follows the image).
    QPushButton* allMetadataButton() const { return m_allMetadata; }
    MetadataDialog* metadataDialog() const;
    void showAllMetadata();
    HistogramWidget* histogramWidget() const { return m_histogram; }
    // Text for the clipboard: the value of `item`, or "key: value" lines of a section.
    static QString copyText(const QTreeWidgetItem* item, bool wholeRow);

signals:
    void loaded(const QString& path);
    void histogramReady(const QString& path);
    void tagsWritten(const QString& path, bool ok);
    void folderMapRequested(); // F-INFO-08: "Ordnerkarte" in the GPS box

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override; // SPEC 10.3: no map requests while hidden
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override; // F-I18N-03: language switch

private:
    void retranslateUi();
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
    // Per path: tag writes started so far. A metadata read that began before a write
    // returns stale tags; they must not replace what the write stored (review #13).
    QHash<QString, quint64> m_tagRevision;
    struct WrittenTags {
        quint64 revision = 0;
        std::optional<QStringList> tags;
        QString error;
    };
    QHash<QString, WrittenTags> m_writtenTags; // result of the latest finished write

    QTreeWidget* m_tree = nullptr;
    HistogramWidget* m_histogram = nullptr;
    QWidget* m_tagBox = nullptr;
    QWidget* m_tagBody = nullptr; // collapsible part of the tag section
    QListWidget* m_tagList = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QToolButton* m_tagRemove = nullptr;
    QLabel* m_tagError = nullptr;
    InfoGpsBox* m_gps = nullptr;
    QPushButton* m_allMetadata = nullptr;
    QPointer<MetadataDialog> m_metaDialog;
};

} // namespace lf
