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
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace lf {

// Names prepared for many comparisons, e.g. for one sort (F-SORT-01, P-05).
class PreparedNames {
public:
    virtual ~PreparedNames() = default;
    // Same result as NameCollator::compare(names[i], names[j]).
    virtual int compare(qsizetype i, qsizetype j) const = 0;
};

// Finder-compatible file name comparison (F-SORT-01).
class NameCollator {
public:
    virtual ~NameCollator() = default;
    // <0, 0, >0 like strcmp. Must be a strict weak ordering (forced ordering: only
    // byte-identical names compare equal).
    virtual int compare(const QString& a, const QString& b) const = 0;
    // Prepares `names` once (platform collators keep per-name data such as a CFString);
    // the default compares the strings with compare() each time.
    virtual std::unique_ptr<PreparedNames> prepare(QVector<QString> names) const;
};

// Determines the sort order the file manager uses for a folder (SPEC 6.1, tiers 1-4; on
// Windows the Explorer window showing the folder).
class FolderOrderProvider {
public:
    virtual ~FolderOrderProvider() = default;
    // allowLiveQuery: ask the file manager's window (F-SORT-04): macOS tier 3 (AppleScript,
    // opt-in), Windows the Explorer window via COM (on by default, key::FinderLiveQuery).
    virtual SortSpec finderOrder(const QString& folderPath, bool allowLiveQuery) = 0;
    // Icon positions (Iloc) for SortKey::FinderIconPosition, keyed by file name.
    virtual QHash<QString, QPointF> iconPositions(const QString& folderPath) = 0;
};

// Fills platform specific attributes (added date, localized kind) of entries.
class FileAttributeProvider {
public:
    virtual ~FileAttributeProvider() = default;
    virtual void fill(FileEntry& entry) = 0;
    // Reads image metadata needed for sorting (F-SORT-02): capture date (EXIF
    // DateTimeOriginal), pixel count and the embedded rating, without decoding pixels.
    // Called on a worker thread, only when the active sort key needs these values.
    // Implementations set entry.imagePropertiesRead. The default does nothing.
    virtual void fillImageProperties(FileEntry& entry) { entry.imagePropertiesRead = true; }
};

// Watches folders for changes (F-OPEN-07). Emits folderChanged for a watched path.
class FolderWatcher : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~FolderWatcher() override = default;
    virtual void watch(const QString& folderPath) = 0;
    virtual void unwatch(const QString& folderPath) = 0;
    // F-OPEN-08: `root` and every folder below it (also folders created later);
    // folderChanged() names the folder whose contents changed (or `root` if the system
    // dropped events). Default: only `root` itself (platforms without recursive watching).
    virtual void watchTree(const QString& root) { watch(root); }
    virtual void unwatchTree(const QString& root) { unwatch(root); }
    // True while watchTree() still sets up its watches in the background (Linux walks the
    // tree on a worker thread); changes in folders not reached yet may go unnoticed until
    // then. For tests.
    virtual bool isSettingUp() const { return false; }
signals:
    void folderChanged(const QString& folderPath);
    // An XMP sidecar ("*.xmp") was written, e.g. by this app's own rating: reported apart
    // from folderChanged (it does not change the image list; code review M3 #16).
    void sidecarChanged(const QString& sidecarPath);
    // watchTree(): the system dropped events below `root`; the whole tree must be read again.
    void rescanNeeded(const QString& root);
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

// Finder tags of a file (F-INFO-06; later also ratings as tags, F-CULL-02). Blocking file
// system calls: use on worker threads. Implementations are stateless and thread-safe.
class TagService {
public:
    virtual ~TagService() = default;
    // False if the platform has no file tags (the info panel then hides the section).
    virtual bool isSupported() const = 0;
    // Tag names in the order the system stores them; nullopt + error if unreadable.
    virtual std::optional<QStringList> tags(const QString& path, QString* error) = 0;
    // Replaces all tags of `path`.
    virtual bool setTags(const QString& path, const QStringList& tags, QString* error) = 0;
};

// F-MAC-09: whether this app opens files of a type by default (Shell::defaultViewerStatus).
struct DefaultViewerStatus {
    enum class State {
        Unknown,    // not determined (query failed, type not ours, platform cannot tell)
        IsDefault,  // this app is the default
        NotDefault, // another app (or none) is the default
    };
    State state = State::Unknown;
    QString type;     // what makeDefaultViewer() takes for it (UTType, MIME type)
    QString typeName; // the system's localized description, e.g. "JPEG-Bild"
};

// Misc OS integration (reveal in Finder etc.).
class Shell {
public:
    Shell();
    virtual ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;
    virtual void revealInFileManager(const QString& path) = 0;
    virtual void openWith(const QString& path, const QString& appPath) = 0;       // empty = default app
    virtual QStringList imageDocumentTypes() const = 0;                           // UTTypes we register for
    virtual bool makeDefaultViewer(const QStringList& types, QString* error) = 0; // F-MAC-04 (blocking)
    // Non-blocking variant for the UI: `done` runs once with the final result on the thread
    // of `context` (not at all if `context` is gone). If the system takes long (it may be
    // waiting for the user to confirm a system dialog), `pending` (optional) is called
    // first with a message for the user; the final result still follows. The default
    // implementation runs makeDefaultViewer() on a worker thread.
    using DefaultViewerDone = std::function<void(bool ok, const QString& error)>;
    using DefaultViewerPending = std::function<void(const QString& message)>;
    virtual void makeDefaultViewerAsync(const QStringList& types, QObject* context, DefaultViewerDone done,
                                        DefaultViewerPending pending = {});
    // F-MAC-09 (blocking, worker threads only - LaunchServices may be slow): the type of
    // `filePath` and whether this app is its default. Default: Unknown (never offered).
    virtual DefaultViewerStatus defaultViewerStatus(const QString& filePath);
    // F-MAC-09 (blocking): runs as the installed app (macOS: bundle in /Applications or
    // ~/Applications on a writable volume; not from a build tree or a disk image), so
    // offering to become the default makes sense. Default: false.
    virtual bool isInstalledApp();
    // Blocks until worker threads started by the default makeDefaultViewerAsync() are done.
    // Whoever owns the shell calls this before destroying it: those threads call
    // makeDefaultViewer() on it.
    void waitForPendingCalls();

private:
    struct AsyncCalls;
    std::unique_ptr<AsyncCalls> m_async;
};

// F-WIN-06: the system wants windows restored at the next launch (macOS: "Fenster beim
// Beenden schließen" off, i.e. NSQuitAlwaysKeepsWindows; macOS default: false).
bool systemRestoresWindows();

// F-I18N-03: the languages chosen for this app only (macOS: System Settings → General →
// Language & Region → Applications, i.e. AppleLanguages in the app's own defaults domain),
// most preferred first; empty if none (then the system's preferred languages apply). A
// value Lumaflip wrote itself (setAppKitLanguage()) is not a per-app choice: empty then.
// `domain`: defaults domain to read (empty = the app's own; tests use their own).
QStringList appLanguagePreference(const QString& domain = QString());

// F-I18N-03: an explicit language choice in Lumaflip also goes to AppleLanguages in the
// app's defaults domain (as ["<tag>"], appleLanguageTag()), so the UI AppKit builds itself
// - "Enter Full Screen", Services, open/save panels, privacy prompts (InfoPlist.strings) -
// follows after a restart. `code` empty or "system" removes what Lumaflip wrote before (a
// per-app language set in System Settings stays). Returns false if the defaults could not
// be written; no-op (true) outside macOS. `domain` as above.
bool setAppKitLanguage(const QString& code, const QString& domain = QString());

// ---- factories (implemented per platform) ----
// The file manager whose order "Wie im Finder" follows (SortSpec.h): macOS Finder, Windows
// Explorer, otherwise FileManager::Other.
FileManager platformFileManager();
std::unique_ptr<NameCollator> createNameCollator();
std::unique_ptr<FolderOrderProvider> createFolderOrderProvider();
std::unique_ptr<FileAttributeProvider> createFileAttributeProvider();
std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent = nullptr);
std::unique_ptr<TrashService> createTrashService();
std::unique_ptr<Shell> createShell();
std::unique_ptr<TagService> createTagService(); // F-INFO-06
// Registers all platform decoder backends into the registry (macOS: ImageIO).
void registerPlatformDecoders(class DecoderRegistry& registry);
// The highest-priority platform decoder alone (macOS: ImageIO), usable before the
// application object exists (ImageProvider::prestart, P-01); null if there is none.
std::unique_ptr<Decoder> createPrimaryDecoder();

} // namespace lf
