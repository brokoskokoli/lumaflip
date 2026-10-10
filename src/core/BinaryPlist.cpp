// SPDX-License-Identifier: GPL-3.0-or-later
#include "BinaryPlist.h"

#include <QDateTime>
#include <QTimeZone>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <cstring>

namespace lf::bplist {
namespace {

constexpr int kMaxDepth = 128;
constexpr quint64 kMaxObjects = 1u << 24; // sanity cap, far above any Finder blob
constexpr quint64 kMaxVisits = 1u << 20;  // logical nodes (shared sub-objects count once per reference)
// Logical output budget: a shared data/string object referenced N times counts N times. Parsed
// objects are memoized per index (QVariant implicit sharing), so the real memory stays close to
// the input size; the budget keeps consumers that walk the tree from expanding it unboundedly.
constexpr quint64 kMaxOutputBytes = 16u << 20;
constexpr quint64 kNodeOverhead = 16; // bytes charged per node on top of its payload

class Parser {
public:
    explicit Parser(const QByteArray& data)
        : d_(reinterpret_cast<const uchar*>(data.constData())), n_(data.size()) {}

    std::optional<QVariant> run(QString* error) {
        auto fail = [&](const char* msg) -> std::optional<QVariant> {
            if (error)
                *error = QString::fromLatin1(msg);
            return std::nullopt;
        };
        if (n_ < 8 + 32)
            return fail("too short");
        if (std::memcmp(d_, "bplist0", 7) != 0)
            return fail("bad magic");
        const quint64 t = n_ - 32;
        offsetIntSize_ = d_[t + 6];
        refSize_ = d_[t + 7];
        numObjects_ = be(t + 8, 8);
        const quint64 top = be(t + 16, 8);
        offsetTable_ = be(t + 24, 8);
        if (offsetIntSize_ < 1 || offsetIntSize_ > 8 || refSize_ < 1 || refSize_ > 8)
            return fail("bad trailer sizes");
        if (numObjects_ == 0 || numObjects_ > kMaxObjects || top >= numObjects_)
            return fail("bad object count");
        // offset table must lie between header and trailer
        if (offsetTable_ < 8 || offsetTable_ > t || (t - offsetTable_) / offsetIntSize_ < numObjects_)
            return fail("bad offset table");
        state_.assign(numObjects_, 0);
        memo_.assign(numObjects_, Memo{});
        bool ok = true;
        QVariant v = object(top, 0, ok);
        if (!ok)
            return fail(err_ ? err_ : "malformed object");
        return v;
    }

private:
    const uchar* d_;
    quint64 n_;
    int offsetIntSize_ = 0;
    int refSize_ = 0;
    quint64 numObjects_ = 0;
    quint64 offsetTable_ = 0;
    std::vector<char> state_; // 0 = unvisited/done, 1 = on the current path (cycle detection)
    const char* err_ = nullptr;
    quint64 visits_ = 0;
    quint64 outBytes_ = 0;

    // Fully parsed objects, so repeated references share one QVariant instead of copying it.
    struct Memo {
        bool done = false;
        QVariant value;
        quint64 nodes = 0; // logical node count of the subtree
        quint64 bytes = 0; // logical output bytes of the subtree
    };
    std::vector<Memo> memo_;

    // Big-endian unsigned read; caller guarantees bounds.
    quint64 be(quint64 pos, int size) const {
        quint64 v = 0;
        for (int i = 0; i < size; ++i)
            v = (v << 8) | d_[pos + quint64(i)];
        return v;
    }
    bool has(quint64 pos, quint64 len) const { return pos <= n_ && len <= n_ - pos; }

    bool objectOffset(quint64 idx, quint64& off) {
        if (idx >= numObjects_)
            return false;
        const quint64 pos = offsetTable_ + idx * quint64(offsetIntSize_);
        if (!has(pos, quint64(offsetIntSize_)))
            return false;
        off = be(pos, offsetIntSize_);
        return off >= 8 && off < offsetTable_;
    }

    // Reads the length of a container/string whose marker low nibble is `nib` at `pos`.
    // Advances `pos` past an extended length int if present.
    bool length(quint64& pos, int nib, quint64& len) {
        if (nib != 0xF) {
            len = quint64(nib);
            return true;
        }
        if (!has(pos, 1))
            return false;
        const uchar m = d_[pos];
        if ((m & 0xF0) != 0x10)
            return false;
        const int sz = 1 << (m & 0x0F);
        if (sz > 8 || !has(pos + 1, quint64(sz)))
            return false;
        len = be(pos + 1, sz);
        pos += 1 + quint64(sz);
        return true;
    }

    QVariant bad(bool& ok, const char* msg) {
        ok = false;
        if (!err_)
            err_ = msg;
        return {};
    }

    bool charge(bool& ok, quint64 nodes, quint64 bytes) {
        visits_ += nodes;
        outBytes_ += bytes;
        if (visits_ > kMaxVisits) {
            bad(ok, "too many objects");
            return false;
        }
        if (outBytes_ > kMaxOutputBytes) {
            bad(ok, "plist expands too much");
            return false;
        }
        return true;
    }

    QVariant object(quint64 idx, int depth, bool& ok) {
        if (depth > kMaxDepth)
            return bad(ok, "nesting too deep");
        if (idx < memo_.size() && memo_[idx].done) {
            const Memo& m = memo_[idx];
            if (!charge(ok, m.nodes, m.bytes))
                return {};
            return m.value;
        }
        const quint64 nodesBefore = visits_;
        const quint64 bytesBefore = outBytes_;
        if (!charge(ok, 1, kNodeOverhead))
            return {};
        QVariant v = parseObject(idx, depth, ok);
        if (!ok)
            return {};
        Memo& m = memo_[idx];
        m.done = true;
        m.value = v;
        m.nodes = visits_ - nodesBefore;
        m.bytes = outBytes_ - bytesBefore;
        return v;
    }

    QVariant parseObject(quint64 idx, int depth, bool& ok) {
        quint64 off = 0;
        if (!objectOffset(idx, off))
            return bad(ok, "bad object offset");
        if (state_[idx] == 1)
            return bad(ok, "reference cycle");
        const uchar marker = d_[off];
        const int type = marker >> 4;
        const int nib = marker & 0x0F;
        quint64 pos = off + 1;
        switch (type) {
        case 0x0:
            if (marker == 0x08)
                return false;
            if (marker == 0x09)
                return true;
            if (marker == 0x00 || marker == 0x0F)
                return {};
            return bad(ok, "unknown simple marker");
        case 0x1: {
            const int sz = 1 << nib;
            if (sz > 16 || !has(pos, quint64(sz)))
                return bad(ok, "truncated int");
            if (sz == 16) // 128-bit: take the low 64 bits
                return qint64(be(pos + 8, 8));
            return qint64(be(pos, sz)); // 1/2/4-byte ints are unsigned, 8-byte signed
        }
        case 0x2: {
            const int sz = 1 << nib;
            if ((sz != 4 && sz != 8) || !has(pos, quint64(sz)))
                return bad(ok, "bad real");
            return realAt(pos, sz);
        }
        case 0x3: {
            if (marker != 0x33 || !has(pos, 8))
                return bad(ok, "bad date");
            const double secs = realAt(pos, 8);
            if (!std::isfinite(secs) || std::fabs(secs) > 1e14)
                return bad(ok, "date out of range");
            const QDateTime ref(QDate(2001, 1, 1), QTime(0, 0), QTimeZone::UTC);
            return ref.addMSecs(qint64(std::llround(secs * 1000.0)));
        }
        case 0x4: {
            quint64 len = 0;
            if (!length(pos, nib, len) || !has(pos, len))
                return bad(ok, "truncated data");
            if (!charge(ok, 0, len))
                return {};
            return QByteArray(reinterpret_cast<const char*>(d_ + pos), qsizetype(len));
        }
        case 0x5: {
            quint64 len = 0;
            if (!length(pos, nib, len) || !has(pos, len))
                return bad(ok, "truncated ascii string");
            if (!charge(ok, 0, len * 2))
                return {};
            return QString::fromLatin1(reinterpret_cast<const char*>(d_ + pos), qsizetype(len));
        }
        case 0x6: {
            quint64 len = 0;
            if (!length(pos, nib, len) || len > n_ / 2 || !has(pos, len * 2))
                return bad(ok, "truncated utf16 string");
            if (!charge(ok, 0, len * 2))
                return {};
            QString s(qsizetype(len), Qt::Uninitialized);
            for (quint64 i = 0; i < len; ++i)
                s[qsizetype(i)] = QChar(char16_t(be(pos + 2 * i, 2)));
            return s;
        }
        case 0x7: {
            quint64 len = 0;
            if (!length(pos, nib, len) || !has(pos, len))
                return bad(ok, "truncated utf8 string");
            if (!charge(ok, 0, len * 2))
                return {};
            return QString::fromUtf8(reinterpret_cast<const char*>(d_ + pos), qsizetype(len));
        }
        case 0x8: {
            const int sz = nib + 1;
            if (sz > 8 || !has(pos, quint64(sz)))
                return bad(ok, "bad uid");
            return QVariant::fromValue(PlistUid{be(pos, sz)});
        }
        case 0xA:
        case 0xB:
        case 0xC: {
            quint64 cnt = 0;
            if (!length(pos, nib, cnt) || cnt > n_ / quint64(refSize_) || !has(pos, cnt * quint64(refSize_)))
                return bad(ok, "truncated array");
            state_[idx] = 1;
            QVariantList list;
            list.reserve(qsizetype(cnt));
            for (quint64 i = 0; i < cnt && ok; ++i)
                list.append(object(be(pos + i * quint64(refSize_), refSize_), depth + 1, ok));
            state_[idx] = 0;
            return ok ? QVariant(list) : QVariant();
        }
        case 0xD: {
            quint64 cnt = 0;
            if (!length(pos, nib, cnt) || cnt > n_ / (2 * quint64(refSize_)) ||
                !has(pos, 2 * cnt * quint64(refSize_)))
                return bad(ok, "truncated dict");
            state_[idx] = 1;
            QVariantMap map;
            for (quint64 i = 0; i < cnt && ok; ++i) {
                const QVariant k = object(be(pos + i * quint64(refSize_), refSize_), depth + 1, ok);
                if (!ok)
                    break;
                const QVariant v = object(be(pos + (cnt + i) * quint64(refSize_), refSize_), depth + 1, ok);
                if (ok)
                    map.insert(k.toString(), v);
            }
            state_[idx] = 0;
            return ok ? QVariant(map) : QVariant();
        }
        default:
            return bad(ok, "unknown object type");
        }
    }

    double realAt(quint64 pos, int sz) const {
        if (sz == 4) {
            const quint32 bits = quint32(be(pos, 4));
            float f;
            std::memcpy(&f, &bits, 4);
            return double(f);
        }
        const quint64 bits = be(pos, 8);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
};

} // namespace

bool looksLikeBinaryPlist(const QByteArray& data) {
    return data.startsWith("bplist");
}

std::optional<QVariant> parse(const QByteArray& data, QString* error) {
    Parser p(data);
    return p.run(error);
}

} // namespace lf::bplist
