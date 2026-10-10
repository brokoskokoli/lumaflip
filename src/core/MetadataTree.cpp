// SPDX-License-Identifier: GPL-3.0-or-later
#include "MetadataTree.h"

#include "NameFilter.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <climits>
#include <optional>

namespace lf::metadatatree {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::MetadataTree)
};

constexpr int kInlineListMax = 16; // short scalar lists become one value
constexpr int kBytesPreview = 32;  // bytes shown of binary values

bool isScalar(const QVariant& v) {
    const int t = v.typeId();
    return t != QMetaType::QVariantMap && t != QMetaType::QVariantList && t != QMetaType::QVariantHash &&
           t != QMetaType::QStringList;
}

QString scalarText(const QVariant& v) {
    if (v.typeId() == QMetaType::QByteArray) {
        const QByteArray b = v.toByteArray();
        QString hex = QString::fromLatin1(b.left(kBytesPreview).toHex(' '));
        if (b.size() > kBytesPreview)
            hex += QStringLiteral(" …");
        //: binary metadata value: its size and the first bytes in hex, e.g. "<2 Bytes: 01 02>"
        return Tr::tr("<%n Bytes: %1>", nullptr, int(std::min<qsizetype>(b.size(), INT_MAX))).arg(hex);
    }
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float)
        return QString::number(v.toDouble(), 'g', 10);
    return v.toString();
}

bool matches(const MetadataNode& n, const QStringList& terms) {
    const QString text = n.key + u' ' + n.value;
    return namefilter::matchesTerms(text, terms);
}

std::optional<MetadataNode> filterNode(const MetadataNode& n, const QStringList& terms) {
    if (matches(n, terms))
        return n; // with all children
    MetadataNode out{n.key, n.value, {}};
    for (const MetadataNode& c : n.children)
        if (auto f = filterNode(c, terms))
            out.children.append(std::move(*f));
    if (out.children.isEmpty())
        return std::nullopt;
    return out;
}

void appendText(const MetadataNode& n, int depth, QStringList& lines) {
    const QString indent(depth * 2, u' ');
    if (n.isGroup()) {
        lines << indent + n.key + u':';
        for (const MetadataNode& c : n.children)
            appendText(c, depth + 1, lines);
    } else {
        lines << indent + n.key + QStringLiteral(": ") + n.value;
    }
}

} // namespace

MetadataNode fromVariant(const QString& key, const QVariant& value) {
    MetadataNode n;
    n.key = key;
    switch (value.typeId()) {
    case QMetaType::QVariantMap: {
        const QVariantMap m = value.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it) // QMap: sorted by key
            n.children.append(fromVariant(it.key(), it.value()));
        if (n.children.isEmpty())
            n.value = QStringLiteral("{}");
        break;
    }
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        const QVariantList l = value.toList();
        const bool inlineList =
            l.size() <= kInlineListMax && std::all_of(l.cbegin(), l.cend(), [](const QVariant& v) {
                return isScalar(v) && v.typeId() != QMetaType::QByteArray;
            });
        if (inlineList) {
            QStringList parts;
            for (const QVariant& v : l)
                parts << scalarText(v);
            n.value = parts.join(QStringLiteral(", "));
        } else {
            for (int i = 0; i < l.size(); ++i)
                n.children.append(fromVariant(QStringLiteral("[%1]").arg(i), l[i]));
        }
        break;
    }
    default:
        n.value = scalarText(value);
        break;
    }
    return n;
}

MetadataNode filtered(const MetadataNode& root, const QString& query) {
    const QStringList terms = namefilter::terms(query);
    if (terms.isEmpty())
        return root;
    MetadataNode out{root.key, root.value, {}};
    for (const MetadataNode& c : root.children)
        if (auto f = filterNode(c, terms))
            out.children.append(std::move(*f));
    return out;
}

int leafCount(const MetadataNode& node) {
    if (!node.isGroup())
        return 1;
    int n = 0;
    for (const MetadataNode& c : node.children)
        n += leafCount(c);
    return n;
}

const MetadataNode* find(const MetadataNode& root, const QString& keyPath) {
    const MetadataNode* n = &root;
    for (const QString& part : keyPath.split(u'/', Qt::SkipEmptyParts)) {
        const auto it = std::find_if(n->children.cbegin(), n->children.cend(),
                                     [&](const MetadataNode& c) { return c.key == part; });
        if (it == n->children.cend())
            return nullptr;
        n = &*it;
    }
    return n;
}

QString toText(const MetadataNode& node, bool includeSelf) {
    QStringList lines;
    if (includeSelf) {
        appendText(node, 0, lines);
    } else {
        for (const MetadataNode& c : node.children)
            appendText(c, 0, lines);
    }
    return lines.join(u'\n');
}

} // namespace lf::metadatatree
