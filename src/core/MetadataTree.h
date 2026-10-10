// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Complete raw metadata of an image as a tree (F-INFO-05): the groups the system reader
// provides (macOS ImageIO: Exif, ExifAux, TIFF, GPS, IPTC, MakerNote dictionaries such as
// "MakerCanon", the XMP tree by namespace prefix …) with their technical key names and
// values. For the "Alle Metadaten" view with search and copy.

#include <QList>
#include <QString>
#include <QVariant>

namespace lf {

struct MetadataNode {
    QString key;   // technical name ("Exif", "FNumber", "xmp:Rating", "[0]")
    QString value; // display text of a leaf; empty for groups
    QList<MetadataNode> children;

    bool isGroup() const { return !children.isEmpty(); }
    friend bool operator==(const MetadataNode&, const MetadataNode&) = default;
};

namespace metadatatree {

// Builds a node from nested QVariantMap / QVariantList / scalar values (maps sorted by key,
// lists as "[i]" children - short lists of scalars become one "a, b, c" value; byte arrays
// as "<n Bytes: hex…>").
MetadataNode fromVariant(const QString& key, const QVariant& value);
// The tree reduced to rows whose key or value contains every word of `query` (case and
// diacritics ignored); a group stays if it or any descendant matches (a matching group
// keeps all its children). Empty query: the tree as it is.
MetadataNode filtered(const MetadataNode& root, const QString& query);
// Number of leaves (values).
int leafCount(const MetadataNode& node);
// Finds a leaf by key path ("Exif/FNumber"); nullptr if missing.
const MetadataNode* find(const MetadataNode& root, const QString& keyPath);
// Clipboard text: one "key: value" line per leaf, groups as "key:" headers, children
// indented by two spaces per level. `includeSelf`: the node's own line comes first.
QString toText(const MetadataNode& node, bool includeSelf = true);

} // namespace metadatatree

// Platform reader (blocking: worker threads only). Root key = file name; empty children if
// the file has no readable metadata. macOS: ImageIO properties + the XMP tree.
MetadataNode readRawMetadata(const QString& path);

} // namespace lf
