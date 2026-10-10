// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which XMP sidecar belongs to which image (F-CULL-02 a, F-FMT-06, F-FILE-08/11).
//
// Two namings exist: "IMG_1.JPG.xmp" (darktable/digiKam, names exactly one image) and the
// Adobe one "IMG_1.xmp" (Lightroom/Bridge/Capture One), which every file with the base name
// "IMG_1" could claim. The rule:
//   - "<file>.xmp" belongs to that file alone; it is read first.
//   - "<base>.xmp" belongs to a camera RAW with that base name (Lightroom's sidecar of the
//     RAW of a RAW+JPEG pair), and to any other image only if no other image file of the
//     folder shares the base name. A JPEG next to "IMG_1.CR3" therefore never reads or
//     writes "IMG_1.xmp" - its rating lives in "IMG_1.JPG.xmp", whatever the pairing setting.
//
// Base names are compared literally (no wildcard patterns, so "Shot [a]" or "a*b" are
// plain names), NFC-normalized and case-insensitively on case-insensitive volumes.
// The functions list the folder (file I/O): call them on worker threads.

#include <QHash>
#include <QString>
#include <QStringList>

namespace lf::sidecars {

// One listing of a folder: lookups by (folded) file name and base name. Lists the folder
// once for many images (F-CULL-04/05 read all ratings of a folder).
class Folder {
public:
    Folder() = default;
    static Folder list(const QString& folderPath);

    QString path() const { return m_path; }
    bool isCaseInsensitive() const { return m_caseInsensitive; }
    // NFC, case-folded on case-insensitive volumes.
    QString fold(const QString& name) const;
    // The actual name of the file `name` (compared folded); empty if there is none.
    QString find(const QString& name) const;
    // Image files (not .xmp) whose complete base name equals `baseName` (folded).
    QStringList imagesWithBase(const QString& baseName) const;

private:
    QString m_path;
    bool m_caseInsensitive = true;
    QHash<QString, QString> m_names;      // folded name -> actual name
    QHash<QString, QStringList> m_byBase; // folded base name -> image file names
};

// True if the volume holding `folderPath` compares names case-insensitively.
bool isCaseInsensitiveVolume(const QString& folderPath);

// True if the Adobe sidecar "<base>.xmp" belongs to `imagePath` (see above).
bool ownsAdobeSidecar(const QString& imagePath, const Folder& folder);
// Existing sidecars `imagePath` reads, in reading order (the first one with a rating wins).
// `folder` (optional): a listing of the image's folder.
QStringList readable(const QString& imagePath, const Folder* folder = nullptr);
// The sidecar a rating of `imagePath` is written to: the first readable one, else
// "<file>.xmp" if the Adobe name is not its own, else "<base>.xmp".
QString forWriting(const QString& imagePath, const Folder* folder = nullptr);
// The existing sidecars that go along when `members` (files of one folder, e.g. both
// files of a RAW+JPEG pair) are moved, copied or trashed together: every "<file>.xmp" of
// a member, and "<base>.xmp" if a member owns it and no file outside `members` does.
QStringList travelling(const QStringList& members, const Folder* folder = nullptr);

} // namespace lf::sidecars
