// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Platform abstraction. Every interface here has a macOS implementation in
// src/platform/mac and a portable fallback in src/platform/generic. The factory
// functions at the bottom are implemented by exactly one of the two.

#include "Decoder.h"
#include "FileEntry.h"
#include "SortSpec.h"

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>

namespace lf {

// Finder-compatible file name comparison (F-SORT-01).
class NameCollator {
public:
    virtual ~NameCollator() = default;
    // <0, 0, >0 like strcmp. Must be a strict weak ordering (forced ordering: only
    // byte-identical names compare equal).
    virtual int compare(const QString& a, const QString& b) const = 0;
};

// Determines the sort order Finder uses for a folder (SPEC 6.1, tiers 1-4).
class FolderOrderProvider {
public:
    virtual ~FolderOrderProvider() = default;
    // allowLiveQuery: tier 3 (AppleScript) is opt-in, F-SORT-04.
    virtual SortSpec finderOrder(const QString& folderPath, bool allowLiveQuery) = 0;
    // Icon positions (Iloc) for SortKey::FinderIconPosition, keyed by file name.
    virtual QHash<QString, QPointF> iconPositions(const QString& folderPath) = 0;
};

// Fills platform specific attributes (added date, localized kind) of entries.
class FileAttributeProvider {
public:
    virtual ~FileAttributeProvider() = default;
    virtual void fill(FileEntry& entry) = 0;
};

// Watches folders for changes (F-OPEN-07). Emits folderChanged for a watched path.
class FolderWatcher : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~FolderWatcher() override = default;
    virtual void watch(const QString& folderPath) = 0;
    virtual void unwatch(const QString& folderPath) = 0;
signals:
    void folderChanged(const QString& folderPath);
};

// Moves files to the trash and back (F-FILE-01).
class TrashService {
public:
    virtual ~TrashService() = default;
    // Returns the location inside the trash, or nullopt + error.
    virtual std::optional<QString> moveToTrash(const QString& path, QString* error) = 0;
    virtual bool restoreFromTrash(const QString& trashedPath, const QString& originalPath,
                                  QString* error) = 0;
};

// Misc OS integration (reveal in Finder etc.).
class Shell {
public:
    virtual ~Shell() = default;
    virtual void revealInFileManager(const QString& path) = 0;
    virtual void openWith(const QString& path, const QString& appPath) = 0;  // empty = default app
    virtual QStringList imageDocumentTypes() const = 0;  // UTTypes we register for
    virtual bool makeDefaultViewer(const QStringList& types, QString* error) = 0;  // F-MAC-04
};

// ---- factories (implemented per platform) ----
std::unique_ptr<NameCollator> createNameCollator();
std::unique_ptr<FolderOrderProvider> createFolderOrderProvider();
std::unique_ptr<FileAttributeProvider> createFileAttributeProvider();
std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent = nullptr);
std::unique_ptr<TrashService> createTrashService();
std::unique_ptr<Shell> createShell();
// Registers all platform decoder backends into the registry (macOS: ImageIO).
void registerPlatformDecoders(class DecoderRegistry& registry);

} // namespace lf
