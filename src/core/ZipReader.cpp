// SPDX-License-Identifier: GPL-3.0-or-later
#include "ZipReader.h"

#include <QCoreApplication>

#include <algorithm>
#include <zlib.h>

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::ZipReader)
};

quint16 u16(const char* p) {
    const auto* b = reinterpret_cast<const uchar*>(p);
    return quint16(b[0] | (b[1] << 8));
}
quint32 u32(const char* p) {
    const auto* b = reinterpret_cast<const uchar*>(p);
    return quint32(b[0]) | (quint32(b[1]) << 8) | (quint32(b[2]) << 16) | (quint32(b[3]) << 24);
}

constexpr quint32 kEocdSig = 0x06054b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kLocalSig = 0x04034b50;
constexpr qint64 kEocdSize = 22;
constexpr qint64 kCentralSize = 46;
constexpr qint64 kLocalSize = 30;

} // namespace

ZipReader::ZipReader(const QString& path) : m_file(path) {
    if (!m_file.open(QIODevice::ReadOnly)) {
        fail(Tr::tr("Die Datei kann nicht gelesen werden."));
        return;
    }
    m_size = m_file.size();
    m_valid = parseDirectory();
}

ZipReader::ZipReader(QByteArray data) : m_data(std::move(data)), m_inMemory(true) {
    m_size = m_data.size();
    m_valid = parseDirectory();
}

bool ZipReader::fail(const QString& error) {
    m_error = error;
    return false;
}

bool ZipReader::readAt(qint64 offset, qint64 size, QByteArray* out) {
    if (offset < 0 || size < 0 || offset > m_size || size > m_size - offset)
        return false;
    if (m_inMemory) {
        *out = m_data.mid(offset, size);
        return true;
    }
    if (!m_file.seek(offset))
        return false;
    *out = m_file.read(size);
    return out->size() == size;
}

bool ZipReader::parseDirectory() {
    const QString damaged = Tr::tr("Das ZIP-Archiv ist beschädigt.");
    if (m_size < kEocdSize)
        return fail(damaged);
    // The end record sits in the last 22 + 65535 (comment) bytes.
    const qint64 tail = std::min<qint64>(m_size, kEocdSize + 0xFFFF);
    QByteArray buf;
    if (!readAt(m_size - tail, tail, &buf))
        return fail(damaged);
    qint64 eocd = -1;
    for (qint64 i = buf.size() - kEocdSize; i >= 0; --i) {
        if (u32(buf.constData() + i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return fail(damaged);
    const char* e = buf.constData() + eocd;
    const quint16 disk = u16(e + 4), cdDisk = u16(e + 6);
    const quint16 entries = u16(e + 10);
    const quint32 cdSize = u32(e + 12), cdOffset = u32(e + 16);
    if (disk != 0 || cdDisk != 0 || cdOffset == 0xFFFFFFFFu || entries == 0xFFFF)
        return fail(Tr::tr("ZIP-Archive dieser Art (mehrteilig, ZIP64) werden nicht unterstützt."));
    QByteArray cd;
    if (!readAt(cdOffset, cdSize, &cd))
        return fail(damaged);
    qint64 pos = 0;
    for (int i = 0; i < entries && i < kMaxMembers; ++i) {
        if (pos + kCentralSize > cd.size() || u32(cd.constData() + pos) != kCentralSig)
            return fail(damaged);
        const char* c = cd.constData() + pos;
        Member m;
        const quint16 flags = u16(c + 8);
        m.method = u16(c + 10);
        m.compressedSize = u32(c + 20);
        m.uncompressedSize = u32(c + 24);
        const quint16 nameLen = u16(c + 28), extraLen = u16(c + 30), commentLen = u16(c + 32);
        m.localHeaderOffset = u32(c + 42);
        const qint64 next = pos + kCentralSize + nameLen + extraLen + commentLen;
        if (next > cd.size())
            return fail(damaged);
        const QByteArray rawName = cd.mid(pos + kCentralSize, nameLen);
        m.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        pos = next;
        if (flags & 0x1) {
            if (m_encrypted.size() < kMaxMembers)
                m_encrypted.insert(m.name);
            continue; // encrypted: never readable
        }
        if (!m_members.contains(m.name))
            m_order << m.name;
        m_members.insert(m.name, m);
    }
    return true;
}

QStringList ZipReader::names() const {
    return m_order;
}

std::optional<ZipReader::Member> ZipReader::member(const QString& name) const {
    const auto it = m_members.constFind(name);
    if (it == m_members.cend())
        return std::nullopt;
    return *it;
}

std::optional<QByteArray> ZipReader::read(const QString& name, qint64 maxBytes) {
    const QString damaged = Tr::tr("Das ZIP-Archiv ist beschädigt.");
    if (!m_valid)
        return std::nullopt;
    const auto m = member(name);
    if (!m) {
        fail(Tr::tr("„%1“ fehlt im Archiv.").arg(name));
        return std::nullopt;
    }
    QByteArray local;
    if (!readAt(m->localHeaderOffset, kLocalSize, &local) || u32(local.constData()) != kLocalSig) {
        fail(damaged);
        return std::nullopt;
    }
    const qint64 dataOffset =
        qint64(m->localHeaderOffset) + kLocalSize + u16(local.constData() + 26) + u16(local.constData() + 28);
    const qint64 limit = std::min<qint64>(maxBytes > 0 ? maxBytes : kMaxMemberBytes, kMaxMemberBytes);
    if (m->uncompressedSize > kMaxMemberBytes && maxBytes <= 0) {
        fail(Tr::tr("„%1“ ist zu groß.").arg(name));
        return std::nullopt;
    }
    // Deflate expands at most ~1032:1: a directory that claims more is damaged (and must not
    // make us allocate gigabytes for a tiny member).
    const qint64 plausible =
        m->method == 8 ? qint64(m->compressedSize) * 1032 + 1024 : qint64(m->compressedSize);
    const qint64 want = std::min<qint64>({qint64(m->uncompressedSize), limit, plausible});
    if (m->method == 0) {
        QByteArray out;
        if (m->compressedSize != m->uncompressedSize || !readAt(dataOffset, want, &out)) {
            fail(damaged);
            return std::nullopt;
        }
        return out;
    }
    if (m->method != 8) {
        fail(Tr::tr("Die Kompressionsart von „%1“ wird nicht unterstützt.").arg(name));
        return std::nullopt;
    }
    // Only the compressed bytes that can produce `want` are read (stored deflate blocks add
    // 5 bytes per 64 KiB at most): a directory claiming gigabytes of compressed data does not
    // make us read them into memory.
    qint64 needed = std::min<qint64>(qint64(m->compressedSize), want + want / 1024 + 4096);
    if (needed < qint64(m->compressedSize))
        needed = std::min(needed, m_size - dataOffset); // a claimed size past the end: what is there
    QByteArray in;
    if (!readAt(dataOffset, needed, &in)) {
        fail(damaged);
        return std::nullopt;
    }
    QByteArray out;
    out.resize(want);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        fail(damaged);
        return std::nullopt;
    }
    zs.next_in = reinterpret_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    int rc = Z_OK;
    while (zs.avail_out > 0 && rc == Z_OK)
        rc = inflate(&zs, Z_NO_FLUSH);
    const qint64 produced = qint64(zs.total_out);
    inflateEnd(&zs);
    // Z_BUF_ERROR with a full buffer: we asked for fewer bytes than the member holds.
    const bool full = produced == want;
    if (!(rc == Z_STREAM_END || (full && (rc == Z_OK || rc == Z_BUF_ERROR)))) {
        fail(damaged);
        return std::nullopt;
    }
    if (!full && want == qint64(m->uncompressedSize)) {
        fail(damaged); // shorter than the directory says
        return std::nullopt;
    }
    out.truncate(produced);
    return out;
}

} // namespace lf
