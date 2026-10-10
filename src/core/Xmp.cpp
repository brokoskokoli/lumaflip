// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal XMP editing (see Xmp.h).

#include "Xmp.h"

#include <QHash>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QVector>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace lf::xmp {

namespace {

const QString kXmp = QString::fromLatin1(kNsXmp);
const QString kRdf = QString::fromLatin1(kNsRdf);
const QString kLf = QString::fromLatin1(kNsLumaflip);

struct Attr {
    QString qname;
    qsizetype start = 0;      // first character of the name
    qsizetype valueStart = 0; // inside the quotes
    qsizetype valueEnd = 0;
    qsizetype end = 0; // behind the closing quote
};

struct Element {
    QString nsUri, local;
    qsizetype tagStart = 0, tagEnd = 0; // start tag "<…>" (or "<…/>")
    qsizetype endStart = 0, endEnd = 0; // end tag "</…>" (== tagEnd for "<…/>")
    bool selfClosing = false;
    bool hasChildren = false;
    int parent = -1;
    QHash<QString, QString> scope; // prefix -> namespace in scope (after own declarations)
    QXmlStreamAttributes attributes;
    QString text; // character content (only meaningful without children)
};

struct Doc {
    QVector<Element> elements;
};

bool isSpace(QChar c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r';
}

// Locates the attributes in the text of a well-formed start tag.
QVector<Attr> scanAttributes(const QString& xml, qsizetype tagStart, qsizetype tagEnd) {
    QVector<Attr> out;
    qsizetype p = tagStart + 1;
    while (p < tagEnd && !isSpace(xml[p]) && xml[p] != u'/' && xml[p] != u'>')
        ++p; // element name
    for (;;) {
        while (p < tagEnd && isSpace(xml[p]))
            ++p;
        if (p >= tagEnd || xml[p] == u'/' || xml[p] == u'>')
            break;
        Attr a;
        a.start = p;
        while (p < tagEnd && !isSpace(xml[p]) && xml[p] != u'=')
            ++p;
        a.qname = xml.mid(a.start, p - a.start);
        while (p < tagEnd && isSpace(xml[p]))
            ++p;
        if (p >= tagEnd || xml[p] != u'=')
            break;
        ++p;
        while (p < tagEnd && isSpace(xml[p]))
            ++p;
        if (p >= tagEnd || (xml[p] != u'"' && xml[p] != u'\''))
            break;
        const QChar quote = xml[p];
        a.valueStart = ++p;
        while (p < tagEnd && xml[p] != quote)
            ++p;
        a.valueEnd = p;
        a.end = std::min(p + 1, tagEnd);
        p = a.end;
        out.append(a);
    }
    return out;
}

// Tokenizes `xml`; false if it is not well-formed.
bool parse(const QString& xml, Doc* doc) {
    QXmlStreamReader r(xml);
    QVector<int> stack;
    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType t = r.readNext();
        const qsizetype end = qsizetype(r.characterOffset());
        if (t == QXmlStreamReader::Invalid)
            break;
        if (t == QXmlStreamReader::StartElement) {
            Element e;
            // The offset after a start tag is exact (other tokens may include one character
            // of look-ahead); '<' cannot occur inside a start tag, so the tag begins at the
            // last '<' before its end.
            const qsizetype start = xml.lastIndexOf(QLatin1Char('<'), end - 1);
            if (start < 0 || end > xml.size() || xml[end - 1] != u'>')
                return false;
            e.tagStart = start;
            e.tagEnd = end;
            e.selfClosing = end >= 2 && xml[end - 2] == u'/' && xml[end - 1] == u'>';
            e.nsUri = r.namespaceUri().toString();
            e.local = r.name().toString();
            e.attributes = r.attributes();
            e.parent = stack.isEmpty() ? -1 : stack.last();
            if (e.parent >= 0) {
                e.scope = doc->elements[e.parent].scope;
                doc->elements[e.parent].hasChildren = true;
            }
            for (const QXmlStreamNamespaceDeclaration& ns : r.namespaceDeclarations())
                e.scope.insert(ns.prefix().toString(), ns.namespaceUri().toString());
            doc->elements.append(std::move(e));
            stack.append(int(doc->elements.size() - 1));
        } else if (t == QXmlStreamReader::EndElement) {
            if (stack.isEmpty())
                return false;
            Element& e = doc->elements[stack.takeLast()];
            if (e.selfClosing) {
                e.endStart = e.endEnd = e.tagEnd;
            } else {
                const qsizetype close = xml.lastIndexOf(QLatin1Char('>'), end - 1);
                e.endStart = xml.lastIndexOf(QLatin1String("</"), close);
                e.endEnd = close + 1;
                if (e.endStart < e.tagEnd)
                    return false;
            }
        } else if (t == QXmlStreamReader::Characters && !stack.isEmpty()) {
            doc->elements[stack.last()].text += r.text();
        }
    }
    return !r.hasError() && stack.isEmpty() && !doc->elements.isEmpty();
}

bool isDescription(const Element& e) {
    return e.nsUri == kRdf && e.local == QLatin1String("Description");
}

struct Edit {
    qsizetype start = 0, end = 0;
    QString text;
};

QString applyEdits(QString xml, QVector<Edit> edits) {
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
    for (const Edit& e : edits)
        xml.replace(e.start, e.end - e.start, e.text);
    return xml;
}

QString escapeAttr(const QString& v) {
    QString s = v;
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

QString escapeText(const QString& v) {
    QString s = v;
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return s;
}

// Start of the whitespace run that precedes `pos`.
qsizetype leadingSpace(const QString& xml, qsizetype pos) {
    while (pos > 0 && isSpace(xml[pos - 1]))
        --pos;
    return pos;
}

// Prefix bound to `nsUri` in `scope` (not the default namespace), empty if none.
QString prefixFor(const QHash<QString, QString>& scope, const QString& nsUri) {
    QString best;
    for (auto it = scope.cbegin(); it != scope.cend(); ++it)
        if (!it.key().isEmpty() && it.value() == nsUri && (best.isEmpty() || it.key() < best))
            best = it.key();
    return best;
}

// Separator used between the attributes of a start tag (keeps one-per-line layouts).
QString attributeSeparator(const QString& xml, const Element& e, const QVector<Attr>& attrs) {
    if (attrs.isEmpty())
        return QStringLiteral(" ");
    const Attr& last = attrs.last();
    const qsizetype ws = leadingSpace(xml, last.start);
    const QString sep = xml.mid(ws, last.start - ws);
    return sep.isEmpty() || ws < e.tagStart ? QStringLiteral(" ") : sep;
}

} // namespace

bool isWellFormed(const QString& xml) {
    Doc d;
    return parse(xml, &d);
}

std::optional<QString> property(const QString& xml, const QString& nsUri, const QString& localName,
                                bool* wellFormed) {
    Doc doc;
    const bool ok = parse(xml, &doc);
    if (wellFormed)
        *wellFormed = ok;
    if (!ok)
        return std::nullopt;
    for (int i = 0; i < doc.elements.size(); ++i) {
        const Element& e = doc.elements[i];
        if (isDescription(e)) {
            for (const QXmlStreamAttribute& a : e.attributes)
                if (a.namespaceUri() == nsUri && a.name() == localName)
                    return a.value().toString();
        } else if (e.parent >= 0 && isDescription(doc.elements[e.parent]) && e.nsUri == nsUri &&
                   e.local == localName && !e.hasChildren) {
            return e.text.trimmed();
        }
    }
    return std::nullopt;
}

std::optional<QString> setProperty(const QString& xml, const QString& nsUri, const QString& preferredPrefix,
                                   const QString& localName, const std::optional<QString>& value) {
    Doc doc;
    if (!parse(xml, &doc))
        return std::nullopt;
    QVector<Edit> edits;
    bool written = false;
    int firstDescription = -1;
    int rdfRoot = -1;
    for (int i = 0; i < doc.elements.size(); ++i) {
        const Element& e = doc.elements[i];
        if (e.nsUri == kRdf && e.local == QLatin1String("RDF") && rdfRoot < 0)
            rdfRoot = i;
        if (isDescription(e)) {
            if (firstDescription < 0)
                firstDescription = i;
            for (const Attr& a : scanAttributes(xml, e.tagStart, e.tagEnd)) {
                const qsizetype colon = a.qname.indexOf(QLatin1Char(':'));
                if (colon <= 0 || a.qname.mid(colon + 1) != localName)
                    continue;
                if (e.scope.value(a.qname.left(colon)) != nsUri)
                    continue;
                if (value && !written) {
                    edits.append({a.valueStart, a.valueEnd, escapeAttr(*value)});
                    written = true;
                } else {
                    edits.append({leadingSpace(xml, a.start), a.end, QString()});
                }
            }
        } else if (e.parent >= 0 && isDescription(doc.elements[e.parent]) && e.nsUri == nsUri &&
                   e.local == localName) {
            if (value && !written) {
                // Only the content changes: the start tag keeps its attributes and namespace
                // declarations (`<xmp:Rating xmlns:xmp="…">`, `<Rating xmlns="…">`).
                if (e.selfClosing) {
                    qsizetype qe = e.tagStart + 1;
                    while (qe < e.tagEnd && !isSpace(xml[qe]) && xml[qe] != u'/' && xml[qe] != u'>')
                        ++qe;
                    const QString q = xml.mid(e.tagStart + 1, qe - e.tagStart - 1);
                    edits.append(
                        {e.tagEnd - 2, e.tagEnd, QStringLiteral(">%1</%2>").arg(escapeText(*value), q)});
                } else {
                    edits.append({e.tagEnd, e.endStart, escapeText(*value)});
                }
                written = true;
            } else {
                edits.append({leadingSpace(xml, e.tagStart), e.endEnd, QString()});
            }
        }
    }
    if (value && !written) {
        if (firstDescription >= 0) {
            const Element& d = doc.elements[firstDescription];
            const QVector<Attr> attrs = scanAttributes(xml, d.tagStart, d.tagEnd);
            QString prefix = prefixFor(d.scope, nsUri);
            const QString sep = attributeSeparator(xml, d, attrs);
            QString insert;
            if (prefix.isEmpty()) {
                prefix = preferredPrefix;
                for (int n = 1; d.scope.contains(prefix) && d.scope.value(prefix) != nsUri; ++n)
                    prefix = preferredPrefix + QString::number(n);
                insert += sep + QStringLiteral("xmlns:%1=\"%2\"").arg(prefix, escapeAttr(nsUri));
            }
            insert += sep + QStringLiteral("%1:%2=\"%3\"").arg(prefix, localName, escapeAttr(*value));
            const qsizetype at = d.selfClosing ? d.tagEnd - 2 : d.tagEnd - 1;
            // Keep "attr/>" style: insert right behind the last attribute.
            const qsizetype pos = attrs.isEmpty() ? at : std::min(at, attrs.last().end);
            edits.append({pos, pos, insert});
        } else if (rdfRoot >= 0) {
            const Element& rdf = doc.elements[rdfRoot];
            const QString rdfPrefix = prefixFor(rdf.scope, kRdf);
            if (rdfPrefix.isEmpty())
                return std::nullopt;
            const QString desc =
                QStringLiteral("<%1:Description %1:about=\"\" xmlns:%2=\"%3\" %2:%4=\"%5\"/>")
                    .arg(rdfPrefix, preferredPrefix, escapeAttr(nsUri), localName, escapeAttr(*value));
            if (rdf.selfClosing) {
                const QString open =
                    xml.mid(rdf.tagStart, rdf.tagEnd - rdf.tagStart - 2).trimmed() + QLatin1Char('>');
                edits.append(
                    {rdf.tagStart, rdf.tagEnd, open + desc + QStringLiteral("</%1:RDF>").arg(rdfPrefix)});
            } else {
                edits.append({rdf.endStart, rdf.endStart, desc});
            }
        } else {
            return std::nullopt;
        }
    }
    return applyEdits(xml, edits);
}

// ---- ratings ----

RatingValues ratingValues(const Rating& rating) {
    RatingValues v;
    if (rating.isReject())
        v.rating = QStringLiteral("-1");
    else if (rating.stars > 0)
        v.rating = QString::number(std::clamp(rating.stars, 1, 5));
    if (rating.isPick())
        v.pick = QStringLiteral("1");
    else if (rating.isReject())
        v.pick = QStringLiteral("-1");
    if (rating.isReject() && rating.stars > 0)
        v.stars = QString::number(std::clamp(rating.stars, 1, 5));
    return v;
}

std::optional<Rating> ratingFromValues(const RatingValues& v) {
    if (!v.rating && !v.pick)
        return std::nullopt;
    auto keptStars = [&] {
        bool isNum = false;
        const int s = v.stars ? v.stars->trimmed().toInt(&isNum) : 0;
        return isNum ? std::clamp(s, 0, 5) : 0;
    };
    Rating r;
    if (v.rating) {
        bool isNum = false;
        const double d = v.rating->trimmed().toDouble(&isNum);
        if (isNum && d < 0) {
            r.flag = PickFlag::Reject;
            r.stars = keptStars();
            return r;
        }
        r.stars = isNum ? std::clamp(int(std::floor(d)), 0, 5) : 0;
        if (v.pick && v.pick->trimmed() == QLatin1String("1"))
            r.flag = PickFlag::Pick;
        return r;
    }
    // Only our own flag (e.g. xmp:Rating removed by another program).
    if (v.pick->trimmed() == QLatin1String("1")) {
        r.flag = PickFlag::Pick;
    } else if (v.pick->trimmed() == QLatin1String("-1")) {
        r.flag = PickFlag::Reject;
        r.stars = keptStars();
    }
    return r;
}

std::optional<Rating> readRating(const QString& xml) {
    bool ok = false;
    RatingValues v;
    v.rating = property(xml, kXmp, QStringLiteral("Rating"), &ok);
    if (!ok)
        return std::nullopt;
    v.pick = property(xml, kLf, QStringLiteral("Pick"));
    v.stars = property(xml, kLf, QStringLiteral("Stars"));
    return ratingFromValues(v);
}

namespace {

// Removes declarations of `nsUri` that nothing uses any more (our namespace after the last
// Lumaflip property went), so that clearing a rating restores the original text.
QString dropUnusedNamespace(const QString& xml, const QString& nsUri) {
    Doc doc;
    if (!parse(xml, &doc))
        return xml;
    for (const Element& e : doc.elements) {
        if (e.nsUri == nsUri)
            return xml;
        for (const QXmlStreamAttribute& a : e.attributes)
            if (a.namespaceUri() == nsUri)
                return xml;
    }
    QVector<Edit> edits;
    for (const Element& e : doc.elements)
        for (const Attr& a : scanAttributes(xml, e.tagStart, e.tagEnd))
            if (a.qname.startsWith(QLatin1String("xmlns:")) &&
                xml.mid(a.valueStart, a.valueEnd - a.valueStart) == nsUri)
                edits.append({leadingSpace(xml, a.start), a.end, QString()});
    return edits.isEmpty() ? xml : applyEdits(xml, edits);
}

} // namespace

std::optional<QString> writeRating(const QString& xml, const Rating& rating) {
    QString text = xml;
    if (text.trimmed().isEmpty())
        return newSidecar(rating);
    const RatingValues v = ratingValues(rating);
    const QString lf = QString::fromLatin1(kPrefixLumaflip);
    auto step = [&](const QString& ns, const QString& prefix, const QString& name,
                    const std::optional<QString>& value) {
        if (const auto out = setProperty(text, ns, prefix, name, value)) {
            text = *out;
            return true;
        }
        return false;
    };
    if (!step(kXmp, QStringLiteral("xmp"), QStringLiteral("Rating"), v.rating) ||
        !step(kLf, lf, QStringLiteral("Pick"), v.pick) || !step(kLf, lf, QStringLiteral("Stars"), v.stars))
        return std::nullopt;
    if (!v.pick && !v.stars)
        text = dropUnusedNamespace(text, kLf);
    return text;
}

QString newSidecar(const Rating& rating) {
    const QString empty =
        QStringLiteral("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Lumaflip\">\n"
                       " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                       "  <rdf:Description rdf:about=\"\"\n"
                       "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n"
                       "  </rdf:Description>\n"
                       " </rdf:RDF>\n"
                       "</x:xmpmeta>\n");
    if (rating.isEmpty())
        return empty;
    return writeRating(empty, rating).value_or(empty);
}

QString wrapPacket(const QString& xml) {
    if (xml.contains(QLatin1String("<?xpacket")))
        return xml;
    return QStringLiteral("<?xpacket begin=\"\uFEFF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n") + xml +
           QStringLiteral("<?xpacket end=\"w\"?>");
}

QString decode(const QByteArray& bytes, Encoding* encoding) {
    if (encoding)
        *encoding = Encoding::Utf8;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        if (encoding)
            *encoding = Encoding::Utf8Bom;
        return QString::fromUtf8(bytes.mid(3));
    }
    if (bytes.startsWith("\xFE\xFF") || bytes.startsWith("\xFF\xFE")) {
        const bool be = bytes.startsWith("\xFE\xFF");
        if (encoding)
            *encoding = be ? Encoding::Utf16BE : Encoding::Utf16LE;
        auto dec = QStringDecoder(be ? QStringDecoder::Utf16BE : QStringDecoder::Utf16LE);
        return dec.decode(bytes.mid(2));
    }
    return QString::fromUtf8(bytes);
}

QString decode(const QByteArray& bytes, bool* hadBom) {
    Encoding e = Encoding::Utf8;
    const QString text = decode(bytes, &e);
    if (hadBom)
        *hadBom = e == Encoding::Utf8Bom;
    return text;
}

QByteArray encode(const QString& text, Encoding encoding) {
    switch (encoding) {
    case Encoding::Utf8:
        return text.toUtf8();
    case Encoding::Utf8Bom:
        return QByteArray("\xEF\xBB\xBF") + text.toUtf8();
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        // Same encoding as the file had (its XML declaration may say "UTF-16").
        const bool be = encoding == Encoding::Utf16BE;
        auto enc = QStringEncoder(be ? QStringEncoder::Utf16BE : QStringEncoder::Utf16LE);
        return QByteArray(be ? "\xFE\xFF" : "\xFF\xFE") + QByteArray(enc.encode(text));
    }
    }
    return text.toUtf8();
}

QByteArray encode(const QString& text, bool withBom) {
    return encode(text, withBom ? Encoding::Utf8Bom : Encoding::Utf8);
}

QString findPacket(const QByteArray& bytes) {
    for (const char* open : {"<x:xmpmeta", "<x:xapmeta"}) {
        const qsizetype start = bytes.indexOf(open);
        if (start < 0)
            continue;
        const QByteArray close = QByteArray("</") + QByteArray(open + 1) + '>';
        const qsizetype end = bytes.indexOf(close, start);
        if (end < 0)
            continue;
        return QString::fromUtf8(bytes.mid(start, end + close.size() - start));
    }
    return {};
}

} // namespace lf::xmp
