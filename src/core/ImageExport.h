// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Exporting / converting images (F-EDIT-04): one image or a selection (grid), into
// JPEG/PNG/HEIC/AVIF/JPEG XL/TIFF - only the formats the platform can encode (macOS:
// CGImageDestinationCopyTypeIdentifiers; JPEG XL is decode-only there and not offered).
// Options: quality, maximum long edge, metadata (keep / without GPS / none), color space
// (keep / sRGB / Display P3), HDR (keep the gain map where the format supports it / SDR
// tone-mapped), file name pattern ("{name}-export"), target folder, and a crop rectangle
// ("Zugeschnitten exportieren" from the crop mode, F-EDIT-03: RAW, animations, archive
// members and formats that cannot be written in place get a cropped copy this way).
//
// The source is never changed or overwritten: names that would hit the source get the
// "Beide behalten" treatment; other existing files are handled with the F-FILE-11 choice
// (Ersetzen = the old file goes to the trash, Beide behalten, Überspringen). Images inside
// archives and PDF pages (read-only, VirtualPath.h) are exported from their decoded pixels.
//
// Files are written to a hidden temporary sibling and renamed into place without replacing
// anything (fileops::moveNoReplace). run() works on a worker thread, reports progress and
// stops between images when cancelled.

#include "FileBatch.h" // ConflictChoice

#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class QImage;

namespace lf {

class TrashService;

namespace exporting {

enum class MetadataMode { KeepAll, RemoveGps, RemoveAll };
enum class ColorMode { Keep, SRgb, DisplayP3 };
enum class HdrMode { Keep, ToneMapSdr };

struct Format {
    QString id;     // "jpeg", "png", "heic", "avif", "jxl", "tiff" (Settings key::ExportFormat)
    QString typeId; // UTType ("public.jpeg" …)
    QString suffix; // file suffix without dot
    QString label;  // "JPEG", "PNG" … (not translated)
    bool lossy = true;
    bool gainMap = false; // can store an HDR gain map (ImageIO: ISO gain map)
};
// Every format the export knows, in menu order.
QVector<Format> knownFormats();
// The formats `encoderTypes` can write (default: platform::exportTypeIdentifiers()).
QVector<Format> availableFormats(const QStringList& encoderTypes);
QVector<Format> availableFormats();
// The format with `id`; empty Format (id empty) if unknown.
Format formatById(const QString& id);

struct Options {
    QString formatId = QStringLiteral("jpeg");
    int quality = 90;    // 1..100 (lossy formats)
    int maxLongEdge = 0; // pixels; 0 = original size (never enlarged)
    MetadataMode metadata = MetadataMode::KeepAll;
    ColorMode color = ColorMode::Keep;
    HdrMode hdr = HdrMode::Keep;
    QString pattern = QStringLiteral("{name}-export");
    QString folder; // empty: next to the source (archives/PDF: next to the container)
    // Part of the image to export, normalized to the displayed image (EXIF orientation
    // applied; 0..1 in both axes). Empty: all of it. Not stored in the settings.
    QRectF crop;
};

// The pixel rectangle of `crop` (normalized, see Options::crop) in an image of the displayed
// size `displayed`; the whole image for an empty or invalid crop. Never empty for a valid size.
QRect cropPixels(const QRectF& crop, QSize displayed);

QString metadataModeId(MetadataMode m); // "all", "noGps", "none"
MetadataMode metadataModeFromId(const QString& id);
QString colorModeId(ColorMode m); // "keep", "srgb", "p3"
ColorMode colorModeFromId(const QString& id);
QString hdrModeId(HdrMode m); // "keep", "sdr"
HdrMode hdrModeFromId(const QString& id);

// The base name of `source` ("IMG_1" for "/a/IMG_1.jpg", "012" for an archive member
// ".../x.cbz#/k/012.png", "Plan Seite 3" for a PDF page).
QString sourceBaseName(const QString& source);
// File name for `source` from the pattern: {name} = base name, {n} = position (1-based,
// zero-padded to the digits of `count`), {format} = format suffix; '/' and ':' are
// replaced, an empty result falls back to "{name}-export". Adds the format's suffix.
QString targetFileName(const QString& source, const Options& options, int index, int count);
QString targetFolder(const QString& source, const Options& options);

struct Item {
    QString source;
    QString target; // full path
    ConflictChoice onConflict = ConflictChoice::Skip;
};
// Targets for `sources`; names repeated inside the batch, and names that are the source
// itself, are made unique at once ("name 2.jpg"). `exists` defaults to the file system.
QVector<Item> planItems(const QStringList& sources, const Options& options,
                        const std::function<bool(const QString&)>& exists = {});
// Indices of items whose target exists (ask: F-FILE-11).
QVector<int> conflicts(const QVector<Item>& items, const std::function<bool(const QString&)>& exists = {});

struct Result {
    int written = 0;
    int skipped = 0;
    int failed = 0;
    bool cancelled = false;
    QStringList outputs; // written files
    QStringList errors;  // German, "name: reason"
    QStringList notices; // German (e.g. HDR not kept for this format)
};
using Progress = std::function<void(int done, int total, const QString& current)>;
// Worker thread. `trash` puts replaced files into the trash (nullptr: Replace acts like
// KeepBoth); the new file is written first, the old one goes to the trash only after that.
Result run(const QVector<Item>& items, const Options& options, const std::atomic_bool& cancel,
           const Progress& progress, TrashService* trash);
// One image (no conflict handling): writes `target` (must not exist). Empty = success.
QString exportOne(const QString& source, const QString& target, const Options& options,
                  QString* notice = nullptr);

} // namespace exporting

namespace platform {
// Type identifiers the platform can encode (macOS: CGImageDestinationCopyTypeIdentifiers).
QStringList exportTypeIdentifiers();
// Encodes `source` (a file the platform can read) into the new file `dst`. `notice` gets a
// German note when an option could not be applied (e.g. HDR without gain map support).
bool exportImage(const QString& source, const QString& dst, const exporting::Options& options, QString* error,
                 QString* notice);
// The same for decoded pixels (images in archives, PDF pages).
bool exportDecoded(const QImage& image, const QString& dst, const exporting::Options& options, QString* error,
                   QString* notice);
} // namespace platform

} // namespace lf
