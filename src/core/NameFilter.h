// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-NAV-08: type-to-filter by file name. Pure matching logic (no I/O, any thread), used by
// Collection::setNameFilter(). Matching ignores case and diacritics and works on any Unicode
// normalization form: macOS file systems hand out decomposed names (NFD, "a" + U+0308)
// while typed text is composed (NFC, "ä"), and both must match.

#include <QString>
#include <QStringList>

namespace lf::namefilter {

// Comparison form of a text: canonical decomposition, combining marks removed, case folded
// ("Ä" / "ä" / "A" -> "a", "ß" -> "ss").
QString fold(const QString& text);
// The query split into folded terms (whitespace separates terms; empty terms dropped).
QStringList terms(const QString& query);
// True if every term of `query` occurs in the file name (empty query: everything matches).
bool matches(const QString& fileName, const QString& query);
// Same with pre-folded terms (many names, one query).
bool matchesTerms(const QString& fileName, const QStringList& foldedTerms);
// Same with an already folded name (fold() computed once per file).
bool matchesFolded(const QString& foldedName, const QStringList& foldedTerms);

} // namespace lf::namefilter
