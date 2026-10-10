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
// All file work is meant for worker threads. Files are replaced atomically (temporary
// file in the same folder + rename) and keep permissions, extended attributes (Finder
// tags), creation date and optionally the modification date.

#include <QByteArray>
#include <QString>

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
} // namespace jpeg

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
};

// Worker thread: decides how the rotation can be saved (reads the file, writes nothing).
RotationPlan planRotationSave(const RotationRequest& request);
// Worker thread: carries out `method` (from the plan, possibly changed after asking:
// JpegDctTrim, ReencodeLossy, or MetadataCopy/PatchExif instead of DCT). Returns an empty
// string on success, else a German error text. The file is replaced atomically.
QString executeRotationSave(const RotationRequest& request, const RotationPlan& plan, RotationMethod method);
// German description of a method for status messages ("EXIF-Ausrichtung gesetzt" …).
QString describeRotationMethod(RotationMethod method);
// Undo/redo of a saved rotation: replaces `target` atomically by a copy of `backup` (bytes
// and modification date of the backup). Empty string on success, else a German error.
QString restoreFileFrom(const QString& backup, const QString& target);

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
// re-encoding the image data.
bool writeOrientationLossless(const QString& src, const QString& dst, int orientation, QString* error);
// Re-encodes `src` into `dst` (same type) with the pixels transformed by `pixelOp` and the
// orientation tag reset to 1; metadata is kept.
bool reencodeTransformed(const QString& src, const QString& dst, int pixelOp, QString* error);
// True if re-encoding `typeId` loses nothing (PNG, BMP, TIFF without JPEG compression …).
bool isLosslessCodec(const QString& typeId);
// Replaces `target` by the new file `replacement` (same folder) atomically. `target`'s
// permissions, ACL, extended attributes (Finder tags) and creation date are carried
// over; its modification date too if `keepModificationDate`.
bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error);
// Copies `path` with all its metadata (clone where possible) to `copy` (undo backups).
bool copyWithMetadata(const QString& path, const QString& copy, QString* error);
} // namespace platform

} // namespace lf
