// SPDX-License-Identifier: GPL-3.0-or-later
#include "DsStore.h"

#include <QFile>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <vector>

namespace lf::dsstore {
namespace {

constexpr int kMaxTreeDepth = 32;
constexpr quint32 kMaxBlocks = 1u << 20;
constexpr int kMaxRecords = 1 << 20;

class Reader {
public:
    explicit Reader(const QByteArray& data)
        : d_(reinterpret_cast<const uchar*>(data.constData())), n_(quint64(data.size())) {}

    std::optional<QVector<DsStoreRecord>> run(QString* error) {
        if (!parseAll()) {
            if (error)
                *error = QString::fromLatin1(err_ ? err_ : "malformed .DS_Store");
            return std::nullopt;
        }
        return std::move(records_);
    }

private:
    const uchar* d_;
    quint64 n_;
    std::vector<quint32> blocks_;
    QVector<DsStoreRecord> records_;
    QSet<quint32> visited_;
    const char* err_ = nullptr;

    bool fail(const char* msg) {
        if (!err_)
            err_ = msg;
        return false;
    }
    bool has(quint64 pos, quint64 len) const { return pos <= n_ && len <= n_ - pos; }
    quint32 u32(quint64 pos) const {
        return (quint32(d_[pos]) << 24) | (quint32(d_[pos + 1]) << 16) | (quint32(d_[pos + 2]) << 8) |
               quint32(d_[pos + 3]);
    }

    // A bounded view on one allocator block.
    struct Block {
        quint64 begin = 0;
        quint64 end = 0;
    };
    bool block(quint32 id, Block& b) {
        if (id >= blocks_.size())
            return fail("block id out of range");
        const quint32 addr = blocks_[id];
        const quint64 off = quint64(addr & ~0x1fu) + 4;
        const quint64 size = quint64(1) << (addr & 0x1fu);
        if (!has(off, size))
            return fail("block outside file");
        b = {off, off + size};
        return true;
    }

    bool parseAll() {
        if (n_ < 36)
            return fail("too short");
        if (u32(0) != 1 || std::memcmp(d_ + 4, "Bud1", 4) != 0)
            return fail("bad magic");
        const quint64 rootOff = quint64(u32(8)) + 4;
        const quint64 rootSize = u32(12);
        if (u32(16) != u32(8))
            return fail("root offset mismatch");
        if (!has(rootOff, rootSize) || rootSize < 8)
            return fail("root block outside file");
        quint64 p = rootOff;
        const quint64 end = rootOff + rootSize;
        const quint32 count = u32(p);
        p += 8; // count + unknown
        if (count > kMaxBlocks)
            return fail("too many blocks");
        const quint64 padded = (quint64(count) + 255) & ~quint64(255);
        if (padded * 4 > end - p)
            return fail("block table truncated");
        blocks_.resize(count);
        for (quint32 i = 0; i < count; ++i)
            blocks_[i] = u32(p + 4 * quint64(i));
        p += padded * 4;
        if (end - p < 4)
            return fail("toc truncated");
        const quint32 tocCount = u32(p);
        p += 4;
        std::optional<quint32> dsdb;
        for (quint32 i = 0; i < tocCount; ++i) {
            if (end - p < 1)
                return fail("toc truncated");
            const quint64 len = d_[p];
            if (end - p < 1 + len + 4)
                return fail("toc truncated");
            const QByteArray name(reinterpret_cast<const char*>(d_ + p + 1), qsizetype(len));
            const quint32 value = u32(p + 1 + len);
            p += 1 + len + 4;
            if (name == "DSDB")
                dsdb = value;
        }
        // Free lists follow; we do not need them.
        if (!dsdb)
            return fail("no DSDB entry");
        Block hdr;
        if (!block(*dsdb, hdr) || hdr.end - hdr.begin < 20)
            return fail("bad DSDB block");
        const quint32 root = u32(hdr.begin);
        return node(root, 0);
    }

    bool node(quint32 id, int depth) {
        if (depth > kMaxTreeDepth)
            return fail("tree too deep");
        if (visited_.contains(id))
            return fail("node cycle");
        visited_.insert(id);
        Block b;
        if (!block(id, b) || b.end - b.begin < 8)
            return fail("bad node block");
        quint64 p = b.begin;
        const quint32 next = u32(p);
        const quint32 count = u32(p + 4);
        p += 8;
        for (quint32 i = 0; i < count; ++i) {
            if (next != 0) {
                if (b.end - p < 4)
                    return fail("node truncated");
                const quint32 child = u32(p);
                p += 4;
                if (!node(child, depth + 1))
                    return false;
            }
            bool stopNode = false;
            if (!record(p, b.end, stopNode))
                return false;
            if (stopNode)
                break; // rest of this node is unreadable; keep what we have, continue elsewhere
        }
        if (next != 0)
            return node(next, depth + 1);
        return true;
    }

    bool record(quint64& p, quint64 end, bool& stopNode) {
        if (records_.size() >= kMaxRecords)
            return fail("too many records");
        if (end - p < 4)
            return fail("record truncated");
        const quint64 nameLen = u32(p);
        p += 4;
        if (nameLen > (end - p) / 2)
            return fail("record name truncated");
        QString name(qsizetype(nameLen), Qt::Uninitialized);
        for (quint64 i = 0; i < nameLen; ++i)
            name[qsizetype(i)] = QChar(char16_t((d_[p + 2 * i] << 8) | d_[p + 2 * i + 1]));
        p += 2 * nameLen;
        if (end - p < 8)
            return fail("record header truncated");
        // Finder stores names as on disk (usually decomposed, NFD); Qt hands out NFC.
        // Normalize so record names compare equal to Qt file names.
        if (std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() >= 0x80; }))
            name = name.normalized(QString::NormalizationForm_C);
        DsStoreRecord r;
        r.fileName = std::move(name);
        r.code = QString::fromLatin1(reinterpret_cast<const char*>(d_ + p), 4);
        const QByteArray type(reinterpret_cast<const char*>(d_ + p + 4), 4);
        r.type = QString::fromLatin1(type);
        p += 8;
        auto need = [&](quint64 len) { return end - p >= len; };
        if (type == "bool") {
            if (!need(1))
                return fail("value truncated");
            r.value = d_[p] != 0;
            p += 1;
        } else if (type == "long" || type == "shor") {
            if (!need(4))
                return fail("value truncated");
            r.value = qint64(qint32(u32(p)));
            p += 4;
        } else if (type == "comp" || type == "dutc") {
            if (!need(8))
                return fail("value truncated");
            r.value = qint64((quint64(u32(p)) << 32) | u32(p + 4));
            p += 8;
        } else if (type == "type") {
            if (!need(4))
                return fail("value truncated");
            r.value = QString::fromLatin1(reinterpret_cast<const char*>(d_ + p), 4);
            p += 4;
        } else if (type == "blob") {
            if (!need(4))
                return fail("value truncated");
            const quint64 len = u32(p);
            p += 4;
            if (!need(len))
                return fail("blob truncated");
            r.value = QByteArray(reinterpret_cast<const char*>(d_ + p), qsizetype(len));
            p += len;
        } else if (type == "ustr") {
            if (!need(4))
                return fail("value truncated");
            const quint64 len = u32(p);
            p += 4;
            if (len > (end - p) / 2)
                return fail("ustr truncated");
            QString s(qsizetype(len), Qt::Uninitialized);
            for (quint64 i = 0; i < len; ++i)
                s[qsizetype(i)] = QChar(char16_t((d_[p + 2 * i] << 8) | d_[p + 2 * i + 1]));
            r.value = s;
            p += 2 * len;
        } else {
            // Unknown (future) type: its length is unknown, so the rest of this node cannot be
            // parsed. Records of other nodes stay usable.
            stopNode = true;
            return true;
        }
        records_.append(std::move(r));
        return true;
    }
};

} // namespace

std::optional<QVector<DsStoreRecord>> parse(const QByteArray& data, QString* error) {
    Reader r(data);
    return r.run(error);
}

std::optional<QVector<DsStoreRecord>> readFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    if (f.size() > kMaxFileSize) {
        if (error)
            *error = QStringLiteral("file too large");
        return std::nullopt;
    }
    const QByteArray data = f.read(kMaxFileSize + 1);
    if (data.size() > kMaxFileSize) {
        if (error)
            *error = QStringLiteral("file too large");
        return std::nullopt;
    }
    return parse(data, error);
}

} // namespace lf::dsstore
