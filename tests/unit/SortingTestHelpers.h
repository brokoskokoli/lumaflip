// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test helpers for the collection/sorting tests: a binary plist writer, a .DS_Store
// writer (synthetic fixtures only - never real user files), fake providers and a small
// event-loop wait helper.

#include "core/BinaryPlist.h"
#include "core/DsStore.h"
#include "core/Platform.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QTimeZone>
#include <QVariant>
#include <QVector>
#include <atomic>
#include <cstring>
#include <functional>

namespace lf::test {

inline void ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "lf_unit_tests";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv); // intentionally leaked, lives for the process
    }
}

inline bool waitUntil(const std::function<bool()>& pred, int timeoutMs = 5000) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

inline void putBE(QByteArray& out, quint64 v, int size) {
    for (int i = size - 1; i >= 0; --i)
        out.append(char((v >> (8 * i)) & 0xff));
}

// ---- binary plist writer ----
class BplistWriter {
public:
    static QByteArray write(const QVariant& root) {
        BplistWriter w;
        const int top = w.add(root);
        QByteArray out("bplist00");
        QVector<quint64> offsets;
        for (const QByteArray& o : std::as_const(w.objs_)) {
            offsets.append(quint64(out.size()));
            out.append(o);
        }
        const quint64 tableOffset = quint64(out.size());
        for (quint64 off : offsets)
            putBE(out, off, 4);
        out.append(QByteArray(6, '\0'));
        out.append(char(4)); // offset int size
        out.append(char(2)); // object ref size
        putBE(out, quint64(w.objs_.size()), 8);
        putBE(out, quint64(top), 8);
        putBE(out, tableOffset, 8);
        return out;
    }

private:
    QVector<QByteArray> objs_;

    static void marker(QByteArray& o, int type, qsizetype len) {
        if (len < 15) {
            o.append(char((type << 4) | int(len)));
        } else {
            o.append(char((type << 4) | 0xF));
            o.append(char(0x12));
            putBE(o, quint64(len), 4);
        }
    }

    int add(const QVariant& v) {
        const int idx = int(objs_.size());
        objs_.append(QByteArray());
        QByteArray o;
        switch (v.typeId()) {
        case QMetaType::Bool:
            o.append(char(v.toBool() ? 0x09 : 0x08));
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            o.append(char(0x13));
            putBE(o, quint64(v.toLongLong()), 8);
            break;
        case QMetaType::Double: {
            o.append(char(0x23));
            const double d = v.toDouble();
            quint64 bits;
            std::memcpy(&bits, &d, 8);
            putBE(o, bits, 8);
            break;
        }
        case QMetaType::QDateTime: {
            o.append(char(0x33));
            const QDateTime ref(QDate(2001, 1, 1), QTime(0, 0), QTimeZone::UTC);
            const double d = double(ref.msecsTo(v.toDateTime())) / 1000.0;
            quint64 bits;
            std::memcpy(&bits, &d, 8);
            putBE(o, bits, 8);
            break;
        }
        case QMetaType::QByteArray: {
            const QByteArray b = v.toByteArray();
            marker(o, 0x4, b.size());
            o.append(b);
            break;
        }
        case QMetaType::QString: {
            const QString s = v.toString();
            bool ascii = true;
            for (QChar c : s)
                ascii = ascii && c.unicode() < 128;
            if (ascii) {
                marker(o, 0x5, s.size());
                o.append(s.toLatin1());
            } else {
                marker(o, 0x6, s.size());
                for (QChar c : s)
                    putBE(o, c.unicode(), 2);
            }
            break;
        }
        case QMetaType::QVariantList: {
            const QVariantList l = v.toList();
            QVector<int> refs;
            for (const QVariant& e : l)
                refs.append(add(e));
            marker(o, 0xA, l.size());
            for (int r : refs)
                putBE(o, quint64(r), 2);
            break;
        }
        case QMetaType::QVariantMap: {
            const QVariantMap m = v.toMap();
            QVector<int> keys, vals;
            for (auto it = m.cbegin(); it != m.cend(); ++it) {
                keys.append(add(it.key()));
                vals.append(add(it.value()));
            }
            marker(o, 0xD, m.size());
            for (int r : keys)
                putBE(o, quint64(r), 2);
            for (int r : vals)
                putBE(o, quint64(r), 2);
            break;
        }
        default:
            if (v.canConvert<PlistUid>() && v.userType() == qMetaTypeId<PlistUid>()) {
                o.append(char(0x83));
                putBE(o, v.value<PlistUid>().value, 4);
            } else {
                o.append(char(0x00)); // null
            }
            break;
        }
        objs_[idx] = o;
        return idx;
    }
};

// ---- .DS_Store writer ----
// `recordsPerLeaf` > 0 builds a two-level B-tree (internal root + leaves).
inline QByteArray writeDsStore(const QVector<DsStoreRecord>& records, int recordsPerLeaf = 0) {
    auto encodeRecord = [](const DsStoreRecord& r) {
        QByteArray o;
        putBE(o, quint64(r.fileName.size()), 4);
        for (QChar c : r.fileName)
            putBE(o, c.unicode(), 2);
        o.append(r.code.toLatin1().leftJustified(4, ' ', true));
        o.append(r.type.toLatin1().leftJustified(4, ' ', true));
        const QByteArray t = r.type.toLatin1();
        if (t == "bool") {
            o.append(char(r.value.toBool() ? 1 : 0));
        } else if (t == "long" || t == "shor") {
            putBE(o, quint64(quint32(qint32(r.value.toLongLong()))), 4);
        } else if (t == "comp" || t == "dutc") {
            putBE(o, quint64(r.value.toLongLong()), 8);
        } else if (t == "type") {
            o.append(r.value.toString().toLatin1().leftJustified(4, ' ', true));
        } else if (t == "blob") {
            const QByteArray b = r.value.toByteArray();
            putBE(o, quint64(b.size()), 4);
            o.append(b);
        } else if (t == "ustr") {
            const QString s = r.value.toString();
            putBE(o, quint64(s.size()), 4);
            for (QChar c : s)
                putBE(o, c.unicode(), 2);
        } else {
            o.append(r.value.toByteArray()); // raw bytes for unknown types (robustness tests)
        }
        return o;
    };

    // Node blocks: index 0 in `nodes` is the B-tree root.
    QVector<QByteArray> nodes;
    if (recordsPerLeaf <= 0 || records.size() <= recordsPerLeaf) {
        QByteArray leaf;
        putBE(leaf, 0, 4);
        putBE(leaf, quint64(records.size()), 4);
        for (const auto& r : records)
            leaf.append(encodeRecord(r));
        nodes.append(leaf);
    } else {
        // Split: leaf, separator, leaf, separator, ..., leaf
        QVector<QVector<DsStoreRecord>> leaves;
        QVector<DsStoreRecord> seps;
        qsizetype i = 0;
        while (i < records.size()) {
            leaves.append(records.mid(i, recordsPerLeaf));
            i += recordsPerLeaf;
            if (i < records.size())
                seps.append(records[i++]);
        }
        if (seps.size() == leaves.size()) // trailing separator without leaf: make it a leaf
            leaves.append({seps.takeLast()});
        nodes.append(QByteArray()); // root placeholder
        for (const auto& l : leaves) {
            QByteArray leaf;
            putBE(leaf, 0, 4);
            putBE(leaf, quint64(l.size()), 4);
            for (const auto& r : l)
                leaf.append(encodeRecord(r));
            nodes.append(leaf);
        }
        // block ids: 0 = allocator root, 1 = DSDB, 2 = tree root, 3.. = leaves
        QByteArray root;
        putBE(root, quint64(2 + leaves.size()), 4); // rightmost child = last leaf
        putBE(root, quint64(seps.size()), 4);
        for (qsizetype k = 0; k < seps.size(); ++k) {
            putBE(root, quint64(3 + k), 4);
            root.append(encodeRecord(seps[k]));
        }
        nodes[0] = root;
    }

    const int blockCount = 2 + int(nodes.size());
    QByteArray dsdb;
    putBE(dsdb, 2, 4); // root node
    putBE(dsdb, nodes.size() > 1 ? 1 : 0, 4);
    putBE(dsdb, quint64(records.size()), 4);
    putBE(dsdb, quint64(nodes.size()), 4);
    putBE(dsdb, 0x1000, 4);

    auto pow2 = [](qsizetype n) {
        int log = 5;
        while ((qsizetype(1) << log) < n)
            ++log;
        return log;
    };
    // Compute block sizes; the allocator root block has a fixed layout size.
    const qsizetype rootBlockSize = 8 + 256 * 4 * ((blockCount + 255) / 256) + 4 + 1 + 4 + 4 + 32 * 4;
    QVector<int> logs = {pow2(rootBlockSize), pow2(dsdb.size())};
    for (const auto& n : nodes)
        logs.append(pow2(n.size()));
    // Place blocks after the 32-byte header, each aligned to its own size (buddy style).
    QVector<quint32> offsets;
    quint64 pos = 32;
    for (int lg : logs) {
        const quint64 sz = quint64(1) << lg;
        pos = (pos + sz - 1) & ~(sz - 1);
        offsets.append(quint32(pos));
        pos += sz;
    }
    QByteArray rootBlock;
    putBE(rootBlock, quint64(blockCount), 4);
    putBE(rootBlock, 0, 4);
    for (int b = 0; b < blockCount; ++b)
        putBE(rootBlock, offsets[b] | quint32(logs[b]), 4);
    for (int b = blockCount; b < 256 * ((blockCount + 255) / 256); ++b)
        putBE(rootBlock, 0, 4);
    putBE(rootBlock, 1, 4);
    rootBlock.append(char(4));
    rootBlock.append("DSDB");
    putBE(rootBlock, 1, 4);
    for (int f = 0; f < 32; ++f)
        putBE(rootBlock, 0, 4);

    QByteArray file;
    putBE(file, 1, 4);
    file.append("Bud1");
    putBE(file, offsets[0], 4);
    putBE(file, quint64(1) << logs[0], 4);
    putBE(file, offsets[0], 4);
    file.append(QByteArray(16, '\0'));
    file.resize(qsizetype(4 + pos), '\0');
    QVector<QByteArray> contents = {rootBlock, dsdb};
    contents.append(nodes);
    for (int b = 0; b < blockCount; ++b)
        file.replace(qsizetype(4 + offsets[b]), contents[b].size(), contents[b]);
    return file;
}

inline DsStoreRecord blobRecord(const QString& name, const char* code, const QVariant& plist) {
    return {name, QString::fromLatin1(code), QStringLiteral("blob"), BplistWriter::write(plist)};
}

inline DsStoreRecord ilocRecord(const QString& name, qint32 x, qint32 y) {
    QByteArray b;
    putBE(b, quint64(quint32(x)), 4);
    putBE(b, quint64(quint32(y)), 4);
    b.append(QByteArray::fromHex("ffffffffffff0000"));
    return {name, QStringLiteral("Iloc"), QStringLiteral("blob"), b};
}

inline bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size();
}

// ---- fake providers ----
class FakeOrderProvider : public FolderOrderProvider {
public:
    SortSpec spec{SortKey::Name, true, SortSource::FinderDsStore};
    QHash<QString, QPointF> positions;
    std::atomic<int> delayMs{0};
    std::atomic<int> calls{0};
    std::atomic<bool> lastAllowLive{false};
    SortSpec finderOrder(const QString&, bool allowLiveQuery) override {
        ++calls;
        lastAllowLive = allowLiveQuery;
        if (delayMs > 0)
            QThread::msleep(ulong(delayMs.load()));
        QMutexLocker l(&mutex);
        return spec;
    }
    QHash<QString, QPointF> iconPositions(const QString&) override {
        QMutexLocker l(&mutex);
        return positions;
    }
    void setSpec(const SortSpec& s) {
        QMutexLocker l(&mutex);
        spec = s;
    }
    QMutex mutex;
};

class FakeAttributeProvider : public FileAttributeProvider {
public:
    QHash<QString, QDateTime> added; // by file name
    QHash<QString, QString> kinds;   // by file name
    void fill(FileEntry& e) override {
        if (added.contains(e.name))
            e.added = added.value(e.name);
        if (kinds.contains(e.name))
            e.kind = kinds.value(e.name);
    }
};

} // namespace lf::test
