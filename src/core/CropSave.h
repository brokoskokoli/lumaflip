// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Cropping (F-EDIT-03): geometry of the interactive crop (aspect presets, handles), the
// mapping between displayed and stored pixels (EXIF orientation), and saving:
//   - JPEG: lossless (libjpeg-turbo tj3Transform with a crop region, like jpegtran -crop).
//     The left/top edge must lie on the iMCU grid (8 or 16 px); the selection is extended
//     to the grid (right/bottom edges stay) and the user is told. EXIF/ICC/XMP markers are
//     copied, the EXIF and XMP pixel dimensions are updated, the thumbnails that would
//     still show the uncropped image (EXIF IFD1, Photoshop IRB) are removed - their bytes,
//     not only the link - and the orientation tag stays (the crop is done in stored pixels). JPEG files with
//     further images (MPF: Ultra HDR gain maps, MPO stereo pairs) are never cropped losslessly - the
//     secondary images would no longer match; re-encoding (with loss, without them) is offered as a question
//     instead.
//   - lossless codecs (PNG, TIFF, BMP, TGA): re-encoded with the same color model, bit
//     depth, palette, alpha, ICC profile and metadata; checked by reading the result back.
//   - lossy codecs (HEIC, AVIF, WebP …) and inexact cases: only after asking (never
//     silently lossy, SPEC 4.8).
//   - RAW, animations, multi-page files, images inside archives/PDFs and formats the
//     platform cannot encode are never cropped in place: the plan is refused with
//     `exportInstead`, and the window offers "Zugeschnitten exportieren …" (the export with
//     exporting::Options::crop writes a new file; animations/multi-page: the first image).
// "Sichern" replaces the file (FileEdit: undo backups, symbolic/hard link safety, fsync,
// plan/commit check); "Speichern unter" always writes a new file and never the source, and
// replaces an existing file only when the user agreed (CropRequest::replaceExisting).
// All file work is for worker threads.

#include "LosslessTransform.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTransform>

#include <array>
#include <memory>

namespace lf {

namespace crop {

// Aspect presets of the crop overlay (ids for Settings key::CropAspect).
enum class Aspect { Free, Original, Square, R4x3, R3x2, R16x9 };
QString aspectId(Aspect a);             // "free", "original", "1:1", "4:3", "3:2", "16:9"
Aspect aspectFromId(const QString& id); // unknown -> Free
QString aspectLabel(Aspect a);          // UI label (translated)
// Width/height ratio of the preset for an image of `original` size (displayed), in the
// landscape form for the fixed ratios and flipped with `portrait`; 0 = free.
double aspectRatio(Aspect a, QSizeF original, bool portrait);

// What a drag changes: one of the eight handles, the whole rectangle or a new rectangle.
enum class Handle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Move, New };

// Applies a drag of `handle` from `start` (the rectangle when the drag began) by `delta`
// (both in the same coordinate system, e.g. widget pixels) within `bounds`, keeping the
// width/height `ratio` (0 = free) and a minimum size. For Handle::New, `anchor` is where
// the drag started and `delta` the offset of the current point.
QRectF dragRect(const QRectF& start, Handle handle, QPointF delta, const QRectF& bounds, double ratio,
                double minSize = 8.0, QPointF anchor = {});
// The largest centered rectangle of `ratio` (0 = all of it) inside `bounds`.
QRectF fitRatio(const QRectF& bounds, double ratio);
// Rule-of-thirds lines of `r`: two vertical x positions and two horizontal y positions.
std::array<double, 4> thirds(const QRectF& r);

// Stored pixels -> displayed pixels for EXIF orientation `orientation` (1..8) of an image
// of `storedSize`: maps rectangles in both directions exactly (integer coordinates).
QTransform storedToDisplay(QSize storedSize, int orientation);
QRect displayToStoredRect(const QRect& displayRect, QSize storedSize, int orientation);
QRect storedToDisplayRect(const QRect& storedRect, QSize storedSize, int orientation);
// Moves the left/top edge of `stored` down to multiples of `mcu` (the right/bottom edges
// stay, so the selection only grows) and clips to `imageSize`. Unchanged for an invalid mcu.
QRect snapToMcu(const QRect& stored, QSize mcu, QSize imageSize);

} // namespace crop

namespace jpeg {
// iMCU size of the JPEG (8x8 … 16x16, by chroma subsampling); invalid if unknown or
// libjpeg-turbo is not available.
QSize mcuSize(const QByteArray& data);
// Width/height of the JPEG frame (stored pixels); invalid if not a JPEG.
QSize frameSize(const QByteArray& data);
// Lossless crop of the DCT blocks to `stored` (x/y multiples of mcuSize()). Markers (EXIF,
// ICC, XMP …) are copied unchanged.
DctResult cropDct(const QByteArray& in, const QRect& stored, QByteArray* out, QString* error);
// Writes the EXIF PixelXDimension/PixelYDimension (Exif IFD) in place where present; false
// if a value does not fit the stored type (SHORT) or the EXIF block is broken.
bool patchPixelDimensions(QByteArray& data, int width, int height);
// EXIF PixelXDimension/PixelYDimension, invalid if absent.
QSize exifPixelDimensions(const QByteArray& data);
// Removes the EXIF thumbnail: IFD0 no longer links IFD1, and IFD1 and the thumbnail JPEG it
// points to are overwritten with zeros (same size, nothing else moves). False if there was
// none.
bool removeExifThumbnail(QByteArray& data);
// Removes the Photoshop thumbnail resources (IRB 0x0409/0x040C) from APP13 "Photoshop 3.0"
// segments (the segment shrinks). Returns how many were removed.
int removeIrbThumbnails(QByteArray& data);
// Writes the size into the XMP packet (exif:PixelXDimension/PixelYDimension,
// tiff:ImageWidth/ImageLength; attribute or element form) where these are present.
void patchXmpDimensions(QByteArray& data, int width, int height);
// Decodes to RGB888 with libjpeg-turbo; `fastUpsample` = nearest chroma upsampling (exact
// comparisons across block borders). Null without libjpeg-turbo.
QImage decodePixels(const QByteArray& data, bool fastUpsample);
} // namespace jpeg

struct CropRequest {
    QString path;
    QRect rect;     // displayed full-resolution pixels (EXIF orientation applied)
    QString saveAs; // empty: replace `path` (undoable); else a new file (never the source)
    // "Speichern unter": the user agreed to replace an existing `saveAs` (file dialog or the
    // question after the suffix was added). Without it an existing file is never replaced.
    bool replaceExisting = false;
    bool keepModificationDate = false;
};

enum class CropMethod {
    None,
    JpegLossless,     // DCT crop (left/top possibly snapped to the block grid)
    ReencodeLossless, // PNG/TIFF/BMP/TGA, exact
    ReencodeLossy,    // after asking
};

struct CropPlan {
    enum Status {
        Ready,         // `method` can be carried out
        NeedsReencode, // only by re-encoding with loss (ask; `message` says why)
        Refused,       // never (RAW, animations, unreadable …), `message` says why
    };
    Status status = Refused;
    CropMethod method = CropMethod::None;
    QRect storedRect;     // the saved part in stored pixels
    QRect displayRect;    // the same in displayed pixels (may differ from the request: snapping)
    bool snapped = false; // JPEG: the selection was extended to the block grid
    // Refused, but "Zugeschnitten exportieren" can write a cropped copy (RAW, animations,
    // multi-page files, archive/PDF members, formats without an encoder here).
    bool exportInstead = false;
    QSize mcu; // JPEG block size
    QString typeId;
    QString message;     // German, user facing (Refused / the question)
    QString notice;      // German, user facing (snapping, auxiliary images)
    QString target;      // the real source file (symbolic links resolved)
    QString destination; // file written: target, or the resolved "save as" path
    FileStamp stamp;     // `target` when planned
};

// Worker thread: how the crop can be saved (reads, writes nothing but probe files in the
// temporary folder).
CropPlan planCropSave(const CropRequest& request);
// Worker thread: carries out `method`. Empty string on success, else a German error.
// Replacing: atomic via platform::replaceFile() after re-checking plan.stamp. Save as: a
// new file next to the destination, renamed without replacing (an existing destination the
// user agreed to replace is replaced safely; the source never).
QString executeCropSave(const CropRequest& request, const CropPlan& plan, CropMethod method);
QString describeCropMethod(CropMethod method, bool snapped);

// Undoable in-place crop for FileOps::modifyAsync().
std::unique_ptr<FileEdit> makeCropEdit(const CropRequest& request, const CropPlan& plan, CropMethod method,
                                       std::shared_ptr<UndoBackupStore> store);

namespace platform {
// Re-encodes `src` into `dst` (same type) keeping only `stored` (stored pixels), with the
// original color model, bit depth, palette, alpha, ICC profile and metadata (EXIF/XMP/IPTC;
// EXIF pixel dimensions updated, orientation unchanged). `requireExact`: the result must
// decode to exactly the cropped pixels, else `dst` is removed, `*inexact` set and false
// returned. Without it (lossy codec, the user agreed) any layout is accepted.
bool reencodeCropped(const QString& src, const QString& dst, const QRect& stored, bool requireExact,
                     QString* error, bool* inexact = nullptr);
// German names of auxiliary images of the file (HDR gain map, depth map …), empty if none.
QStringList auxiliaryImageNames(const QString& path);
// True if reencodeCropped() can write files of `typeId` (macOS: ImageIO encoders; elsewhere
// Qt's image writers).
bool canEncodeType(const QString& typeId);
// False where reencodeCropped() writes the pixels only (Qt's writers: no EXIF/XMP/IPTC).
bool reencodeKeepsMetadata();
} // namespace platform

} // namespace lf
