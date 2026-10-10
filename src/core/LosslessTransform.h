// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Saving the view rotation into the file (F-EDIT-02), without re-encoding pixels:
//   (1) EXIF orientation tag (default; JPEG, HEIC/HEIF, TIFF, PNG): only the metadata
//       changes. JPEG files that already carry the tag are patched in place (two bytes),
//       everything else goes through the platform's lossless metadata copy
//       (ImageIO CGImageDestinationCopyImageSource on macOS).
//   (2) JPEG DCT transform (libjpeg-turbo tj3Transform, like jpegtran): the compressed
//       blocks are rearranged, the orientation tag becomes 1. Not every transform is
//       possible without touching the partial blocks at the right/bottom edge; then the
//       caller asks whether those edges may be trimmed.
// Lossy re-encoding is never done silently: planRotationSave() reports NeedsReencode and
// the caller asks the user first. Camera RAW files are never written.
//
// All file work is meant for worker threads.
//
// File safety (see docs in replaceFile() below):
//   - symbolic links are resolved: the real file is rewritten next to itself, the link stays;
//   - the new contents are written to a temporary sibling, flushed to the disk
//     (F_FULLFSYNC) and then either renamed over the original (one link: atomic; a new
//     inode that keeps permissions, ACL, extended attributes/Finder tags, creation date and
//     optionally the modification date) or - for a file with several hard links - copied
//     into the existing inode, so that every link sees the new contents and the file ID
//     stays (aliases/bookmarks);
//   - a FileStamp taken when planning is checked again right before the replacement: if the
//     file changed meanwhile, nothing is written;
//   - RotationEdit keeps undo backups (UndoBackupStore) and refuses undo/redo when the file
//     was changed by someone else after the last step.
// JPEG files with additional images behind the primary one (MPF: Ultra HDR / gain maps,
// MPO stereo pairs) are never DCT-transformed (the secondary images would get lost or no
// longer match); the orientation tag is set instead (RotationPlan::notice says why).
// HEIC/HEIF files whose auxiliary images (gain map, depth, matte) would not survive the
// metadata copy are refused.

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <mutex>

namespace lf {

// ---- orientation algebra (EXIF orientation codes 1..8 = the dihedral group D4) ----
namespace orientation {
// Orientation that shows a file with orientation `exif` the way the view currently shows
// it after rotating it by `quarterTurnsCW` (clockwise) and then flipping it (horizontal
// = left/right mirror, applied after the rotation, as ImageView does).
int compose(int exif, int quarterTurnsCW, bool flipH, bool flipV);
// a·b: first b, then a (both as transforms of the stored pixels to the display).
int multiply(int a, int b);
int inverse(int o);
bool swapsAxes(int o); // 5..8
} // namespace orientation

// ---- JPEG helpers (portable, byte level) ----
namespace jpeg {
bool isJpeg(const QByteArray& data);
// Compressed image data: everything from the first SOS marker to the end of the file.
// Identical scan data = identical pixels (the metadata write did not re-encode).
QByteArray scanData(const QByteArray& data);
// Orientation stored in the EXIF IFD0 (0 = no EXIF/no tag).
int exifOrientation(const QByteArray& data);
enum class PatchResult { Patched, NoTag, Invalid };
// Writes `orientation` into the existing EXIF orientation tag (and an XMP tiff:Orientation
// of the same length) in place; nothing else changes. `dropThumbnail` unlinks the EXIF
// thumbnail (IFD1), which no longer matches after a DCT transform.
PatchResult patchOrientation(QByteArray& data, int orientation, bool dropThumbnail = false);

enum class DctResult { Ok, NotPerfect, Unavailable, Failed };
// True if libjpeg-turbo is linked in (strategy (2) available).
bool dctTransformAvailable();
// Applies the pixel transform `op` (orientation code 2..8: the transform that maps the
// stored pixels to the new stored pixels) to the DCT blocks. `trim` drops partial edge
// blocks that cannot be transformed; without it such transforms return NotPerfect.
// Markers (EXIF, ICC, XMP) are copied unchanged.
DctResult transformDct(const QByteArray& in, int op, bool trim, QByteArray* out, QString* error);
// Offset just behind the EOI marker of the first (primary) image, -1 if not found.
qsizetype primaryImageEnd(const QByteArray& data);
// True if the file carries more than the primary image: an MPF (APP2 "MPF\0") segment or
// data behind the primary image's EOI (Ultra HDR gain map, MPO second view, …).
bool hasAdditionalImages(const QByteArray& data);
} // namespace jpeg

// ---- file identity ----

// What a file looked like at one moment: size, modification time, inode/device and - for
// files up to kHashLimit bytes - a SHA-256 of the contents. Used to notice changes made by
// someone else between two steps (plan -> save, save -> undo -> redo).
struct FileStamp {
    static constexpr qint64 kHashLimit = 64 * 1024 * 1024;
    qint64 size = -1;
    qint64 mtimeNs = 0;
    quint64 inode = 0;
    quint64 device = 0;
    QByteArray hash; // empty if the file is larger than kHashLimit
    bool isValid() const { return size >= 0; }
    bool operator==(const FileStamp&) const = default;
};
FileStamp fileStamp(const QString& path);

// The file `path` really is: symbolic links resolved (absolute, canonical). For a missing
// file the absolute path.
QString resolvedFilePath(const QString& path);

// ---- saving the rotation ----

enum class RotationStrategy { ExifOrientation, LosslessJpeg };

struct RotationRequest {
    QString path;
    int quarterTurnsCW = 0; // view rotation (ImageView::rotation())
    bool flipH = false, flipV = false;
    RotationStrategy strategy = RotationStrategy::ExifOrientation;
    bool keepModificationDate = false;
};

enum class RotationMethod {
    None,             // nothing to save (view shows the file as stored)
    PatchExif,        // JPEG: orientation tag patched in place
    MetadataCopy,     // platform lossless metadata copy with a new orientation
    JpegDct,          // DCT transform (strategy 2)
    JpegDctTrim,      // DCT transform dropping partial edge blocks (user agreed)
    ReencodeLossless, // format without orientation support, lossless codec (PNG/BMP/TIFF/TGA)
    ReencodeLossy,    // format without orientation support, lossy codec (user agreed)
};

struct RotationPlan {
    enum Status {
        Ready,         // `method` can be carried out
        NeedsTrim,     // DCT transform possible only with trimmed edges (ask; alternative: EXIF)
        NeedsReencode, // only by re-encoding with loss (ask)
        Refused,       // never (RAW, animations, multi-page, unreadable) - `message` says why
    };
    Status status = Refused;
    RotationMethod method = RotationMethod::None;
    int newOrientation = 1; // orientation tag after saving
    int pixelOp = 1;        // transform of the stored pixels (DCT / re-encode)
    QString typeId;         // UTType / format id of the file
    QString message;        // German, user facing (Refused / question details)
    QString notice;         // German, user facing: why another method than asked for was chosen
    QString target;         // the real file (symbolic links resolved)
    FileStamp stamp;        // `target` when planned: re-checked before it is replaced
};

// Worker thread: decides how the rotation can be saved (reads the file, writes nothing).
RotationPlan planRotationSave(const RotationRequest& request);
// Worker thread: carries out `method` (from the plan, possibly changed after asking:
// JpegDctTrim, ReencodeLossy, or MetadataCopy/PatchExif instead of DCT). Returns an empty
// string on success, else a German error text. The file is replaced atomically.
QString executeRotationSave(const RotationRequest& request, const RotationPlan& plan, RotationMethod method);
// German description of a method for status messages ("EXIF-Ausrichtung gesetzt" …).
QString describeRotationMethod(RotationMethod method);
// Undo/redo of a saved rotation: replaces `target` (symbolic links resolved) by a copy of
// `backup` (bytes and modification date of the backup), like executeRotationSave() does.
// Empty string on success, else a German error.
QString restoreFileFrom(const QString& backup, const QString& target);

class UndoBackupStore;

// An in-place change of one file with undo/redo, for FileOps::modifyAsync() (F-EDIT-02
// rotation, F-EDIT-03 crop): apply() saves (first call: `save`, which must replace `target`
// safely, e.g. with platform::replaceFile()) or redoes, revert() undoes. Both run on the
// FileOps worker, one after the other. Before the first save a backup of the file is put
// into the UndoBackupStore, after it a copy of the result (for redo). After every successful
// step the FileStamp of the file is recorded; a later step refuses (German message, nothing
// written) if the file no longer matches it, or if the store dropped the backups to stay
// within its budget.
class FileEdit {
public:
    using Save = std::function<QString()>; // empty string = success, else a German error
    FileEdit(QString target, Save save, std::shared_ptr<UndoBackupStore> store);
    ~FileEdit(); // releases the backups (deleted on a worker thread)
    FileEdit(const FileEdit&) = delete;
    FileEdit& operator=(const FileEdit&) = delete;

    QString apply();
    QString revert();
    QString target() const { return m_target; }

    static QString messageChangedUndo(); // "Datei wurde inzwischen verändert – Rückgängig nicht möglich"
    static QString messageChangedRedo();
    static QString messageBackupDropped();

private:
    QString step(bool forward);
    QString m_target; // the real file (symbolic links resolved)
    Save m_save;
    std::shared_ptr<UndoBackupStore> m_store;
    std::mutex m_mutex;
    quint64 m_id = 0;     // backup entry in the store (0 = none yet)
    bool m_done = false;  // the file holds the change (else the original)
    FileStamp m_expected; // the file after the last successful step
};

// One saved rotation with undo/redo (F-EDIT-02): a FileEdit whose save is
// executeRotationSave().
class RotationEdit {
public:
    RotationEdit(const RotationRequest& request, RotationPlan plan, RotationMethod method,
                 std::shared_ptr<UndoBackupStore> store);
    ~RotationEdit();
    RotationEdit(const RotationEdit&) = delete;
    RotationEdit& operator=(const RotationEdit&) = delete;

    QString apply() { return m_edit->apply(); }
    QString revert() { return m_edit->revert(); }

    static QString messageChangedUndo() { return FileEdit::messageChangedUndo(); }
    static QString messageChangedRedo() { return FileEdit::messageChangedRedo(); }
    static QString messageBackupDropped() { return FileEdit::messageBackupDropped(); }

private:
    std::unique_ptr<FileEdit> m_edit;
};

// ---- helpers for safe file writes (shared by F-EDIT-02/03/04) ----
namespace filesafety {
// Hidden temporary file next to `path` (same volume: the final rename is atomic).
QString siblingTempPath(const QString& path);
// Writes `data` to the new file `path` (never replaces an existing one).
bool writeNewFile(const QString& path, const QByteArray& data, QString* error);
// The whole file; empty (and a German `error`) if unreadable.
QByteArray readFile(const QString& path, QString* error);
} // namespace filesafety

// ---- platform part (src/platform/*/…ImageTransform…) ----
namespace platform {
// Type identifier of the file by content ("public.jpeg", "public.heic", …), empty if unknown.
QString imageTypeIdentifier(const QString& path);
bool isCameraRawType(const QString& typeId);
// Number of images in the file (frames or pages); 0 if unreadable.
int imageCount(const QString& path);
// True if the type can store an orientation tag that the platform writes losslessly.
bool supportsOrientationMetadata(const QString& typeId);
// Copies `src` to `dst` (new file) with the orientation tag set to `orientation`, without
// re-encoding the image data. Fails (German explanation) if auxiliary images of `src`
// (HDR gain map, depth, disparity, matte) would be missing in `dst`.
bool writeOrientationLossless(const QString& src, const QString& dst, int orientation, QString* error);
// Re-encodes `src` into `dst` (same type) with the stored pixels moved by `pixelOp` and the
// orientation tag reset to 1; metadata is kept. The decoded pixel buffer is moved as it is
// (no premultiplication, same color model, bit depth, palette and alpha) and written with
// the original properties. With `requireExact` the result is decoded again and must equal
// the moved pixels in color model, depth, alpha and every value; otherwise `dst` is removed,
// `*inexact` (optional) set and false returned - the codec cannot store the image without
// loss. Without it (lossy codec, the user agreed) layouts that cannot be moved bytewise are
// drawn instead.
bool reencodeTransformed(const QString& src, const QString& dst, int pixelOp, bool requireExact,
                         QString* error, bool* inexact = nullptr);
// True if re-encoding `typeId` can be lossless (PNG, BMP, TIFF, TGA); it still has to pass
// the exact check of reencodeTransformed().
bool isLosslessCodec(const QString& typeId);
// Replaces `target` (a regular file, no symbolic link) by the new file `replacement` (same
// folder). The replacement is flushed to the disk first (F_FULLFSYNC on macOS, fsync
// elsewhere).
//   - One link: atomic rename over `target`, then the folder is flushed. `target`'s
//     permissions, ACL, extended attributes (Finder tags) and creation date are carried
//     over. The file gets a new inode (as with every "safe save" on APFS: exchangedata()
//     is not supported there and renamex_np(RENAME_SWAP) moves the inode with the name).
//   - Several hard links: the contents are copied into the existing inode (every link sees
//     them, the file ID and all attributes stay), then flushed; `replacement` is removed
//     afterwards. Not atomic - a crash in between leaves the hidden temporary file (and the
//     undo backup) with the complete new contents.
// The modification date becomes the current time, or stays `target`'s if `keepModificationDate`.
bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error);
// Copies `path` with all its metadata to the new file `copy` (undo backups): an APFS clone
// if possible. `*cloned` (optional) tells whether it was one (shares the data blocks).
bool copyWithMetadata(const QString& path, const QString& copy, QString* error, bool* cloned = nullptr);

// Durability steps, reported for tests (seam: setDurabilityObserver()).
enum class DurabilityStep { FullSync, Rename, InPlaceCopy, FolderSync };
void reportDurabilityStep(DurabilityStep step, const QString& path);
// Test seam: observes every durability step (nullptr = none). Not for production code.
void setDurabilityObserver(std::function<void(DurabilityStep, const QString&)> observer);
} // namespace platform

} // namespace lf
