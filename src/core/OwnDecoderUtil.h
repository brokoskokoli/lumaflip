// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small helpers shared by the own layered-format decoders (XcfDecoder, PsbDecoder): a
// bounds-checked big-endian reader, reading a whole file (memory mapped), and the target
// size of a request (maxPixelSize and the byte budget, P-08).

#include "Decoder.h"

#include <QByteArray>
#include <QFile>
#include <QSize>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lf::decutil {

// Big-endian reader over a byte buffer. Every read is bounds-checked: past the end it
// returns 0 and clears ok() for good.
class Reader {
public:
    explicit Reader(const QByteArray& data)
        : m_p(reinterpret_cast<const uchar*>(data.constData())), m_n(data.size()) {}
    Reader(const uchar* p, qint64 n) : m_p(p), m_n(n) {}

    bool ok() const { return m_ok; }
    void fail() { m_ok = false; }
    qint64 pos() const { return m_pos; }
    qint64 size() const { return m_n; }
    qint64 remaining() const { return m_ok ? m_n - m_pos : 0; }
    const uchar* data() const { return m_p; }
    const uchar* here() const { return m_p + m_pos; }
    bool has(qint64 k) const { return m_ok && k >= 0 && m_pos <= m_n - k; }
    bool seek(qint64 off) {
        if (!m_ok || off < 0 || off > m_n)
            return m_ok = false;
        m_pos = off;
        return true;
    }
    bool skip(qint64 k) { return has(k) ? (m_pos += k, true) : (m_ok = false); }
    quint8 u8() {
        if (!has(1))
            return m_ok = false, 0;
        return m_p[m_pos++];
    }
    quint16 u16() {
        if (!has(2))
            return m_ok = false, 0;
        const quint16 v = quint16((m_p[m_pos] << 8) | m_p[m_pos + 1]);
        m_pos += 2;
        return v;
    }
    quint32 u32() {
        if (!has(4))
            return m_ok = false, 0;
        const quint32 v = (quint32(m_p[m_pos]) << 24) | (quint32(m_p[m_pos + 1]) << 16) |
                          (quint32(m_p[m_pos + 2]) << 8) | quint32(m_p[m_pos + 3]);
        m_pos += 4;
        return v;
    }
    quint64 u64() {
        const quint64 hi = u32();
        const quint64 lo = u32();
        return (hi << 32) | lo;
    }
    qint32 i32() { return qint32(u32()); }
    float f32() {
        const quint32 v = u32();
        float f;
        std::memcpy(&f, &v, sizeof f);
        return f;
    }
    QByteArray bytes(qint64 k) {
        if (!has(k))
            return m_ok = false, QByteArray();
        QByteArray out(reinterpret_cast<const char*>(m_p + m_pos), k);
        m_pos += k;
        return out;
    }

private:
    const uchar* m_p = nullptr;
    qint64 m_n = 0;
    qint64 m_pos = 0;
    bool m_ok = true;
};

// A whole file for the in-memory parsers (memory mapped where possible). `data` stays
// valid while the object lives.
struct FileBytes {
    QFile file;
    QByteArray data;
};

inline bool readFile(const QString& path, FileBytes& fb, qint64 maxBytes, QString* error,
                     const QString& unreadable, const QString& empty, const QString& tooLarge) {
    fb.file.setFileName(path);
    if (!fb.file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = unreadable;
        return false;
    }
    const qint64 size = fb.file.size();
    if (size <= 0 || size > maxBytes) {
        if (error)
            *error = size <= 0 ? empty : tooLarge;
        return false;
    }
    if (uchar* m = fb.file.map(0, size)) {
        fb.data = QByteArray::fromRawData(reinterpret_cast<const char*>(m), size);
        return true;
    }
    fb.data = fb.file.readAll();
    if (fb.data.size() != size) {
        if (error)
            *error = unreadable;
        return false;
    }
    return true;
}

inline QSize fitLongEdge(QSize s, int longEdge) {
    const double scale = double(longEdge) / std::max(s.width(), s.height());
    return QSize(std::max(1, int(std::lround(s.width() * scale))),
                 std::max(1, int(std::lround(s.height() * scale))));
}

// Longer edge the result may have (0 = full size): maxPixelSize and the byte budget (P-08).
inline int targetEdge(QSize full, int bytesPerPixel, const DecodeRequest& req) {
    int edge = req.maxPixelSize;
    if (req.maxBytes > 0 && !full.isEmpty()) {
        const double pixels = double(full.width()) * full.height();
        const double maxPixels = double(std::max<qint64>(1, req.maxBytes / bytesPerPixel));
        if (pixels > maxPixels) {
            const int longE = std::max(full.width(), full.height());
            const int shortE = std::min(full.width(), full.height());
            int e = std::max(1, int(std::floor(longE * std::sqrt(maxPixels / pixels))));
            while (e > 1 && double(e) * std::ceil(double(e) * shortE / longE) > maxPixels)
                --e;
            edge = edge > 0 ? std::min(edge, e) : e;
        }
    }
    if (edge > 0 && std::max(full.width(), full.height()) <= edge)
        return 0;
    return edge;
}

// Integer subsampling factor so that the longer edge of `full` / factor stays >= `edge`
// (0 = full size), and the pixel count stays within `maxPixels`.
inline int subsampleFactor(QSize full, int edge, qint64 maxPixels) {
    const int longE = std::max(full.width(), full.height());
    int k = edge > 0 ? std::max(1, longE / edge) : 1;
    while ((qint64(full.width() + k - 1) / k) * (qint64(full.height() + k - 1) / k) > maxPixels)
        ++k;
    return k;
}

// sRGB transfer function (linear -> encoded) and its inverse.
inline float linearToSrgb(float v) {
    if (!(v > 0.0f))
        return 0.0f;
    if (v >= 1.0f)
        return 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

} // namespace lf::decutil
