// SPDX-License-Identifier: GPL-3.0-or-later
// XMP in PNG files (see PngXmp.h).

#include "PngXmp.h"

#include <QVector>
#include <QtEndian>

#include <array>

namespace lf::pngxmp {

namespace {

constexpr char kSignature[] = "\x89PNG\r\n\x1a\n";
constexpr qsizetype kSignatureSize = 8;
constexpr char kKeyword[] = "XML:com.adobe.xmp";

struct Chunk {
    qsizetype start = 0; // length field
    quint32 length = 0;
    QByteArray type;
    qsizetype dataStart = 0;
    qsizetype end = 0; // behind the CRC
};

// The chunks of a well-formed PNG; nullopt otherwise.
std::optional<QVector<Chunk>> chunks(const QByteArray& data) {
    if (!isPng(data))
        return std::nullopt;
    QVector<Chunk> out;
    qsizetype p = kSignatureSize;
    while (p < data.size()) {
        if (data.size() - p < 12)
            return std::nullopt;
        Chunk c;
        c.start = p;
        c.length = qFromBigEndian<quint32>(data.constData() + p);
        if (c.length > 0x7fffffffu || qsizetype(c.length) > data.size() - p - 12)
            return std::nullopt;
        c.type = data.mid(p + 4, 4);
        c.dataStart = p + 8;
        c.end = c.dataStart + c.length + 4;
        out.append(c);
        p = c.end;
        if (c.type == "IEND")
            break;
    }
    if (out.isEmpty() || out.first().type != "IHDR" || out.last().type != "IEND")
        return std::nullopt;
    return out;
}

// For an iTXt chunk with our keyword: the text (decompressed), else nullopt.
std::optional<QString> xmpText(const QByteArray& data, const Chunk& c) {
    if (c.type != "iTXt")
        return std::nullopt;
    const QByteArray body = data.mid(c.dataStart, c.length);
    const qsizetype keyEnd = body.indexOf('\0');
    if (keyEnd < 0 || body.left(keyEnd) != kKeyword || body.size() < keyEnd + 3)
        return std::nullopt;
    const bool compressed = body.at(keyEnd + 1) != 0;
    const qsizetype langEnd = body.indexOf('\0', keyEnd + 3);
    if (langEnd < 0)
        return std::nullopt;
    const qsizetype transEnd = body.indexOf('\0', langEnd + 1);
    if (transEnd < 0)
        return std::nullopt;
    QByteArray text = body.mid(transEnd + 1);
    if (compressed) {
        // qUncompress() wants the expected size up front (a hint; it grows the buffer).
        QByteArray withSize(4, '\0');
        qToBigEndian<quint32>(quint32(std::min<qsizetype>(text.size() * 8 + 1024, 64 * 1024 * 1024)),
                              withSize.data());
        text = qUncompress(withSize + text);
    }
    return QString::fromUtf8(text);
}

QByteArray makeChunk(const QByteArray& type, const QByteArray& body) {
    QByteArray out(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), out.data());
    out += type;
    out += body;
    QByteArray crc(4, '\0');
    qToBigEndian<quint32>(crc32(type + body), crc.data());
    out += crc;
    return out;
}

} // namespace

quint32 crc32(const QByteArray& bytes) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xffffffffu;
    for (const char ch : bytes)
        c = table[(c ^ quint8(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

bool isPng(const QByteArray& data) {
    return data.startsWith(QByteArray(kSignature, kSignatureSize));
}

std::optional<QString> read(const QByteArray& data) {
    const auto list = chunks(data);
    if (!list)
        return std::nullopt;
    for (const Chunk& c : *list)
        if (auto text = xmpText(data, c))
            return text;
    return QString();
}

std::optional<QByteArray> withPacket(const QByteArray& data, const QString& packet) {
    const auto list = chunks(data);
    if (!list)
        return std::nullopt;
    // keyword, 0, compression flag 0, method 0, empty language tag, 0, empty translation, 0
    QByteArray body = QByteArray(kKeyword) + QByteArray("\0\0\0\0\0", 5) + packet.toUtf8();
    const QByteArray chunk = makeChunk("iTXt", body);
    QByteArray out = data.left(kSignatureSize);
    bool written = false;
    for (const Chunk& c : *list) {
        const bool ours = xmpText(data, c).has_value();
        if (ours) {
            if (!written)
                out += chunk; // replaced in place
            written = true;
            continue; // a second XMP chunk is dropped
        }
        if (!written && (c.type == "IDAT" || c.type == "IEND")) {
            out += chunk;
            written = true;
        }
        out += data.mid(c.start, c.end - c.start);
    }
    out += data.mid(list->last().end); // bytes behind IEND stay as they were
    return out;
}

} // namespace lf::pngxmp
