// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): reading archives (ZIP/CBZ, RAR/CBR, 7z/CB7) and PDF documents like a
// folder. Member paths: VirtualPath.h.
//
// Readers:
//  - ZIP/CBZ: the own bounds-checked ZipReader (stored/deflate).
//  - RAR/CBR, 7z/CB7 (and ZIP variants ZipReader rejects, e.g. ZIP64): libarchive. macOS
//    ships it as /usr/lib/libarchive.2.dylib (the SDK has the .tbd but no header; the few
//    functions used are declared in LibArchive.cpp). Builds without it report these formats
//    as unsupported (libArchiveAvailable()).
//  - PDF: the pages (PdfDocument.h), member names from vpath::pdfPageMember().
//
// Safety (archives come from anywhere): every member is read with a hard limit of
// kMaxMemberBytes produced bytes - whatever size the archive claims (zip bombs, sizes that
// lie); at most kMaxMembers names are listed; names with ".." or absolute paths are dropped.
// With a decoder helper (DecoderHelper.h) list() and read() run in that sandboxed process,
// so a parser bug in libarchive can only take down the helper; without one they run here.
//
// Thread-safe (every call opens the archive itself); blocking: call on worker threads.

#include "FileEntry.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

namespace lf {

class NameCollator;

namespace archive {

// Upper bound of one extracted member (the helper protocol carries it in one frame).
inline constexpr qint64 kMaxMemberBytes = qint64(60) << 20; // 60 MiB
inline constexpr int kMaxMembers = 100000;
// Upper bound of the bytes libarchive may decompress while looking for one member (solid
// RAR/7z archives decompress everything before it): a decompression bomb ends here.
inline constexpr qint64 kMaxScanBytes = qint64(4) << 30; // 4 GiB
// Upper bound of the members extracted together with a wanted one (readRun(); the helper
// returns them in one frame, so it stays below kMaxMemberBytes).
inline constexpr qint64 kMaxRunBytes = qint64(32) << 20; // 32 MiB

struct Member {
    QString name;     // path inside the archive ("Kapitel 1/012.jpg")
    qint64 size = -1; // uncompressed size as the archive states it (-1 unknown)
    QDateTime modified;
    friend bool operator==(const Member&, const Member&) = default;
};

// One extracted member (readRun()).
struct Extracted {
    QString name;
    QByteArray data;
};

struct Listing {
    bool ok = false;
    QString error; // user facing
    QVector<Member> members;
};

bool libArchiveAvailable();

// In this process (the helper uses these).
Listing listLocal(const QString& containerPath);
std::optional<QByteArray> readLocal(const QString& containerPath, const QString& member, QString* error);

// The member and, for archives read front to back (RAR, 7z: libarchive), the image members
// that follow it in the same pass, up to kMaxRunBytes together (result[0] = `member`). ZIP
// archives are read member by member (random access): only `member`.
std::optional<QVector<Extracted>> readRunLocal(const QString& containerPath, const QString& member,
                                               QString* error);

// Through the decoder helper if there is one, else listLocal()/readLocal()/readRunLocal().
Listing list(const QString& containerPath);
std::optional<QByteArray> read(const QString& containerPath, const QString& member, QString* error);
std::optional<QVector<Extracted>> readRun(const QString& containerPath, const QString& member,
                                          QString* error);

// The entries a collection shows for a container: image members only
// (vpath::isImageMemberName; every PDF page), FileEntry::path = member path, name = the
// member's file name, size/modified from the listing (the container's date if unknown). In
// Finder name order of the whole member path (folders inside the archive stay together).
QVector<FileEntry> imageEntries(const QString& containerPath, const Listing& listing,
                                const NameCollator& collator);
// Finder order of member paths (natural numbers, case-insensitive), used by imageEntries().
void sortMemberEntries(QVector<FileEntry>& entries, const NameCollator& collator, bool ascending = true);

// The user facing messages.
QString messageUnsupportedArchive(); // no libarchive / unknown format
QString messageMemberTooLarge();
QString messageDamagedArchive();
QString messageNoImages(); // the archive holds no images
QString messageEncryptedArchive(); // password protected
QString messageArchiveTooLarge();  // the member lies beyond kMaxScanBytes

} // namespace archive
} // namespace lf
