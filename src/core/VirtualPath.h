// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): paths of images inside archives (ZIP/CBZ, RAR/CBR, 7z/CB7) and of PDF
// pages, so a container can be browsed like a folder.
//
// A member path is the container's absolute path, the marker "#/" and the member's name
// inside the container: "/Comics/Band 1.cbz#/Kapitel 1/012.jpg". PDF pages are members
// named by their 1-based number with four digits: "/Docs/Plan.pdf#/0003". The marker only
// counts after a container suffix, so ordinary paths are never mistaken for members (a
// folder named "x.cbz#" would be the only clash - not worth a file system check).
//
// Pure string functions, thread-safe.

#include <QString>
#include <QStringList>

#include <optional>

namespace lf::vpath {

inline constexpr char kMarker[] = "#/";

enum class ContainerKind { None, Zip, LibArchive, Pdf };

// Container suffixes (lower case, without dot): zip cbz rar cbr 7z cb7 pdf.
QStringList containerSuffixes();
// Archive suffixes only (no pdf).
QStringList archiveSuffixes();
ContainerKind kindForSuffix(const QString& lowerSuffix);
// Kind of the container file `path` (by its suffix); None for anything else.
ContainerKind containerKind(const QString& path);
bool isContainerPath(const QString& path); // containerKind() != None
// Comic book archives (cbz, cbr, cb7): read right to left on request (F-OPEN-10).
bool isComicArchive(const QString& path);

struct Parts {
    QString container; // absolute path of the archive/PDF
    QString member;    // name inside it ("Kapitel 1/012.jpg", "0003" for PDF page 3)
};
// Splits a member path; nullopt for ordinary paths.
std::optional<Parts> split(const QString& path);
bool isMember(const QString& path);
QString join(const QString& container, const QString& member);
// The container of a member path, else the path itself.
QString containerOf(const QString& path);

// PDF pages: member name of the 0-based page `index` and back (-1 if not a page name).
QString pdfPageMember(int index);
int pdfPageIndex(const QString& member);

// Last component of the member name ("012.jpg"); for PDF pages "Seite 3" in the UI
// language.
QString memberFileName(const QString& path);
// "Band 1.cbz › Kapitel 1/012.jpg" (status bar, title); ordinary paths: the file name.
QString displayName(const QString& path);

// Member names worth showing: images by suffix, no hidden files, no macOS resource forks
// ("__MACOSX/", "._x"), no directories.
bool isImageMemberName(const QString& member);

} // namespace lf::vpath
