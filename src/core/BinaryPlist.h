// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable, defensive parser for Apple binary property lists ("bplist00").
// Used for the blobs inside .DS_Store files (lsvC, lsvp, icvp, ...; SPEC 6, F-SORT-08).
//
// Mapping to QVariant:
//   dict   -> QVariantMap (non-string keys are converted with QVariant::toString())
//   array  -> QVariantList (sets too)
//   string -> QString (ASCII, UTF-16BE, UTF-8)
//   int    -> qint64 (128-bit ints: low 64 bits), uint64 > INT64_MAX -> quint64
//   real   -> double
//   bool   -> bool
//   date   -> QDateTime (UTC)
//   data   -> QByteArray
//   uid    -> lf::PlistUid
//   null   -> invalid QVariant
//
// The parser never crashes and never reads out of bounds: every offset, length and
// reference is checked; cycles and excessive nesting are rejected.

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <optional>

namespace lf {

struct PlistUid {
    quint64 value = 0;
    friend bool operator==(const PlistUid&, const PlistUid&) = default;
};

namespace bplist {

// True if `data` starts with the "bplist" magic.
bool looksLikeBinaryPlist(const QByteArray& data);

// Parses a complete binary plist. Returns nullopt (and sets `error`) on malformed input.
std::optional<QVariant> parse(const QByteArray& data, QString* error = nullptr);

} // namespace bplist
} // namespace lf

Q_DECLARE_METATYPE(lf::PlistUid)
