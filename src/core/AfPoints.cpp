// SPDX-License-Identifier: GPL-3.0-or-later
// AF points from maker notes and EXIF (F-INFO-07), see AfPoints.h. Layouts follow the
// public descriptions of the maker notes (ExifTool tag documentation).

#include "AfPoints.h"

#include "CropSave.h"

#include <QFile>

#include <algorithm>
#include <optional>

namespace lf::afpoints {

namespace {

// Bounds-checked TIFF access: offsets relative to `base`.
struct Ctx {
    const QByteArray* d = nullptr;
    qint64 base = 0;
    bool little = true;

    bool has(qint64 off, qint64 len) const { return off >= 0 && len >= 0 && base + off + len <= d->size(); }
    quint16 u16(qint64 off) const {
        if (!has(off, 2))
            return 0;
        const auto* p = reinterpret_cast<const uchar*>(d->constData() + base + off);
        return little ? quint16(p[0] | (p[1] << 8)) : quint16((p[0] << 8) | p[1]);
    }
    quint32 u32(qint64 off) const {
        if (!has(off, 4))
            return 0;
        const auto* p = reinterpret_cast<const uchar*>(d->constData() + base + off);
        return little ? quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24)
                      : (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
    }
};

struct Entry {
    quint16 tag = 0, type = 0;
    quint32 count = 0;
    qint64 pos = 0; // offset of the entry (relative to base)
};

int typeSize(quint16 type) {
    switch (type) {
    case 1:
    case 2:
    case 6:
    case 7:
        return 1;
    case 3:
    case 8:
        return 2;
    case 4:
    case 9:
    case 11:
        return 4;
    case 5:
    case 10:
    case 12:
        return 8;
    default:
        return 0;
    }
}

QVector<Entry> readIfd(const Ctx& c, qint64 off) {
    QVector<Entry> out;
    if (!c.has(off, 2))
        return out;
    const int n = c.u16(off);
    if (n <= 0 || n > 1000 || !c.has(off + 2, qint64(n) * 12))
        return out;
    for (int i = 0; i < n; ++i) {
        const qint64 e = off + 2 + qint64(i) * 12;
        out << Entry{c.u16(e), c.u16(e + 2), c.u32(e + 4), e};
    }
    return out;
}

std::optional<Entry> find(const QVector<Entry>& ifd, quint16 tag) {
    for (const Entry& e : ifd)
        if (e.tag == tag)
            return e;
    return std::nullopt;
}

// Offset (relative to c.base) of the entry's data; `dataBase` = what value offsets are
// relative to (relative to c.base, 0 for normal TIFF entries).
qint64 dataOffset(const Ctx& c, const Entry& e, qint64 dataBase = 0) {
    const qint64 size = qint64(typeSize(e.type)) * e.count;
    if (size <= 4)
        return e.pos + 8;
    return dataBase + qint64(c.u32(e.pos + 8));
}

QVector<int> shorts(const Ctx& c, const Entry& e, qint64 dataBase = 0) {
    QVector<int> v;
    if (e.type != 3 && e.type != 8)
        return v;
    const qint64 off = dataOffset(c, e, dataBase);
    if (e.count > 10000 || !c.has(off, qint64(e.count) * 2))
        return v;
    for (quint32 i = 0; i < e.count; ++i) {
        const quint16 raw = c.u16(off + qint64(i) * 2);
        v << (e.type == 8 ? int(qint16(raw)) : int(raw));
    }
    return v;
}

qint64 numberValue(const Ctx& c, const Entry& e) {
    if (e.count != 1)
        return -1;
    if (e.type == 3)
        return c.u16(e.pos + 8);
    if (e.type == 4)
        return c.u32(e.pos + 8);
    return -1;
}

QString ascii(const Ctx& c, const Entry& e) {
    if (e.type != 2 || e.count == 0 || e.count > 256)
        return {};
    const qint64 off = dataOffset(c, e);
    if (!c.has(off, e.count))
        return {};
    return QString::fromLatin1(c.d->constData() + c.base + off, int(e.count)).trimmed().remove(QChar(0));
}

QRectF normalized(double cx, double cy, double w, double h, QSizeF size) {
    if (size.width() <= 0 || size.height() <= 0)
        return {};
    return QRectF((cx - w / 2) / size.width(), (cy - h / 2) / size.height(), w / size.width(), h / size.height())
        .intersected(QRectF(0, 0, 1, 1));
}

// A box for cameras that report only a point: 4 % of the long edge.
QRectF pointBox(double x, double y, QSizeF size) {
    const double s = 0.04 * std::max(size.width(), size.height());
    return normalized(x, y, s, s, size);
}

// ---- maker notes ----

Result canon(const Ctx& c, qint64 mn) {
    Result r;
    const QVector<Entry> ifd = readIfd(c, mn);
    const auto e = find(ifd, 0x0026);
    if (!e)
        return r;
    const QVector<int> v = shorts(c, *e);
    if (v.size() < 8)
        return r;
    const int n = v[2];
    const int words = (n + 15) / 16;
    if (n <= 0 || v.size() < 8 + 4 * n + words)
        return r;
    const int valid = v[3] > 0 && v[3] <= n ? v[3] : n;
    const double W = v[6] > 0 ? v[6] : v[4], H = v[7] > 0 ? v[7] : v[5];
    if (W <= 0 || H <= 0)
        return r;
    auto bit = [&](int start, int i) {
        const int idx = start + i / 16;
        return idx < v.size() && (quint16(v[idx]) >> (i % 16)) & 1;
    };
    const bool haveSelected = v.size() >= 8 + 4 * n + 2 * words;
    for (int i = 0; i < valid; ++i) {
        const int w = v[8 + i], h = v[8 + n + i];
        const int x = int(qint16(quint16(v[8 + 2 * n + i]))), y = int(qint16(quint16(v[8 + 3 * n + i])));
        if (w <= 0 || h <= 0)
            continue;
        Point p;
        p.rect = normalized(W / 2 + x, H / 2 - y, w, h, QSizeF(W, H)); // positive Y is up
        p.inFocus = bit(8 + 4 * n, i);
        p.selected = haveSelected && bit(8 + 4 * n + words, i);
        if (!p.rect.isEmpty())
            r.points << p;
    }
    r.source = QStringLiteral("Canon");
    return r;
}

Result nikon(const Ctx& c, qint64 mn, quint32 count) {
    Result r;
    if (!c.has(mn, 18) || c.d->mid(c.base + mn, 6) != QByteArray("Nikon\0", 6))
        return r;
    Ctx inner = c;
    inner.base = c.base + mn + 10;
    const QByteArray order = c.d->mid(inner.base, 2);
    if (order != "II" && order != "MM")
        return r;
    inner.little = order == "II";
    Q_UNUSED(count);
    const auto e = find(readIfd(inner, inner.u32(4)), 0x00B7);
    if (!e || e->type != 7 || e->count < 30)
        return r;
    const qint64 off = dataOffset(inner, *e);
    if (!inner.has(off, 30) || inner.d->mid(inner.base + off, 4) != "0100")
        return r;
    const uchar contrast = uchar((*inner.d)[inner.base + off + 4]);
    const double aw = inner.u16(off + 16), ah = inner.u16(off + 18);
    const double x = inner.u16(off + 20), y = inner.u16(off + 22);
    const double w = inner.u16(off + 24), h = inner.u16(off + 26);
    if (!contrast || aw <= 0 || ah <= 0 || w <= 0 || h <= 0)
        return r;
    Point p;
    p.rect = normalized(x, y, w, h, QSizeF(aw, ah));
    p.inFocus = uchar((*inner.d)[inner.base + off + 28]) != 0;
    p.selected = true;
    if (!p.rect.isEmpty())
        r.points << p;
    r.source = QStringLiteral("Nikon");
    return r;
}

Result sony(const Ctx& c, qint64 mn) {
    Result r;
    const qint64 ifdOff = c.d->mid(c.base + mn, 4) == "SONY" ? mn + 12 : mn;
    const auto e = find(readIfd(c, ifdOff), 0x2027);
    if (!e)
        return r;
    const QVector<int> v = shorts(c, *e);
    if (v.size() < 4 || v[0] <= 0 || v[1] <= 0 || (v[2] == 0 && v[3] == 0))
        return r;
    Point p;
    p.rect = pointBox(v[2], v[3], QSizeF(v[0], v[1]));
    p.inFocus = true;
    p.selected = true;
    r.points << p;
    r.source = QStringLiteral("Sony");
    return r;
}

Result fujifilm(const Ctx& c, qint64 mn, QSize imageSize) {
    Result r;
    if (!c.has(mn, 12) || c.d->mid(c.base + mn, 8) != "FUJIFILM" || imageSize.isEmpty())
        return r;
    Ctx f = c;
    f.base = c.base + mn; // offsets inside the maker note are relative to its start
    f.little = true;
    const auto e = find(readIfd(f, f.u32(8)), 0x1023);
    if (!e)
        return r;
    const QVector<int> v = shorts(f, *e);
    if (v.size() < 2 || (v[0] == 0 && v[1] == 0))
        return r;
    Point p;
    p.rect = pointBox(v[0], v[1], QSizeF(imageSize));
    p.inFocus = true;
    p.selected = true;
    r.points << p;
    r.source = QStringLiteral("Fujifilm");
    return r;
}

Result subjectArea(const Ctx& c, const QVector<Entry>& exif, QSize imageSize) {
    Result r;
    if (imageSize.isEmpty())
        return r;
    const QSizeF size(imageSize);
    QRectF rect;
    if (const auto e = find(exif, 0x9214)) {
        const QVector<int> v = shorts(c, *e);
        if (v.size() == 2)
            rect = pointBox(v[0], v[1], size);
        else if (v.size() == 3)
            rect = normalized(v[0], v[1], v[2], v[2], size);
        else if (v.size() >= 4)
            rect = normalized(v[0], v[1], v[2], v[3], size);
    }
    if (rect.isEmpty()) {
        if (const auto e = find(exif, 0xA214)) {
            const QVector<int> v = shorts(c, *e);
            if (v.size() >= 2)
                rect = pointBox(v[0], v[1], size);
        }
    }
    if (rect.isEmpty())
        return r;
    r.points << Point{rect, true, true};
    r.source = QStringLiteral("EXIF");
    return r;
}

Result parseTiff(const QByteArray& data, qint64 base, QSize frame) {
    Ctx c;
    c.d = &data;
    c.base = base;
    const QByteArray order = data.mid(base, 2);
    if (order != "II" && order != "MM")
        return {};
    c.little = order == "II";
    const QVector<Entry> ifd0 = readIfd(c, c.u32(4));
    if (ifd0.isEmpty())
        return {};
    QString make;
    if (const auto e = find(ifd0, 0x010F))
        make = ascii(c, *e).toUpper();
    QVector<Entry> exif;
    if (const auto e = find(ifd0, 0x8769))
        exif = readIfd(c, c.u32(e->pos + 8));
    QSize size = frame;
    if (const auto w = find(exif, 0xA002), h = find(exif, 0xA003); w && h) {
        const qint64 W = numberValue(c, *w), H = numberValue(c, *h);
        if (W > 0 && H > 0)
            size = QSize(int(W), int(H));
    }
    if (size.isEmpty()) {
        if (const auto w = find(ifd0, 0x0100), h = find(ifd0, 0x0101); w && h) {
            const qint64 W = numberValue(c, *w), H = numberValue(c, *h);
            if (W > 0 && H > 0)
                size = QSize(int(W), int(H));
        }
    }
    if (const auto mnEntry = find(exif, 0x927C)) {
        const qint64 mn = qint64(c.u32(mnEntry->pos + 8));
        Result r;
        if (make.startsWith(QLatin1String("CANON")))
            r = canon(c, mn);
        else if (make.startsWith(QLatin1String("NIKON")))
            r = nikon(c, mn, mnEntry->count);
        else if (make.startsWith(QLatin1String("SONY")))
            r = sony(c, mn);
        else if (make.startsWith(QLatin1String("FUJIFILM")))
            r = fujifilm(c, mn, size);
        if (r.valid())
            return r;
    }
    return subjectArea(c, exif, size);
}

} // namespace

Result parse(const QByteArray& data) {
    if (data.size() < 16)
        return {};
    // Fujifilm RAF: the embedded JPEG carries the EXIF with the maker note.
    if (data.startsWith("FUJIFILMCCD-RAW")) {
        Ctx c;
        c.d = &data;
        c.little = false;
        const qint64 off = c.u32(84), len = c.u32(88);
        if (off > 0 && len > 0 && off + len <= data.size())
            return parse(QByteArray::fromRawData(data.constData() + off, qsizetype(len)));
        return {};
    }
    if (uchar(data[0]) == 0xFF && uchar(data[1]) == 0xD8) {
        // JPEG: the EXIF APP1 segment.
        qint64 pos = 2;
        while (pos + 4 <= data.size() && uchar(data[pos]) == 0xFF) {
            const uchar marker = uchar(data[pos + 1]);
            if (marker == 0xDA || marker == 0xD9)
                break;
            const qint64 len = (uchar(data[pos + 2]) << 8) | uchar(data[pos + 3]);
            if (len < 2 || pos + 2 + len > data.size())
                break;
            if (marker == 0xE1 && len > 8 && data.mid(pos + 4, 6) == QByteArray("Exif\0\0", 6))
                return parseTiff(data, pos + 10, jpeg::frameSize(data));
            pos += 2 + len;
        }
        return {};
    }
    // TIFF based (TIFF, DNG, CR2, NEF, ARW …).
    if (data.startsWith(QByteArray("II*\0", 4)) || data.startsWith(QByteArray("MM\0*", 4)))
        return parseTiff(data, 0, QSize());
    return {};
}

Result read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 16)
        return {};
    if (uchar* p = f.map(0, f.size())) {
        const Result r = parse(QByteArray::fromRawData(reinterpret_cast<const char*>(p), qsizetype(f.size())));
        f.unmap(p);
        return r;
    }
    return parse(f.read(4 * 1024 * 1024));
}

QVector<Point> toDisplay(const QVector<Point>& points, int exifOrientation) {
    // Normalized coordinates: the orientation of a 1 x 1 image.
    QVector<Point> out;
    const QTransform t = crop::storedToDisplay(QSize(1, 1), exifOrientation);
    for (Point p : points) {
        p.rect = t.mapRect(p.rect);
        out << p;
    }
    return out;
}

} // namespace lf::afpoints
