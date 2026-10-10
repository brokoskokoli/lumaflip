// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Synthetic EXIF/TIFF structures for tests (F-EDIT-03 metadata, F-INFO-07 AF points):
// a small TIFF writer (IFDs with out-of-line data, both byte orders) and JPEG helpers that
// insert APP1/APP2 segments. No real camera files (privacy, size).

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QVector>

namespace lf::test {

class TiffWriter {
public:
    struct Entry {
        quint16 tag = 0;
        quint16 type = 0; // 2 ASCII, 3 SHORT, 4 LONG, 7 UNDEFINED, 8 SSHORT
        quint32 count = 0;
        QByteArray data; // encoded in the writer's byte order
    };

    explicit TiffWriter(bool little = true) : m_little(little) {
        m_b.append(little ? "II" : "MM", 2);
        put16(m_b, 42);
        put32(m_b, 8);
    }

    bool little() const { return m_little; }
    QByteArray bytes() const { return m_b; }
    qint64 size() const { return m_b.size(); }

    QByteArray u16(int v) const {
        QByteArray a;
        put16(a, quint16(v));
        return a;
    }
    QByteArray u32(quint32 v) const {
        QByteArray a;
        put32(a, v);
        return a;
    }
    QByteArray shorts(const QVector<int>& v) const {
        QByteArray a;
        for (int x : v)
            put16(a, quint16(x));
        return a;
    }

    Entry shortEntry(quint16 tag, const QVector<int>& v) const { return {tag, 3, quint32(v.size()), shorts(v)}; }
    Entry longEntry(quint16 tag, quint32 v) const { return {tag, 4, 1, u32(v)}; }
    Entry asciiEntry(quint16 tag, const QByteArray& s) const {
        QByteArray d = s;
        d.append('\0');
        return {tag, 2, quint32(d.size()), d};
    }
    Entry undefinedEntry(quint16 tag, const QByteArray& d) const { return {tag, 7, quint32(d.size()), d}; }

    // Writes an IFD (entries sorted by the caller) at the end, its out-of-line data right
    // behind it. Returns its offset; `valuePos` gets the offset of each entry's value field.
    qint64 writeIfd(const QVector<Entry>& entries, QVector<qint64>* valuePos = nullptr, quint32 next = 0) {
        align();
        const qint64 off = m_b.size();
        put16(m_b, quint16(entries.size()));
        const qint64 dataStart = off + 2 + qint64(entries.size()) * 12 + 4;
        QByteArray data;
        for (const Entry& e : entries) {
            put16(m_b, e.tag);
            put16(m_b, e.type);
            put32(m_b, e.count);
            if (valuePos)
                valuePos->append(m_b.size());
            if (e.data.size() <= 4) {
                QByteArray v = e.data;
                v.append(QByteArray(4 - v.size(), '\0'));
                m_b.append(v);
            } else {
                put32(m_b, quint32(dataStart + data.size()));
                data.append(e.data);
                if (data.size() % 2)
                    data.append('\0');
            }
        }
        put32(m_b, next);
        m_b.append(data);
        return off;
    }

    qint64 append(const QByteArray& blob) {
        align();
        const qint64 off = m_b.size();
        m_b.append(blob);
        return off;
    }

    void patch32(qint64 pos, quint32 v) {
        QByteArray a;
        put32(a, v);
        m_b.replace(pos, 4, a);
    }

private:
    void align() {
        if (m_b.size() % 2)
            m_b.append('\0');
    }
    void put16(QByteArray& a, quint16 v) const {
        if (m_little)
            a.append(char(v & 0xFF)).append(char(v >> 8));
        else
            a.append(char(v >> 8)).append(char(v & 0xFF));
    }
    void put32(QByteArray& a, quint32 v) const {
        for (int k = 0; k < 4; ++k)
            a.append(char((v >> (m_little ? 8 * k : 8 * (3 - k))) & 0xFF));
    }

    bool m_little;
    QByteArray m_b;
};

// A JPEG segment: marker + length + payload.
inline QByteArray jpegSegment(uchar marker, const QByteArray& payload) {
    QByteArray s;
    s.append(char(0xFF)).append(char(marker));
    const int len = int(payload.size()) + 2;
    s.append(char(len >> 8)).append(char(len & 0xFF));
    s.append(payload);
    return s;
}

// Inserts segments right after SOI.
inline QByteArray withSegments(const QByteArray& jpeg, const QByteArray& segments) {
    return jpeg.left(2) + segments + jpeg.mid(2);
}

inline QByteArray exifApp1(const QByteArray& tiff) {
    return jpegSegment(0xE1, QByteArray("Exif\0\0", 6) + tiff);
}

// Noise with a few shapes: every crop position is distinguishable.
inline QImage noisyPattern(QSize size, quint32 seed = 7) {
    QImage img(size, QImage::Format_RGB32);
    QRandomGenerator rng(seed);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            img.setPixel(x, y, qRgb((x * 3 + int(rng.bounded(40))) & 0xFF, (y * 5 + int(rng.bounded(40))) & 0xFF,
                                    int(rng.bounded(256))));
    QPainter p(&img);
    p.fillRect(QRect(0, 0, size.width() / 3, size.height() / 4), Qt::red);
    p.fillRect(QRect(size.width() / 2, size.height() / 2, size.width() / 5, size.height() / 6), Qt::blue);
    return img;
}

inline QByteArray encodeJpeg(const QImage& img, int quality = 90) {
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", quality);
    return data;
}

} // namespace lf::test
