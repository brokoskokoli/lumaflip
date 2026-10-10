// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where ratings live (F-CULL-02): (a) XMP sidecar next to the image, (b) XMP embedded in
// the image (JPEG, HEIC, TIFF, PNG, DNG - metadata only, no pixel re-encoding), (c) Finder
// tags. Several stores may be enabled; all enabled ones are written. When reading, the
// first enabled store in the order sidecar -> embedded -> Finder tags that has a rating
// wins (SPEC: "bei Widerspruch gewinnt die zuerst gewählte Quelle").
//
// Formats that cannot be written safely (camera RAW, animations, multi-page files, types
// ImageIO cannot write losslessly) fall back to the sidecar; WriteResult::notice says so.
// Embedded writes use the same safety as saving the rotation (LosslessTransform.h):
// symbolic links resolved, temporary sibling + F_FULLFSYNC + rename (hard links: copy
// into the inode), Finder tags/xattrs/ACL/creation date kept, the modification date kept
// on request, and a FileStamp check right before the replacement. JPEG files are checked
// to have byte-identical scan data afterwards.
//
// The free functions block (file I/O): call them on worker threads. RatingStore (GUI
// thread) caches ratings, reads them asynchronously and queues writes on FileOps with undo.

#include "FileOps.h"
#include "Rating.h"
#include "Sidecars.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace lf {

struct RatingOptions {
    bool sidecar = true;
    bool embedded = false;
    bool finderTags = false;
    RatingTagNames tags;
    bool keepModificationDate = true; // embedded writes (F-CULL-02)
    friend bool operator==(const RatingOptions&, const RatingOptions&) = default;
};

enum class RatingSource { Sidecar, Embedded, FinderTags };

struct RatingWriteResult {
    QString error;  // German; empty = success
    QString notice; // German, e.g. "RAW: in der XMP-Zusatzdatei gespeichert"
    bool ok() const { return error.isEmpty(); }
};

namespace ratingio {

// Conflict rule: the first enabled source (sidecar, embedded, tags) with a value wins.
std::optional<Rating> resolve(const RatingOptions& options, const std::optional<Rating>& sidecar,
                              const std::optional<Rating>& embedded, const std::optional<Rating>& tags);

// The sidecar a rating is read from (first existing one the image owns) / written to (the
// existing one, else "<file>.xmp" or "<base>.xmp"), see Sidecars.h: a JPEG next to a RAW
// of the same base name never uses the RAW's "<base>.xmp". `folder` (optional): a listing
// of the image's folder, made once for many images.
QString existingSidecar(const QString& imagePath, const sidecars::Folder* folder = nullptr);
QString sidecarForWriting(const QString& imagePath);

std::optional<Rating> readSidecar(const QString& imagePath, const sidecars::Folder* folder = nullptr);
// Writes the rating into the sidecar (other XMP content kept; created if missing; an empty
// rating in a sidecar that holds nothing else of ours leaves the other content). Atomic.
QString writeSidecar(const QString& imagePath, const Rating& rating);

// True if the rating can be embedded into this file (type, not RAW, single image).
bool canEmbed(const QString& path, QString* reason = nullptr);
std::optional<Rating> readEmbedded(const QString& path);
// Empty string = written. `*refused` (optional) is set if the format cannot be written
// safely (caller falls back to the sidecar); otherwise a non-empty string is an error.
QString writeEmbedded(const QString& path, const Rating& rating, bool keepModificationDate,
                      bool* refused = nullptr);

std::optional<Rating> readTags(const QString& path, const RatingTagNames& names);
QString writeTags(const QString& path, const Rating& rating, const RatingTagNames& names);

// All enabled sources, conflict rule applied. Unrated (nothing stored) = empty Rating.
Rating read(const QString& path, const RatingOptions& options, const sidecars::Folder* folder = nullptr);
// Every enabled store (embedded refused -> sidecar).
RatingWriteResult write(const QString& path, const Rating& rating, const RatingOptions& options);
// Many files (F-CULL-04/05): the .xmp files of each folder are listed once.
QHash<QString, Rating> readMany(const QStringList& paths, const RatingOptions& options,
                                const std::function<bool()>& cancelled = {});

} // namespace ratingio

// Platform part (src/platform/*): embedded XMP and Finder tags with colors.
namespace platform {
// ImageIO can write the type's metadata without re-encoding (JPEG, HEIC/HEIF, TIFF, PNG,
// DNG when supported by the system).
bool canEmbedRatingType(const QString& typeId);
std::optional<Rating> readEmbeddedRating(const QString& path);
// Copies `src` to the new file `dst` with the rating merged into its XMP (pixels untouched).
bool writeEmbeddedRating(const QString& src, const QString& dst, const Rating& rating, QString* error);
// Raw Finder tags ("Name" / "Name\n<color>"); nullopt + error if unreadable.
std::optional<QStringList> readRawTags(const QString& path, QString* error);
bool writeRawTags(const QString& path, const QStringList& rawTags, QString* error);
bool tagsSupported();
} // namespace platform

// GUI-thread facade: cache, asynchronous reads, undoable writes.
class RatingStore : public QObject {
    Q_OBJECT
public:
    static RatingStore& instance();
    explicit RatingStore(QObject* parent = nullptr);
    ~RatingStore() override;

    RatingOptions options() const;
    void setOptions(const RatingOptions& options); // clears the cache
    // Options from Settings (key::Rating*), kept up to date (called by the app once).
    void followSettings();

    // Cached value, nullopt if not read yet (then call request()).
    std::optional<Rating> cached(const QString& path) const;
    // Reads on a worker thread; ratingChanged(path, rating) follows (also if unchanged).
    void request(const QString& path);
    // Blocking read (worker threads, e.g. Collection jobs); fills the cache.
    Rating readNow(const QString& path);
    // Reader for Collection (thread-safe).
    std::function<Rating(const QString&)> reader();
    void forget(const QString& path); // the file changed or went away

    // F-CULL-01/02: writes `rating` to every enabled store through `ops` (one undo step,
    // undo writes the previous rating back). `done(ok, error, notice)` on the GUI thread.
    using WriteDone = std::function<void(bool ok, const QString& error, const QString& notice)>;
    void setRating(FileOps& ops, const QString& path, const Rating& rating, WriteDone done = {});
    // Like setRating(), with the new rating computed from the stored one on the worker
    // (e.g. "P" keeps the stars even if the rating was not read yet). `done` gets the result.
    using Change = std::function<Rating(const Rating& stored)>;
    using ChangeDone =
        std::function<void(bool ok, const QString& error, const QString& notice, const Rating& now)>;
    void changeRating(FileOps& ops, const QString& path, Change change, ChangeDone done = {});
    // Several files as ONE undo step (both files of a RAW+JPEG pair, a grid selection):
    // each file's new rating is computed from its own stored one; if one write fails, the
    // files written before get their previous rating back. `done` reports the first path.
    void changeRatings(FileOps& ops, const QStringList& paths, Change change, ChangeDone done = {});

signals:
    void ratingChanged(const QString& path, const lf::Rating& rating);

private:
    void store(const QString& path, const Rating& r);
    mutable QMutex m_mutex;
    RatingOptions m_options;
    QHash<QString, Rating> m_cache;
    quint64 m_optionsGeneration = 0;
};

} // namespace lf
