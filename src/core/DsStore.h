// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable, fully bounds-checked reader for Finder's .DS_Store files (SPEC 6, F-SORT-08).
//
// Format: 4-byte alignment word (1), "Bud1" buddy allocator header, a root block with the
// block address table and table of contents ("DSDB"), and a B-tree of records sorted by
// (file name, code). Each record: file name (UTF-16BE, length-prefixed), 4-char code,
// 4-char data type and a value of that type.
// References: Mozilla wiki "DS_Store File Format", Mac::Finder::DSStore (Perl),
// the Python ds_store package and the Kaitai Struct spec.

#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVector>
#include <optional>

namespace lf {

struct DsStoreRecord {
    QString fileName; // "." for the folder itself
    QString code;     // e.g. "vstl", "lsvC", "icvp", "Iloc"
    QString type;     // "long", "shor", "bool", "blob", "type", "ustr", "comp", "dutc"
    // long/shor/comp/dutc -> qint64, bool -> bool, blob -> QByteArray,
    // type -> QString (4 chars), ustr -> QString
    QVariant value;
};

namespace dsstore {

// Upper bound for files we are willing to read (real ones are a few KB to a few MB).
constexpr qint64 kMaxFileSize = 64 * 1024 * 1024;

// Parses the complete file contents. Never crashes; returns nullopt on malformed input.
std::optional<QVector<DsStoreRecord>> parse(const QByteArray& data, QString* error = nullptr);

// Reads and parses `path`. Missing/unreadable/oversized files -> nullopt.
std::optional<QVector<DsStoreRecord>> readFile(const QString& path, QString* error = nullptr);

} // namespace dsstore
} // namespace lf
