// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal writers for GIMP XCF and Photoshop PSB files, used by the decoder tests
// (test_xcf_psb.cpp) to build files with exactly the features under test: versions,
// compressions, precisions, layer modes, offsets, masks and groups (XCF); depths, colour
// modes, compressions and image resources (PSB). Not a general-purpose encoder.

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lf::test {

inline void putBE(QByteArray& out, quint64 v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        out.append(char((v >> (8 * i)) & 0xFF));
}
inline void put32(QByteArray& out, quint32 v) {
    putBE(out, v, 4);
}
inline void put16(QByteArray& out, quint16 v) {
    putBE(out, v, 2);
}
inline void patchBE(QByteArray& out, qsizetype at, quint64 v, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out[at + i] = char((v >> (8 * (bytes - 1 - i))) & 0xFF);
}

// ---- XCF ----------------------------------------------------------------------------------------

struct XcfLayerSpec {
    int width = 1, height = 1;
    int type = 1;      // 0 RGB, 1 RGBA, 2 Gray, 3 GrayA, 4 Indexed, 5 IndexedA
    QByteArray pixels; // interleaved components, big-endian, precision of the image
    int x = 0, y = 0;
    float opacity = 1.0f;
    bool floatOpacity = false; // write PROP_FLOAT_OPACITY instead of PROP_OPACITY
    bool visible = true;
    quint32 mode = 28;        // GIMP_LAYER_MODE_NORMAL
    qint32 compositeMode = 0; // 0 = not written
    bool group = false;
    QVector<quint32> path; // PROP_ITEM_PATH (children of groups)
    QByteArray mask;       // one component per pixel (same precision); empty = no mask
    bool applyMask = true;
};

struct XcfSpec {
    int version = 11;
    int width = 1, height = 1;
    int baseType = 0;             // 0 RGB, 1 Gray, 2 Indexed
    quint32 precision = 150;      // 8-bit perceptual (version >= 4)
    int compression = 1;          // 0 none, 1 RLE, 2 zlib
    QVector<XcfLayerSpec> layers; // top first
    QByteArray colormap;          // r g b triples
    QByteArray icc;
    QVector<QPair<quint32, QByteArray>> extraLayerProps; // added to every layer
};

inline QByteArray xcfRle(const QByteArray& raw, int bpp) {
    QByteArray out;
    const int n = int(raw.size() / bpp);
    for (int plane = 0; plane < bpp; ++plane) {
        QByteArray p(n, '\0');
        for (int i = 0; i < n; ++i)
            p[i] = raw[qsizetype(i) * bpp + plane];
        int i = 0;
        while (i < n) {
            int run = 1;
            while (i + run < n && p[i + run] == p[i] && run < 65535)
                ++run;
            if (run >= 2) {
                if (run <= 127) {
                    out.append(char(run - 1));
                } else {
                    out.append(char(127));
                    put16(out, quint16(run));
                }
                out.append(p[i]);
                i += run;
                continue;
            }
            int lit = 1; // literal until the next run of 2
            while (i + lit < n && lit < 65535 && !(i + lit + 1 < n && p[i + lit] == p[i + lit + 1]))
                ++lit;
            if (lit <= 127) {
                out.append(char(256 - lit));
            } else {
                out.append(char(128));
                put16(out, quint16(lit));
            }
            out.append(p.mid(i, lit));
            i += lit;
        }
    }
    return out;
}

inline QByteArray xcfCompressTile(const QByteArray& raw, int bpp, int compression) {
    if (compression == 1)
        return xcfRle(raw, bpp);
    if (compression == 2) {
        uLongf len = compressBound(uLong(raw.size()));
        QByteArray out(qsizetype(len), '\0');
        compress2(reinterpret_cast<Bytef*>(out.data()), &len, reinterpret_cast<const Bytef*>(raw.constData()),
                  uLong(raw.size()), 6);
        out.resize(qsizetype(len));
        return out;
    }
    return raw;
}

class XcfWriter {
public:
    explicit XcfWriter(const XcfSpec& spec) : s(spec) {}

    QByteArray write() {
        out.clear();
        out.append("gimp xcf ");
        if (s.version == 0)
            out.append("file");
        else
            out.append(QStringLiteral("v%1").arg(s.version, 3, 10, QLatin1Char('0')).toLatin1());
        out.append('\0');
        put32(out, quint32(s.width));
        put32(out, quint32(s.height));
        put32(out, quint32(s.baseType));
        if (s.version >= 4)
            put32(out, s.precision);
        // image properties
        prop(17, QByteArray(1, char(s.compression)));
        if (!s.colormap.isEmpty()) {
            QByteArray cm;
            put32(cm, quint32(s.colormap.size() / 3));
            cm.append(s.colormap);
            prop(1, cm);
        }
        if (!s.icc.isEmpty()) {
            QByteArray p;
            const QByteArray name("icc-profile", 12);
            put32(p, quint32(name.size()));
            p.append(name);
            put32(p, 1); // flags
            put32(p, quint32(s.icc.size()));
            p.append(s.icc);
            prop(21, p);
        }
        put32(out, 0);
        put32(out, 0);
        // layer pointer table (patched below), channel table (empty)
        QVector<qsizetype> layerSlots;
        for (int i = 0; i < s.layers.size(); ++i) {
            layerSlots << out.size();
            ptr(0);
        }
        ptr(0);
        ptr(0); // channels
        for (int i = 0; i < s.layers.size(); ++i) {
            patchPtr(layerSlots[i], out.size());
            writeLayer(s.layers[i]);
        }
        return out;
    }

private:
    int ptrSize() const { return s.version >= 11 ? 8 : 4; }
    void ptr(quint64 v) { putBE(out, v, ptrSize()); }
    void patchPtr(qsizetype at, quint64 v) { patchBE(out, at, v, ptrSize()); }
    void prop(quint32 type, const QByteArray& payload) {
        put32(out, type);
        put32(out, quint32(payload.size()));
        out.append(payload);
    }
    void string(const QByteArray& str) {
        put32(out, quint32(str.size() + 1));
        out.append(str);
        out.append('\0');
    }
    int componentBytes() const {
        if (s.version < 4)
            return 1;
        if (s.version == 4)
            return QVector<int>{1, 2, 4, 2, 4}.value(int(s.precision), 1);
        switch (s.precision) {
        case 200:
        case 250:
            return 2;
        case 300:
        case 350:
            return 4;
        case 400:
        case 450:
            return 2;
        case 500:
        case 550:
            return s.version <= 6 ? 4 : 2;
        case 600:
        case 650:
            return 4;
        case 700:
        case 750:
            return 8;
        default:
            return 1;
        }
    }

    void writeLayer(const XcfLayerSpec& L) {
        static const int comps[] = {3, 4, 1, 2, 1, 2};
        put32(out, quint32(L.width));
        put32(out, quint32(L.height));
        put32(out, quint32(L.type));
        string("layer");
        if (L.floatOpacity) {
            QByteArray f;
            quint32 u;
            std::memcpy(&u, &L.opacity, 4);
            put32(f, u);
            prop(33, f);
        } else {
            QByteArray o;
            put32(o, quint32(std::lround(L.opacity * 255)));
            prop(6, o);
        }
        QByteArray v;
        put32(v, L.visible ? 1 : 0);
        prop(8, v);
        QByteArray m;
        put32(m, L.mode);
        prop(7, m);
        if (L.compositeMode != 0) {
            QByteArray c;
            put32(c, quint32(L.compositeMode));
            prop(35, c);
        }
        QByteArray off;
        put32(off, quint32(L.x));
        put32(off, quint32(L.y));
        prop(15, off);
        if (!L.mask.isEmpty()) {
            QByteArray a;
            put32(a, L.applyMask ? 1 : 0);
            prop(11, a);
        }
        if (L.group)
            prop(29, QByteArray());
        if (!L.path.isEmpty()) {
            QByteArray p;
            for (quint32 i : L.path)
                put32(p, i);
            prop(30, p);
        }
        for (const auto& [type, payload] : s.extraLayerProps)
            prop(type, payload);
        put32(out, 0);
        put32(out, 0);
        const qsizetype hierSlot = out.size();
        ptr(0);
        const qsizetype maskSlot = out.size();
        ptr(0);
        const int bpp = comps[L.type] * componentBytes();
        QByteArray pixels = L.pixels;
        if (pixels.isEmpty())
            pixels = QByteArray(qsizetype(L.width) * L.height * bpp, '\0');
        patchPtr(hierSlot, out.size());
        writeHierarchy(L.width, L.height, bpp, pixels);
        if (!L.mask.isEmpty()) {
            patchPtr(maskSlot, out.size());
            put32(out, quint32(L.width));
            put32(out, quint32(L.height));
            string("mask");
            put32(out, 0);
            put32(out, 0);
            const qsizetype h = out.size();
            ptr(0);
            patchPtr(h, out.size());
            writeHierarchy(L.width, L.height, componentBytes(), L.mask);
        }
    }

    void writeHierarchy(int w, int h, int bpp, const QByteArray& pixels) {
        put32(out, quint32(w));
        put32(out, quint32(h));
        put32(out, quint32(bpp));
        const qsizetype levelSlot = out.size();
        ptr(0);
        ptr(0); // no further levels
        patchPtr(levelSlot, out.size());
        put32(out, quint32(w));
        put32(out, quint32(h));
        const int tx = (w + 63) / 64, ty = (h + 63) / 64;
        QVector<qsizetype> tileSlots;
        for (int i = 0; i < tx * ty; ++i) {
            tileSlots << out.size();
            ptr(0);
        }
        ptr(0);
        for (int j = 0; j < ty; ++j) {
            for (int i = 0; i < tx; ++i) {
                const int tw = std::min(64, w - i * 64), th = std::min(64, h - j * 64);
                QByteArray raw;
                for (int y = 0; y < th; ++y)
                    raw.append(pixels.mid((qsizetype(j * 64 + y) * w + i * 64) * bpp, qsizetype(tw) * bpp));
                patchPtr(tileSlots[j * tx + i], out.size());
                out.append(xcfCompressTile(raw, bpp, s.compression));
            }
        }
    }

    const XcfSpec& s;
    QByteArray out;
};

inline QByteArray writeXcf(const XcfSpec& spec) {
    return XcfWriter(spec).write();
}

// Solid-colour 8-bit RGBA pixels.
inline QByteArray solidRgba8(int w, int h, int r, int g, int b, int a = 255) {
    QByteArray px;
    px.reserve(qsizetype(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        px.append(char(r));
        px.append(char(g));
        px.append(char(b));
        px.append(char(a));
    }
    return px;
}

// Quadrants red/green/blue/white (RGBA 8 bit, opaque).
inline QByteArray quadRgba8(int w, int h) {
    QByteArray px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool top = y < h / 2, left = x < w / 2;
            const int r = (top && left) || (!top && !left) ? 255 : 0;
            const int g = (top && !left) || (!top && !left) ? 255 : 0;
            const int b = (!top) ? 255 : 0;
            px.append(char(r));
            px.append(char(g));
            px.append(char(b));
            px.append(char(255));
        }
    return px;
}

// ---- PSB ----------------------------------------------------------------------------------------

struct PsbSpec {
    int version = 2;
    int width = 1, height = 1;
    int depth = 8;
    int mode = 3;                 // 3 RGB, 1 gray, 2 indexed, 4 CMYK, 9 Lab, 0 bitmap
    int compression = 1;          // 0 raw, 1 RLE, 2 ZIP, 3 ZIP with prediction
    QVector<QByteArray> channels; // planar rows, big-endian samples, rowBytes each
    QByteArray colorModeData;
    QByteArray icc;
    bool versionInfo = false; // write resource 0x0421 with hasRealMergedData
    bool realMergedData = true;
};

inline QByteArray packBitsRow(const QByteArray& row) {
    QByteArray out;
    int i = 0;
    const int n = int(row.size());
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i])
            ++run;
        if (run >= 2) {
            out.append(char(1 - run));
            out.append(row[i]);
            i += run;
            continue;
        }
        int lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && row[i + lit] == row[i + lit + 1]))
            ++lit;
        out.append(char(lit - 1));
        out.append(row.mid(i, lit));
        i += lit;
    }
    return out;
}

inline QByteArray writePsb(const PsbSpec& s) {
    QByteArray out("8BPS");
    put16(out, quint16(s.version));
    out.append(QByteArray(6, '\0'));
    put16(out, quint16(s.channels.size()));
    put32(out, quint32(s.height));
    put32(out, quint32(s.width));
    put16(out, quint16(s.depth));
    put16(out, quint16(s.mode));
    put32(out, quint32(s.colorModeData.size()));
    out.append(s.colorModeData);
    QByteArray res;
    auto resource = [&res](quint16 id, const QByteArray& data) {
        res.append("8BIM");
        put16(res, id);
        res.append(QByteArray(2, '\0')); // empty pascal name, padded
        put32(res, quint32(data.size()));
        res.append(data);
        if (data.size() % 2)
            res.append('\0');
    };
    if (!s.icc.isEmpty())
        resource(0x040F, s.icc);
    if (s.versionInfo) {
        QByteArray vi;
        put32(vi, 1);
        vi.append(char(s.realMergedData ? 1 : 0));
        resource(0x0421, vi);
    }
    put32(out, quint32(res.size()));
    out.append(res);
    if (s.version == 2)
        putBE(out, 0, 8); // layer and mask information: empty
    else
        put32(out, 0);
    put16(out, quint16(s.compression));
    const qsizetype rowBytes = s.depth == 1 ? (s.width + 7) / 8 : qsizetype(s.width) * (s.depth / 8);
    QByteArray all;
    for (const QByteArray& c : s.channels)
        all.append(c);
    switch (s.compression) {
    case 0:
        out.append(all);
        break;
    case 1: {
        QVector<QByteArray> packed;
        for (qsizetype r = 0; r < all.size() / rowBytes; ++r)
            packed << packBitsRow(all.mid(r * rowBytes, rowBytes));
        for (const QByteArray& p : packed)
            putBE(out, quint64(p.size()), s.version == 2 ? 4 : 2);
        for (const QByteArray& p : packed)
            out.append(p);
        break;
    }
    default: {
        QByteArray data = all;
        if (s.compression == 3) { // prediction (delta per row)
            for (qsizetype r = 0; r < data.size() / rowBytes; ++r) {
                auto* row = reinterpret_cast<uchar*>(data.data() + r * rowBytes);
                if (s.depth == 8) {
                    for (qsizetype x = rowBytes - 1; x > 0; --x)
                        row[x] = uchar(row[x] - row[x - 1]);
                } else if (s.depth == 16) {
                    for (qsizetype x = rowBytes / 2 - 1; x > 0; --x) {
                        const quint16 v = quint16((row[2 * x] << 8) | row[2 * x + 1]);
                        const quint16 p = quint16((row[2 * x - 2] << 8) | row[2 * x - 1]);
                        const quint16 d = quint16(v - p);
                        row[2 * x] = uchar(d >> 8);
                        row[2 * x + 1] = uchar(d);
                    }
                } else if (s.depth == 32) {
                    const qsizetype w = rowBytes / 4;
                    QByteArray planar(rowBytes, '\0');
                    for (qsizetype x = 0; x < w; ++x)
                        for (int b = 0; b < 4; ++b)
                            planar[b * w + x] = char(row[4 * x + b]);
                    std::memcpy(row, planar.constData(), size_t(rowBytes));
                    for (qsizetype x = rowBytes - 1; x > 0; --x)
                        row[x] = uchar(row[x] - row[x - 1]);
                }
            }
        }
        uLongf len = compressBound(uLong(data.size()));
        QByteArray z(qsizetype(len), '\0');
        compress2(reinterpret_cast<Bytef*>(z.data()), &len, reinterpret_cast<const Bytef*>(data.constData()),
                  uLong(data.size()), 6);
        z.resize(qsizetype(len));
        out.append(z);
        break;
    }
    }
    return out;
}

} // namespace lf::test
