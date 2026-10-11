// SPDX-License-Identifier: GPL-3.0-or-later
// Cropping (F-EDIT-03), see CropSave.h.

#include "CropSave.h"

#include "Decoder.h"
#include "FileOps.h"
#include "ImageFormats.h"
#include "UndoBackupStore.h"
#include "VirtualPath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>

#ifdef LF_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::CropSave)
};

// Same matrices as LosslessTransform.cpp (x' = m0 x + m1 y, y' = m2 x + m3 y; stored ->
// displayed pixels for EXIF orientation 1..8).
constexpr std::array<std::array<int, 4>, 9> kOrient = {{{1, 0, 0, 1},
                                                        {1, 0, 0, 1},
                                                        {-1, 0, 0, 1},
                                                        {-1, 0, 0, -1},
                                                        {1, 0, 0, -1},
                                                        {0, 1, 1, 0},
                                                        {0, -1, 1, 0},
                                                        {0, -1, -1, 0},
                                                        {0, 1, -1, 0}}};

// ---- JPEG byte level (small, independent of LosslessTransform.cpp's private helpers) ----

quint16 be16(const QByteArray& d, qsizetype i) {
    return quint16((uchar(d[i]) << 8) | uchar(d[i + 1]));
}

struct Seg {
    uchar marker = 0;
    qsizetype payload = 0;
    qsizetype length = 0;
};

// Marker segments before the first SOS; fn returns false to stop.
template <typename Fn> void eachSegment(const QByteArray& d, Fn fn) {
    if (d.size() < 4 || uchar(d[0]) != 0xFF || uchar(d[1]) != 0xD8)
        return;
    qsizetype pos = 2;
    while (pos + 4 <= d.size()) {
        if (uchar(d[pos]) != 0xFF)
            return;
        qsizetype m = pos + 1;
        while (m < d.size() && uchar(d[m]) == 0xFF)
            ++m;
        if (m >= d.size())
            return;
        const uchar marker = uchar(d[m]);
        if (marker == 0xDA)
            return;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos = m + 1;
            continue;
        }
        if (m + 3 > d.size())
            return;
        const qsizetype len = be16(d, m + 1);
        if (len < 2 || m + 1 + len > d.size())
            return;
        if (!fn(Seg{marker, m + 3, len - 2}))
            return;
        pos = m + 1 + len;
    }
}

struct Tiff {
    qsizetype base = 0;
    qsizetype size = 0;
    bool little = true;
    quint16 u16(const QByteArray& d, qsizetype off) const {
        const qsizetype i = base + off;
        return little ? quint16(uchar(d[i]) | (uchar(d[i + 1]) << 8)) : be16(d, i);
    }
    quint32 u32(const QByteArray& d, qsizetype off) const {
        quint32 v = 0;
        for (int k = 0; k < 4; ++k) {
            const quint32 b = uchar(d[base + off + k]);
            v |= little ? b << (8 * k) : b << (8 * (3 - k));
        }
        return v;
    }
    void put16(QByteArray& d, qsizetype off, quint16 v) const {
        d[base + off] = char(little ? (v & 0xFF) : (v >> 8));
        d[base + off + 1] = char(little ? (v >> 8) : (v & 0xFF));
    }
    void put32(QByteArray& d, qsizetype off, quint32 v) const {
        for (int k = 0; k < 4; ++k)
            d[base + off + k] = char((v >> (little ? 8 * k : 8 * (3 - k))) & 0xFF);
    }
};

bool findExif(const QByteArray& d, Tiff* tiff) {
    bool found = false;
    eachSegment(d, [&](const Seg& s) {
        if (s.marker != 0xE1 || s.length < 14 || d.mid(s.payload, 6) != QByteArray("Exif\0\0", 6))
            return true;
        Tiff t;
        t.base = s.payload + 6;
        t.size = s.length - 6;
        const QByteArray order = d.mid(t.base, 2);
        if (order != "II" && order != "MM")
            return true;
        t.little = order == "II";
        if (t.u16(d, 2) != 42)
            return true;
        *tiff = t;
        found = true;
        return false;
    });
    return found;
}

// Offset of the entry `tag` in the IFD at `ifd` (relative to the TIFF header), -1 if absent.
qsizetype ifdEntry(const QByteArray& d, const Tiff& t, quint32 ifd, quint16 tag) {
    if (ifd < 8 || qsizetype(ifd) + 2 > t.size)
        return -1;
    const int n = t.u16(d, ifd);
    if (qsizetype(ifd) + 2 + qsizetype(n) * 12 > t.size)
        return -1;
    for (int i = 0; i < n; ++i) {
        const qsizetype e = qsizetype(ifd) + 2 + qsizetype(i) * 12;
        if (t.u16(d, e) == tag)
            return e;
    }
    return -1;
}

// Offset of the Exif IFD (relative to the TIFF header), 0 if none.
quint32 exifIfd(const QByteArray& d, const Tiff& t) {
    const qsizetype e = ifdEntry(d, t, t.u32(d, 4), 0x8769);
    return e < 0 ? 0 : t.u32(d, e + 8);
}

// Value of a SHORT/LONG entry with count 1, -1 otherwise.
qint64 entryValue(const QByteArray& d, const Tiff& t, qsizetype e) {
    if (e < 0 || t.u32(d, e + 4) != 1)
        return -1;
    const quint16 type = t.u16(d, e + 2);
    if (type == 3)
        return t.u16(d, e + 8);
    if (type == 4)
        return t.u32(d, e + 8);
    return -1;
}

int clampOrientation(int o) {
    return o >= 1 && o <= 8 ? o : 1;
}

bool isJpegType(const QString& typeId) {
    return typeId == QLatin1String("public.jpeg");
}

QString messageChangedSincePlan() {
    return Tr::tr("Die Datei wurde inzwischen verändert – nichts gesichert. Schneide das Bild erneut zu.");
}

QString messageAdditionalImages() {
    return Tr::tr("Die Datei enthält Zusatzbilder (z. B. eine HDR-Gain-Map oder ein zweites Stereobild), die "
                  "nach einem verlustfreien Zuschnitt nicht mehr passen würden. Zuschneiden ist nur durch "
                  "Neukodieren möglich – dabei gehen etwas Bildqualität und diese Zusatzbilder verloren.");
}

QString tempProbePath(const QString& suffix) {
    return QDir(QDir::tempPath())
        .filePath(QStringLiteral("lumaflip-crop-probe-%1.%2")
                      .arg(QUuid::createUuid().toString(QUuid::Id128).left(12), suffix));
}

} // namespace

// ---------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------

QString crop::aspectId(Aspect a) {
    switch (a) {
    case Aspect::Free:
        return QStringLiteral("free");
    case Aspect::Original:
        return QStringLiteral("original");
    case Aspect::Square:
        return QStringLiteral("1:1");
    case Aspect::R4x3:
        return QStringLiteral("4:3");
    case Aspect::R3x2:
        return QStringLiteral("3:2");
    case Aspect::R16x9:
        return QStringLiteral("16:9");
    }
    return QStringLiteral("free");
}

crop::Aspect crop::aspectFromId(const QString& id) {
    for (Aspect a :
         {Aspect::Free, Aspect::Original, Aspect::Square, Aspect::R4x3, Aspect::R3x2, Aspect::R16x9})
        if (aspectId(a) == id)
            return a;
    return Aspect::Free;
}

QString crop::aspectLabel(Aspect a) {
    switch (a) {
    case Aspect::Free:
        return Tr::tr("Frei");
    case Aspect::Original:
        return Tr::tr("Original");
    default:
        return aspectId(a); // "4:3" … (the same in every language)
    }
}

double crop::aspectRatio(Aspect a, QSizeF original, bool portrait) {
    double r = 0;
    switch (a) {
    case Aspect::Free:
        return 0;
    case Aspect::Original:
        // The image's own ratio; the portrait toggle swaps it like the others.
        if (original.width() <= 0 || original.height() <= 0)
            return 0;
        r = original.width() / original.height();
        return portrait ? 1.0 / r : r;
    case Aspect::Square:
        return 1.0;
    case Aspect::R4x3:
        r = 4.0 / 3.0;
        break;
    case Aspect::R3x2:
        r = 3.0 / 2.0;
        break;
    case Aspect::R16x9:
        r = 16.0 / 9.0;
        break;
    }
    return portrait ? 1.0 / r : r;
}

QRectF crop::fitRatio(const QRectF& bounds, double ratio) {
    if (ratio <= 0 || bounds.isEmpty())
        return bounds;
    double w = bounds.width(), h = w / ratio;
    if (h > bounds.height()) {
        h = bounds.height();
        w = h * ratio;
    }
    return QRectF(bounds.center().x() - w / 2, bounds.center().y() - h / 2, w, h);
}

std::array<double, 4> crop::thirds(const QRectF& r) {
    return {r.left() + r.width() / 3.0, r.left() + 2.0 * r.width() / 3.0, r.top() + r.height() / 3.0,
            r.top() + 2.0 * r.height() / 3.0};
}

namespace {

// Corner drag: `fixed` stays, the moving corner goes towards (sx, sy) = direction signs.
QRectF cornerRect(QPointF fixed, QPointF moving, int sx, int sy, const QRectF& b, double ratio,
                  double minSize) {
    double w = std::max(minSize, (moving.x() - fixed.x()) * sx);
    double h = std::max(minSize, (moving.y() - fixed.y()) * sy);
    const double maxW = sx > 0 ? b.right() - fixed.x() : fixed.x() - b.left();
    const double maxH = sy > 0 ? b.bottom() - fixed.y() : fixed.y() - b.top();
    if (ratio > 0) {
        // The larger of both proposals wins (the corner follows the pointer).
        if (w / h > ratio)
            h = w / ratio;
        else
            w = h * ratio;
        if (w > maxW) {
            w = maxW;
            h = w / ratio;
        }
        if (h > maxH) {
            h = maxH;
            w = h * ratio;
        }
    } else {
        w = std::min(w, maxW);
        h = std::min(h, maxH);
    }
    const double x = sx > 0 ? fixed.x() : fixed.x() - w;
    const double y = sy > 0 ? fixed.y() : fixed.y() - h;
    return QRectF(x, y, w, h);
}

} // namespace

QRectF crop::dragRect(const QRectF& s, Handle handle, QPointF delta, const QRectF& b, double ratio,
                      double minSize, QPointF anchor) {
    minSize = std::min({minSize, b.width(), b.height()});
    switch (handle) {
    case Handle::None:
        return s;
    case Handle::Move: {
        QRectF r = s.translated(delta);
        r.moveLeft(std::clamp(r.left(), b.left(), std::max(b.left(), b.right() - r.width())));
        r.moveTop(std::clamp(r.top(), b.top(), std::max(b.top(), b.bottom() - r.height())));
        return r;
    }
    case Handle::New: {
        const QPointF a(std::clamp(anchor.x(), b.left(), b.right()),
                        std::clamp(anchor.y(), b.top(), b.bottom()));
        const QPointF p = a + delta;
        return cornerRect(a, p, p.x() >= a.x() ? 1 : -1, p.y() >= a.y() ? 1 : -1, b, ratio, minSize);
    }
    case Handle::TopLeft:
        return cornerRect(s.bottomRight(), s.topLeft() + delta, -1, -1, b, ratio, minSize);
    case Handle::TopRight:
        return cornerRect(s.bottomLeft(), s.topRight() + delta, 1, -1, b, ratio, minSize);
    case Handle::BottomLeft:
        return cornerRect(s.topRight(), s.bottomLeft() + delta, -1, 1, b, ratio, minSize);
    case Handle::BottomRight:
        return cornerRect(s.topLeft(), s.bottomRight() + delta, 1, 1, b, ratio, minSize);
    case Handle::Left:
    case Handle::Right: {
        const bool right = handle == Handle::Right;
        const double fixedX = right ? s.left() : s.right();
        const double maxW = right ? b.right() - fixedX : fixedX - b.left();
        double w = std::clamp(s.width() + (right ? delta.x() : -delta.x()), minSize, std::max(minSize, maxW));
        double h = s.height();
        double y = s.top();
        if (ratio > 0) {
            h = w / ratio;
            if (h > b.height()) {
                h = b.height();
                w = h * ratio;
            }
            y = std::clamp(s.center().y() - h / 2, b.top(), b.bottom() - h);
        }
        return QRectF(right ? fixedX : fixedX - w, y, w, h);
    }
    case Handle::Top:
    case Handle::Bottom: {
        const bool bottom = handle == Handle::Bottom;
        const double fixedY = bottom ? s.top() : s.bottom();
        const double maxH = bottom ? b.bottom() - fixedY : fixedY - b.top();
        double h =
            std::clamp(s.height() + (bottom ? delta.y() : -delta.y()), minSize, std::max(minSize, maxH));
        double w = s.width();
        double x = s.left();
        if (ratio > 0) {
            w = h * ratio;
            if (w > b.width()) {
                w = b.width();
                h = w / ratio;
            }
            x = std::clamp(s.center().x() - w / 2, b.left(), b.right() - w);
        }
        return QRectF(x, bottom ? fixedY : fixedY - h, w, h);
    }
    }
    return s;
}

QTransform crop::storedToDisplay(QSize storedSize, int orientation) {
    const auto& m = kOrient[size_t(clampOrientation(orientation))];
    const double w = storedSize.width(), h = storedSize.height();
    double minX = 0, minY = 0;
    for (const auto& c : {std::array<double, 2>{0, 0}, {w, 0}, {0, h}, {w, h}}) {
        minX = std::min(minX, m[0] * c[0] + m[1] * c[1]);
        minY = std::min(minY, m[2] * c[0] + m[3] * c[1]);
    }
    // QTransform: x' = m11 x + m21 y + dx, y' = m12 x + m22 y + dy.
    return QTransform(m[0], m[2], m[1], m[3], -minX, -minY);
}

QRect crop::displayToStoredRect(const QRect& r, QSize storedSize, int orientation) {
    const QTransform inv = storedToDisplay(storedSize, orientation).inverted();
    return inv.mapRect(QRectF(r)).toAlignedRect();
}

QRect crop::storedToDisplayRect(const QRect& r, QSize storedSize, int orientation) {
    return storedToDisplay(storedSize, orientation).mapRect(QRectF(r)).toAlignedRect();
}

QRect crop::snapToMcu(const QRect& stored, QSize mcu, QSize imageSize) {
    QRect r = stored.intersected(QRect(QPoint(0, 0), imageSize));
    if (!mcu.isValid() || mcu.isEmpty() || r.isEmpty())
        return r;
    const int x = (r.left() / mcu.width()) * mcu.width();
    const int y = (r.top() / mcu.height()) * mcu.height();
    return QRect(QPoint(x, y), r.bottomRight());
}

// ---------------------------------------------------------------------------------------
// JPEG
// ---------------------------------------------------------------------------------------

QSize jpeg::frameSize(const QByteArray& d) {
    QSize s;
    eachSegment(d, [&](const Seg& seg) {
        const uchar m = seg.marker;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && seg.length >= 5) {
            s = QSize(be16(d, seg.payload + 3), be16(d, seg.payload + 1));
            return false;
        }
        return true;
    });
    return s;
}

QSize jpeg::mcuSize(const QByteArray& data) {
#ifdef LF_HAVE_TURBOJPEG
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (!h)
        return {};
    QSize s;
    if (tj3DecompressHeader(h, reinterpret_cast<const unsigned char*>(data.constData()),
                            size_t(data.size())) == 0) {
        const int ss = tj3Get(h, TJPARAM_SUBSAMP);
        if (ss >= 0 && ss < TJ_NUMSAMP)
            s = QSize(tjMCUWidth[ss], tjMCUHeight[ss]);
    }
    tj3Destroy(h);
    return s;
#else
    Q_UNUSED(data);
    return {};
#endif
}

jpeg::DctResult jpeg::cropDct(const QByteArray& in, const QRect& stored, QByteArray* out, QString* error) {
#ifdef LF_HAVE_TURBOJPEG
    tjhandle h = tj3Init(TJINIT_TRANSFORM);
    if (!h) {
        if (error)
            *error = Tr::tr("Die JPEG-Bibliothek konnte nicht gestartet werden.");
        return DctResult::Failed;
    }
    const auto* src = reinterpret_cast<const unsigned char*>(in.constData());
    auto fail = [&]() {
        if (error)
            *error = Tr::tr("Die JPEG-Daten konnten nicht verlustfrei zugeschnitten werden (%1).")
                         .arg(QString::fromUtf8(tj3GetErrorStr(h)));
        tj3Destroy(h);
        return DctResult::Failed;
    };
    if (tj3DecompressHeader(h, src, size_t(in.size())) < 0)
        return fail();
    const int ss = tj3Get(h, TJPARAM_SUBSAMP);
    const int w = tj3Get(h, TJPARAM_JPEGWIDTH), hgt = tj3Get(h, TJPARAM_JPEGHEIGHT);
    if (ss < 0 || ss >= TJ_NUMSAMP || stored.x() % tjMCUWidth[ss] != 0 || stored.y() % tjMCUHeight[ss] != 0 ||
        !QRect(0, 0, w, hgt).contains(stored) || stored.isEmpty()) {
        tj3Destroy(h);
        if (error)
            *error = Tr::tr("Der Zuschnitt liegt nicht auf dem JPEG-Blockraster.");
        return DctResult::NotPerfect;
    }
    tjtransform xf{};
    xf.op = TJXOP_NONE;
    xf.options = TJXOPT_CROP;
    xf.r.x = stored.x();
    xf.r.y = stored.y();
    xf.r.w = stored.width();
    xf.r.h = stored.height();
    unsigned char* dst = nullptr;
    size_t dstSize = 0;
    if (tj3Transform(h, src, size_t(in.size()), 1, &dst, &dstSize, &xf) < 0) {
        tj3Free(dst);
        return fail();
    }
    *out = QByteArray(reinterpret_cast<const char*>(dst), qsizetype(dstSize));
    tj3Free(dst);
    tj3Destroy(h);
    return DctResult::Ok;
#else
    Q_UNUSED(in);
    Q_UNUSED(stored);
    Q_UNUSED(out);
    if (error)
        *error = Tr::tr("Verlustfreies Zuschneiden von JPEG-Dateien ist in dieser Version nicht verfügbar.");
    return DctResult::Unavailable;
#endif
}

QSize jpeg::exifPixelDimensions(const QByteArray& d) {
    Tiff t;
    if (!findExif(d, &t))
        return {};
    const quint32 ifd = exifIfd(d, t);
    if (!ifd)
        return {};
    const qint64 w = entryValue(d, t, ifdEntry(d, t, ifd, 0xA002));
    const qint64 h = entryValue(d, t, ifdEntry(d, t, ifd, 0xA003));
    return w > 0 && h > 0 ? QSize(int(w), int(h)) : QSize();
}

bool jpeg::patchPixelDimensions(QByteArray& d, int width, int height) {
    Tiff t;
    if (!findExif(d, &t))
        return true; // nothing to update
    const quint32 ifd = exifIfd(d, t);
    if (!ifd)
        return true;
    bool ok = true;
    for (auto [tag, value] : {std::pair<quint16, int>{0xA002, width}, {0xA003, height}}) {
        const qsizetype e = ifdEntry(d, t, ifd, tag);
        if (e < 0)
            continue;
        if (t.u32(d, e + 4) != 1) {
            ok = false;
            continue;
        }
        const quint16 type = t.u16(d, e + 2);
        if (type == 3 && value <= 0xFFFF) {
            t.put16(d, e + 8, quint16(value));
            t.put16(d, e + 10, 0);
        } else if (type == 4) {
            t.put32(d, e + 8, quint32(value));
        } else {
            ok = false;
        }
    }
    return ok;
}

bool jpeg::removeExifThumbnail(QByteArray& d) {
    Tiff t;
    if (!findExif(d, &t))
        return false;
    const quint32 ifd0 = t.u32(d, 4);
    if (ifd0 < 8 || qsizetype(ifd0) + 2 > t.size)
        return false;
    const qsizetype next = qsizetype(ifd0) + 2 + qsizetype(t.u16(d, ifd0)) * 12;
    if (next + 4 > t.size)
        return false;
    const quint32 ifd1 = t.u32(d, next);
    if (ifd1 == 0)
        return false;
    t.put32(d, next, 0); // unlinked
    if (ifd1 < 8 || qsizetype(ifd1) + 2 > t.size)
        return true;
    // The thumbnail JPEG (JPEGInterchangeFormat / ...Length) and the IFD itself: zeroed.
    const qint64 offset = entryValue(d, t, ifdEntry(d, t, ifd1, 0x0201));
    const qint64 length = entryValue(d, t, ifdEntry(d, t, ifd1, 0x0202));
    if (offset >= 8 && length > 0 && offset + length <= t.size)
        std::fill(d.begin() + t.base + offset, d.begin() + t.base + offset + length, '\0');
    const qsizetype ifdEnd = qsizetype(ifd1) + 2 + qsizetype(t.u16(d, ifd1)) * 12 + 4;
    if (ifdEnd <= t.size)
        std::fill(d.begin() + t.base + ifd1, d.begin() + t.base + ifdEnd, '\0');
    return true;
}

namespace {

// Replaces the payload of the segment at `s` (Seg::payload/length) with `payload`.
void replacePayload(QByteArray& d, const Seg& s, const QByteArray& payload) {
    const qsizetype len = payload.size() + 2;
    QByteArray head;
    head.append(char((len >> 8) & 0xFF)).append(char(len & 0xFF));
    d = d.left(s.payload - 2) + head + payload + d.mid(s.payload + s.length);
}

} // namespace

int jpeg::removeIrbThumbnails(QByteArray& d) {
    static const QByteArray id("Photoshop 3.0\0", 14);
    QList<Seg> segments;
    eachSegment(d, [&](const Seg& s) {
        if (s.marker == 0xED && s.length > id.size() && d.mid(s.payload, id.size()) == id)
            segments << s;
        return true;
    });
    int removed = 0;
    for (qsizetype i = segments.size() - 1; i >= 0; --i) { // back to front: offsets stay
        const Seg& s = segments.at(i);
        const qsizetype end = s.payload + s.length;
        QByteArray kept = id;
        qsizetype p = s.payload + id.size();
        int dropped = 0;
        bool ok = true;
        while (p < end) {
            if (p + 7 > end || d.mid(p, 4) != "8BIM") {
                ok = false; // not a resource block: leave the segment as it is
                break;
            }
            const quint16 res = be16(d, p + 4);
            qsizetype q = p + 6;
            const int nameLen = uchar(d[q]);
            q += 1 + nameLen;
            if ((1 + nameLen) % 2)
                ++q; // the Pascal name is padded to an even length
            if (q + 4 > end) {
                ok = false;
                break;
            }
            const qsizetype size = (qsizetype(uchar(d[q])) << 24) | (qsizetype(uchar(d[q + 1])) << 16) |
                                   (qsizetype(uchar(d[q + 2])) << 8) | qsizetype(uchar(d[q + 3]));
            qsizetype blockEnd = q + 4 + size;
            if (size % 2)
                ++blockEnd;
            if (blockEnd > end) {
                ok = false;
                break;
            }
            if (res == 0x0409 || res == 0x040C)
                ++dropped;
            else
                kept += d.mid(p, blockEnd - p);
            p = blockEnd;
        }
        if (ok && dropped > 0) {
            replacePayload(d, s, kept);
            removed += dropped;
        }
    }
    return removed;
}

void jpeg::patchXmpDimensions(QByteArray& d, int width, int height) {
    static const QByteArray id("http://ns.adobe.com/xap/1.0/\0", 29);
    QList<Seg> segments;
    eachSegment(d, [&](const Seg& s) {
        if (s.marker == 0xE1 && s.length > id.size() && d.mid(s.payload, id.size()) == id)
            segments << s;
        return true;
    });
    for (qsizetype i = segments.size() - 1; i >= 0; --i) { // back to front: offsets stay
        const Seg& s = segments.at(i);
        const QByteArray raw = d.mid(s.payload + id.size(), s.length - id.size());
        QString xml = QString::fromUtf8(raw);
        if (xml.toUtf8() != raw)
            continue; // not clean UTF-8: left byte-identical
        const QString before = xml;
        for (const auto& [name, value] : {std::pair<const char*, int>{"exif:PixelXDimension", width},
                                          {"exif:PixelYDimension", height},
                                          {"tiff:ImageWidth", width},
                                          {"tiff:ImageLength", height}}) {
            const QString n = QRegularExpression::escape(QString::fromLatin1(name));
            const QString v = QString::number(value);
            // Only the digits (capture 1) are replaced, in the attribute and the element form.
            for (const QString& pattern : {QStringLiteral("%1\\s*=\\s*[\"'](\\d+)[\"']").arg(n),
                                           QStringLiteral("<%1>\\s*(\\d+)\\s*</%1>").arg(n)}) {
                QString out;
                qsizetype last = 0;
                QRegularExpressionMatchIterator m = QRegularExpression(pattern).globalMatch(xml);
                while (m.hasNext()) {
                    const QRegularExpressionMatch match = m.next();
                    out += xml.mid(last, match.capturedStart(1) - last) + v;
                    last = match.capturedEnd(1);
                }
                xml = out + xml.mid(last);
            }
        }
        if (xml == before)
            continue;
        const QByteArray payload = id + xml.toUtf8();
        if (payload.size() + 2 <= 0xFFFF)
            replacePayload(d, s, payload);
    }
}

QImage jpeg::decodePixels(const QByteArray& data, bool fastUpsample) {
#ifdef LF_HAVE_TURBOJPEG
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (!h)
        return {};
    QImage img;
    const auto* src = reinterpret_cast<const unsigned char*>(data.constData());
    if (tj3DecompressHeader(h, src, size_t(data.size())) == 0) {
        const int w = tj3Get(h, TJPARAM_JPEGWIDTH), hgt = tj3Get(h, TJPARAM_JPEGHEIGHT);
        tj3Set(h, TJPARAM_FASTUPSAMPLE, fastUpsample ? 1 : 0);
        img = QImage(w, hgt, QImage::Format_RGB888);
        if (!img.isNull() &&
            tj3Decompress8(h, src, size_t(data.size()), img.bits(), int(img.bytesPerLine()), TJPF_RGB) < 0)
            img = QImage();
    }
    tj3Destroy(h);
    return img;
#else
    Q_UNUSED(data);
    Q_UNUSED(fastUpsample);
    return {};
#endif
}

// ---------------------------------------------------------------------------------------
// Planning and saving
// ---------------------------------------------------------------------------------------

namespace {

CropPlan planCropSaveImpl(const CropRequest& r) {
    CropPlan plan;
    if (vpath::isMember(r.path)) {
        plan.message = Tr::tr("Bilder in Archiven und PDF-Dokumenten werden nicht verändert. Der Zuschnitt "
                              "kann als neue Datei exportiert werden.");
        plan.exportInstead = true;
        return plan;
    }
    if (!QFileInfo::exists(r.path)) {
        plan.message = Tr::tr("Die Datei existiert nicht mehr.");
        return plan;
    }
    plan.target = resolvedFilePath(r.path);
    const QFileInfo fi(plan.target);
    if (!fi.isFile()) {
        plan.message = Tr::tr("Die Datei existiert nicht mehr.");
        return plan;
    }
    plan.typeId = platform::imageTypeIdentifier(plan.target);
    if (plan.typeId.isEmpty()) {
        plan.message = Tr::tr("Das Dateiformat wird zum Sichern nicht erkannt.");
        return plan;
    }
    if (platform::isCameraRawType(plan.typeId) || isRawSuffix(fi.suffix().toLower()) ||
        isRawSuffix(QFileInfo(r.path).suffix().toLower())) {
        plan.message =
            Tr::tr("RAW-Dateien werden nie verändert. Der Zuschnitt kann als neue Datei exportiert werden.");
        plan.exportInstead = true;
        return plan;
    }
    if (platform::imageCount(plan.target) > 1) {
        plan.message =
            Tr::tr("Animierte oder mehrseitige Dateien können nicht zugeschnitten gesichert werden. Der "
                   "Zuschnitt des ersten Bildes kann als neue Datei exportiert werden.");
        plan.exportInstead = true;
        return plan;
    }
    if (r.saveAs.isEmpty()) {
        plan.destination = plan.target;
        if (!fi.isWritable() || !QFileInfo(fi.absolutePath()).isWritable()) {
            plan.message = Tr::tr("„%1“ ist schreibgeschützt.").arg(fi.fileName());
            return plan;
        }
    } else {
        plan.destination = resolvedFilePath(r.saveAs);
        if (plan.destination == plan.target) {
            plan.message =
                Tr::tr("„Sichern unter“ überschreibt nie das Original – wähle einen anderen Namen.");
            return plan;
        }
        const QFileInfo dest(plan.destination);
        if (!QFileInfo(dest.absolutePath()).isWritable() || (dest.exists() && !dest.isFile())) {
            plan.message = Tr::tr("In „%1“ kann nicht geschrieben werden.").arg(dest.absolutePath());
            return plan;
        }
        if ((dest.exists() || dest.isSymLink()) && !r.replaceExisting) {
            plan.message = Tr::tr("„%1“ existiert bereits.").arg(dest.fileName());
            return plan;
        }
    }
    plan.stamp = fileStamp(plan.target);
    if (!plan.stamp.isValid()) {
        plan.message = Tr::tr("„%1“ kann nicht gelesen werden.").arg(fi.fileName());
        return plan;
    }

    // Stored size and orientation.
    const bool isJpeg = isJpegType(plan.typeId);
    QByteArray bytes;
    QSize stored;
    int orient = 1;
    if (isJpeg) {
        bytes = filesafety::readFile(plan.target, &plan.message);
        if (bytes.isEmpty())
            return plan;
        stored = jpeg::frameSize(bytes);
        orient = clampOrientation(jpeg::exifOrientation(bytes));
    }
    if (!stored.isValid()) {
        QString err;
        auto info = DecoderRegistry::instance().info(plan.target, &err);
        if (!info) {
            plan.message = err.isEmpty() ? Tr::tr("„%1“ kann nicht gelesen werden.").arg(fi.fileName()) : err;
            return plan;
        }
        stored = info->pixelSize;
        orient = clampOrientation(info->exifOrientation);
    }
    const QSize displaySize = crop::storedToDisplayRect(QRect(QPoint(0, 0), stored), stored, orient).size();
    const QRect wanted = r.rect.intersected(QRect(QPoint(0, 0), displaySize));
    if (wanted.isEmpty()) {
        plan.message = Tr::tr("Kein Bildbereich ausgewählt.");
        return plan;
    }
    if (wanted == QRect(QPoint(0, 0), displaySize)) {
        plan.message = Tr::tr("Der Zuschnitt umfasst das ganze Bild – es gibt nichts zuzuschneiden.");
        return plan;
    }
    plan.storedRect = crop::displayToStoredRect(wanted, stored, orient);
    plan.displayRect = wanted;
    plan.status = CropPlan::Ready;

    const QStringList aux = platform::auxiliaryImageNames(plan.target);
    if (isJpeg) {
        if (jpeg::hasAdditionalImages(bytes)) {
            plan.status = CropPlan::NeedsReencode;
            plan.method = CropMethod::ReencodeLossy;
            plan.message = messageAdditionalImages();
            return plan;
        }
        plan.mcu = jpeg::mcuSize(bytes);
        if (plan.mcu.isValid()) {
            const QRect snapped = crop::snapToMcu(plan.storedRect, plan.mcu, stored);
            QByteArray probe;
            QString err;
            if (jpeg::cropDct(bytes, snapped, &probe, &err) == jpeg::DctResult::Ok) {
                plan.snapped = snapped != plan.storedRect;
                plan.storedRect = snapped;
                plan.displayRect = crop::storedToDisplayRect(snapped, stored, orient);
                plan.method = CropMethod::JpegLossless;
                if (plan.snapped)
                    plan.notice =
                        Tr::tr("Für den verlustfreien Zuschnitt wurde der Zuschnittbereich an das "
                               "JPEG-Blockraster (%1 × %2 Pixel) angepasst und ist jetzt %3 × %4 Pixel groß.")
                            .arg(plan.mcu.width())
                            .arg(plan.mcu.height())
                            .arg(plan.displayRect.width())
                            .arg(plan.displayRect.height());
                return plan;
            }
        }
        plan.status = CropPlan::NeedsReencode;
        plan.method = CropMethod::ReencodeLossy;
        plan.message = Tr::tr("Diese JPEG-Datei lässt sich nicht verlustfrei zuschneiden. Beim Neukodieren "
                              "geht etwas Bildqualität verloren.");
        return plan;
    }
    if (platform::isLosslessCodec(plan.typeId)) {
        const QString probe = tempProbePath(fi.suffix());
        QString err;
        bool inexact = false;
        const bool exact =
            platform::reencodeCropped(plan.target, probe, plan.storedRect, true, &err, &inexact);
        QFile::remove(probe);
        if (exact) {
            plan.method = CropMethod::ReencodeLossless;
            return plan;
        }
        if (!inexact) {
            plan.status = CropPlan::Refused;
            plan.message = err.isEmpty() ? Tr::tr("Der Zuschnitt kann nicht gesichert werden.") : err;
            return plan;
        }
        plan.status = CropPlan::NeedsReencode;
        plan.method = CropMethod::ReencodeLossy;
        plan.message = Tr::tr("Beim Neukodieren bliebe das Bild nicht exakt gleich (Farbmodell, Bittiefe, "
                              "Palette oder Transparenz würden verändert).");
        return plan;
    }
    if (!platform::canEncodeType(plan.typeId)) {
        // Asking "Neu kodieren?" would only end in an error.
        plan.status = CropPlan::Refused;
        plan.message = Tr::tr("Dieses Format kann hier nicht geschrieben werden. Der Zuschnitt kann in einem "
                              "anderen Format exportiert werden.");
        plan.exportInstead = true;
        return plan;
    }
    plan.status = CropPlan::NeedsReencode;
    plan.method = CropMethod::ReencodeLossy;
    plan.message = Tr::tr("Dieses Format komprimiert verlustbehaftet. Der Zuschnitt lässt sich nur sichern, "
                          "indem das Bild neu kodiert wird – dabei geht etwas Bildqualität verloren.");
    if (!aux.isEmpty())
        plan.message += QLatin1Char(' ') +
                        Tr::tr("Zusatzbilder (%1) gehen dabei verloren.").arg(aux.join(QStringLiteral(", ")));
    return plan;
}

} // namespace

CropPlan planCropSave(const CropRequest& r) {
    CropPlan plan = planCropSaveImpl(r);
    const bool reencode =
        plan.method == CropMethod::ReencodeLossless || plan.method == CropMethod::ReencodeLossy;
    if (plan.status != CropPlan::Refused && reencode && !platform::reencodeKeepsMetadata()) {
        // Never silently: Qt's writers keep the pixels only.
        const QString lost = Tr::tr("Metadaten (EXIF, XMP, IPTC) gehen beim Neukodieren verloren.");
        plan.notice = plan.notice.isEmpty() ? lost : plan.notice + QLatin1Char(' ') + lost;
        if (plan.status == CropPlan::NeedsReencode)
            plan.message += QLatin1Char(' ') + lost;
    }
    return plan;
}

QString executeCropSave(const CropRequest& r, const CropPlan& plan, CropMethod method) {
    if (method == CropMethod::None)
        return {};
    const QString target = plan.target.isEmpty() ? resolvedFilePath(r.path) : plan.target;
    const QString destination = plan.destination.isEmpty() ? target : plan.destination;
    const bool inPlace = destination == target;
    if (!inPlace && resolvedFilePath(destination) == target)
        return Tr::tr("„Sichern unter“ überschreibt nie das Original – wähle einen anderen Namen.");
    QString error;
    const QString tmp = filesafety::siblingTempPath(destination);
    bool ok = false;
    switch (method) {
    case CropMethod::JpegLossless: {
        const QByteArray data = filesafety::readFile(target, &error);
        if (data.isEmpty())
            return error;
        if (jpeg::hasAdditionalImages(data))
            return messageAdditionalImages();
        QByteArray out;
        if (jpeg::cropDct(data, plan.storedRect, &out, &error) != jpeg::DctResult::Ok)
            return error.isEmpty() ? Tr::tr("Verlustfreies Zuschneiden ist für dieses Bild nicht möglich.")
                                   : error;
        jpeg::patchPixelDimensions(out, plan.storedRect.width(), plan.storedRect.height());
        jpeg::patchXmpDimensions(out, plan.storedRect.width(), plan.storedRect.height());
        // The thumbnails show the uncropped image (privacy: what was cut away must go):
        // removed, not only unlinked. The orientation stays.
        jpeg::removeExifThumbnail(out);
        jpeg::removeIrbThumbnails(out);
        ok = filesafety::writeNewFile(tmp, out, &error);
        break;
    }
    case CropMethod::ReencodeLossless:
    case CropMethod::ReencodeLossy:
        ok = platform::reencodeCropped(target, tmp, plan.storedRect, method == CropMethod::ReencodeLossless,
                                       &error);
        break;
    case CropMethod::None:
        return {};
    }
    if (!ok) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Der Zuschnitt konnte nicht gesichert werden.") : error;
    }
    // The source must still be the file that was planned (and read).
    if (plan.stamp.isValid() && fileStamp(target) != plan.stamp) {
        QFile::remove(tmp);
        return messageChangedSincePlan();
    }
    if (inPlace || (r.replaceExisting && QFileInfo::exists(destination))) {
        // In place, or an existing file the user agreed to replace ("Speichern unter"); any
        // other existing destination makes moveNoReplace() below fail.
        if (!platform::replaceFile(tmp, destination, inPlace && r.keepModificationDate, &error)) {
            QFile::remove(tmp);
            return error.isEmpty() ? Tr::tr("Die Datei konnte nicht ersetzt werden.") : error;
        }
        return {};
    }
    if (const QString e = fileops::moveNoReplace(tmp, destination); !e.isEmpty()) {
        QFile::remove(tmp);
        return e;
    }
    return {};
}

QString describeCropMethod(CropMethod method, bool snapped) {
    switch (method) {
    case CropMethod::None:
        return Tr::tr("Nichts zuzuschneiden");
    case CropMethod::JpegLossless:
        return snapped ? Tr::tr("Verlustfrei zugeschnitten (an JPEG-Blöcke angepasst)")
                       : Tr::tr("Verlustfrei zugeschnitten");
    case CropMethod::ReencodeLossless:
        return Tr::tr("Zugeschnitten (verlustfrei neu kodiert)");
    case CropMethod::ReencodeLossy:
        return Tr::tr("Zugeschnitten (neu kodiert)");
    }
    return {};
}

std::unique_ptr<FileEdit> makeCropEdit(const CropRequest& request, const CropPlan& plan, CropMethod method,
                                       std::shared_ptr<UndoBackupStore> store) {
    CropRequest r = request;
    r.saveAs.clear(); // undo is for the in-place crop only
    CropPlan p = plan;
    if (p.target.isEmpty())
        p.target = resolvedFilePath(r.path);
    p.destination = p.target;
    const QString target = p.target;
    return std::make_unique<FileEdit>(
        target, [r, p, method] { return executeCropSave(r, p, method); }, std::move(store));
}

} // namespace lf
