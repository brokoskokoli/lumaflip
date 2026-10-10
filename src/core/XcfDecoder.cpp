// SPDX-License-Identifier: GPL-3.0-or-later
// GIMP XCF decoder (see XcfDecoder.h). Format reference: GIMP's devel-docs/xcf.txt.
#include "XcfDecoder.h"

#include "OwnDecoderUtil.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QVector>
#include <QtNumeric>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace lf {

namespace {

Q_LOGGING_CATEGORY(lcXcf, "lf.xcf", QtInfoMsg)

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::XcfDecoder)
};

using decutil::Reader;

constexpr qint64 kMaxFileBytes = qint64(8) << 30;
constexpr int kTile = 64;
constexpr int kMaxProperties = 100000;

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

QString damaged() {
    return Tr::tr("Die XCF-Datei ist beschädigt oder unvollständig.");
}

// ---- properties and modes -------------------------------------------------------------------

enum Prop : quint32 {
    PropEnd = 0,
    PropColormap = 1,
    PropOpacity = 6,
    PropMode = 7,
    PropVisible = 8,
    PropApplyMask = 11,
    PropOffsets = 15,
    PropCompression = 17,
    PropParasites = 21,
    PropGroupItem = 29,
    PropItemPath = 30,
    PropFloatOpacity = 33,
    PropCompositeMode = 35,
    PropFirstEffectProp = 44, // GIMP 3 filters (non-destructive effects)
};

enum Mode : quint32 {
    NormalLegacy = 0,
    Dissolve = 1,
    BehindLegacy = 2,
    MultiplyLegacy = 3,
    ScreenLegacy = 4,
    OverlayLegacy = 5,
    DifferenceLegacy = 6,
    AdditionLegacy = 7,
    SubtractLegacy = 8,
    DarkenLegacy = 9,
    LightenLegacy = 10,
    DivideLegacy = 15,
    DodgeLegacy = 16,
    BurnLegacy = 17,
    HardlightLegacy = 18,
    SoftlightLegacy = 19,
    GrainExtractLegacy = 20,
    GrainMergeLegacy = 21,
    LastLegacy = 22, // colour erase (legacy)
    Overlay = 23,
    Normal = 28,
    Behind = 29,
    Multiply = 30,
    Screen = 31,
    Difference = 32,
    Addition = 33,
    Subtract = 34,
    Darken = 35,
    Lighten = 36,
    Divide = 41,
    Dodge = 42,
    Burn = 43,
    Hardlight = 44,
    Softlight = 45,
    GrainExtract = 46,
    GrainMerge = 47,
    VividLight = 48,
    PinLight = 49,
    LinearLight = 50,
    HardMix = 51,
    Exclusion = 52,
    LinearBurn = 53,
    PassThrough = 61,
};

enum class Op {
    Normal,
    Dissolve,
    Behind,
    Multiply,
    Screen,
    Overlay,
    Difference,
    Addition,
    Subtract,
    Darken,
    Lighten,
    Divide,
    Dodge,
    Burn,
    HardLight,
    SoftLight,
    GrainExtract,
    GrainMerge,
    VividLight,
    PinLight,
    LinearLight,
    HardMix,
    Exclusion,
    LinearBurn,
    Unsupported,
};

Op opFor(quint32 mode) {
    switch (mode) {
    case NormalLegacy:
    case Normal:
    case PassThrough: // a group mode; as a layer mode it is normal
        return Op::Normal;
    case Dissolve:
        return Op::Dissolve;
    case BehindLegacy:
    case Behind:
        return Op::Behind;
    case MultiplyLegacy:
    case Multiply:
        return Op::Multiply;
    case ScreenLegacy:
    case Screen:
        return Op::Screen;
    case OverlayLegacy:
    case Overlay:
        return Op::Overlay;
    case DifferenceLegacy:
    case Difference:
        return Op::Difference;
    case AdditionLegacy:
    case Addition:
        return Op::Addition;
    case SubtractLegacy:
    case Subtract:
        return Op::Subtract;
    case DarkenLegacy:
    case Darken:
        return Op::Darken;
    case LightenLegacy:
    case Lighten:
        return Op::Lighten;
    case DivideLegacy:
    case Divide:
        return Op::Divide;
    case DodgeLegacy:
    case Dodge:
        return Op::Dodge;
    case BurnLegacy:
    case Burn:
        return Op::Burn;
    case HardlightLegacy:
    case Hardlight:
        return Op::HardLight;
    case SoftlightLegacy:
    case Softlight:
        return Op::SoftLight;
    case GrainExtractLegacy:
    case GrainExtract:
        return Op::GrainExtract;
    case GrainMergeLegacy:
    case GrainMerge:
        return Op::GrainMerge;
    case VividLight:
        return Op::VividLight;
    case PinLight:
        return Op::PinLight;
    case LinearLight:
        return Op::LinearLight;
    case HardMix:
        return Op::HardMix;
    case Exclusion:
        return Op::Exclusion;
    case LinearBurn:
        return Op::LinearBurn;
    default:
        return Op::Unsupported;
    }
}

bool isLegacyMode(quint32 mode) {
    return mode <= LastLegacy;
}

float clamp01(float v) {
    return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
}

float burn(float cb, float cs) {
    return cs <= 0.0f ? (cb >= 1.0f ? 1.0f : 0.0f) : 1.0f - (1.0f - cb) / cs;
}
float dodge(float cb, float cs) {
    return cs >= 1.0f ? (cb > 0.0f ? 1.0f : 0.0f) : cb / (1.0f - cs);
}

// Blend of one colour channel: backdrop cb, layer cs (perceptual, 0..1).
float blend(Op op, float cb, float cs) {
    switch (op) {
    case Op::Multiply:
        return cb * cs;
    case Op::Screen:
        return 1.0f - (1.0f - cb) * (1.0f - cs);
    case Op::Overlay:
        return cb < 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case Op::Difference:
        return std::fabs(cb - cs);
    case Op::Addition:
        return clamp01(cb + cs);
    case Op::Subtract:
        return clamp01(cb - cs);
    case Op::Darken:
        return std::min(cb, cs);
    case Op::Lighten:
        return std::max(cb, cs);
    case Op::Divide:
        return clamp01(cs <= 0.0f ? (cb > 0.0f ? 1.0f : 0.0f) : cb / cs);
    case Op::Dodge:
        return clamp01(dodge(cb, cs));
    case Op::Burn:
        return clamp01(burn(cb, cs));
    case Op::HardLight:
        return cs < 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case Op::SoftLight: // GIMP's soft light
        return clamp01((1.0f - cb) * cb * cs + cb * (1.0f - (1.0f - cb) * (1.0f - cs)));
    case Op::GrainExtract:
        return clamp01(cb - cs + 0.5f);
    case Op::GrainMerge:
        return clamp01(cb + cs - 0.5f);
    case Op::VividLight:
        return clamp01(cs <= 0.5f ? burn(cb, 2.0f * cs) : dodge(cb, 2.0f * cs - 1.0f));
    case Op::PinLight:
        return cs > 0.5f ? std::max(cb, 2.0f * cs - 1.0f) : std::min(cb, 2.0f * cs);
    case Op::LinearLight:
        return clamp01(cb + 2.0f * cs - 1.0f);
    case Op::HardMix:
        return cb + cs >= 1.0f ? 1.0f : 0.0f;
    case Op::Exclusion:
        return clamp01(cb + cs - 2.0f * cb * cs);
    case Op::LinearBurn:
        return clamp01(cb + cs - 1.0f);
    case Op::Normal:
    case Op::Dissolve:
    case Op::Behind:
    case Op::Unsupported:
        break;
    }
    return cs;
}

// ---- precision ----------------------------------------------------------------------------------

enum class Kind { Int, Half, Float, Double };

struct Precision {
    int bytes = 1; // per component
    Kind kind = Kind::Int;
    bool linear = false;
    int bits() const { return bytes * 8; }
    bool isFloat() const { return kind != Kind::Int; }
};

std::optional<Precision> precisionFor(int version, quint32 code) {
    if (version < 4)
        return Precision{1, Kind::Int, false};
    if (version == 4) {
        switch (code) {
        case 0:
            return Precision{1, Kind::Int, false};
        case 1:
            return Precision{2, Kind::Int, false};
        case 2:
            return Precision{4, Kind::Int, true};
        case 3:
            return Precision{2, Kind::Half, true};
        case 4:
            return Precision{4, Kind::Float, true};
        default:
            return std::nullopt;
        }
    }
    switch (code) {
    case 100:
        return Precision{1, Kind::Int, true};
    case 150:
        return Precision{1, Kind::Int, false};
    case 200:
        return Precision{2, Kind::Int, true};
    case 250:
        return Precision{2, Kind::Int, false};
    case 300:
        return Precision{4, Kind::Int, true};
    case 350:
        return Precision{4, Kind::Int, false};
    default:
        break;
    }
    if (version <= 6) {
        switch (code) {
        case 400:
            return Precision{2, Kind::Half, true};
        case 450:
            return Precision{2, Kind::Half, false};
        case 500:
            return Precision{4, Kind::Float, true};
        case 550:
            return Precision{4, Kind::Float, false};
        default:
            return std::nullopt;
        }
    }
    switch (code) {
    case 500:
        return Precision{2, Kind::Half, true};
    case 550:
        return Precision{2, Kind::Half, false};
    case 600:
        return Precision{4, Kind::Float, true};
    case 650:
        return Precision{4, Kind::Float, false};
    case 700:
        return Precision{8, Kind::Double, true};
    case 750:
        return Precision{8, Kind::Double, false};
    default:
        return std::nullopt;
    }
}

float readComponent(const uchar* p, const Precision& pr) {
    float v = 0.0f;
    switch (pr.kind) {
    case Kind::Int:
        switch (pr.bytes) {
        case 1:
            return p[0] / 255.0f;
        case 2:
            return float((p[0] << 8) | p[1]) / 65535.0f;
        default: {
            const quint32 u =
                (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
            return float(double(u) / 4294967295.0);
        }
        }
    case Kind::Half: {
        const quint16 u = quint16((p[0] << 8) | p[1]);
        qfloat16 h;
        std::memcpy(&h, &u, sizeof u);
        v = float(h);
        break;
    }
    case Kind::Float: {
        const quint32 u =
            (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
        std::memcpy(&v, &u, sizeof v);
        break;
    }
    case Kind::Double: {
        quint64 u = 0;
        for (int i = 0; i < 8; ++i)
            u = (u << 8) | p[i];
        double d;
        std::memcpy(&d, &u, sizeof d);
        v = float(d);
        break;
    }
    }
    return std::isfinite(v) ? v : 0.0f;
}

// ---- header ------------------------------------------------------------------------------------

struct Header {
    int version = 0;
    quint32 width = 0, height = 0, baseType = 0;
    Precision precision;
    qint64 propsOffset = 0;
};

std::optional<int> parseVersion(const QByteArray& h) {
    if (h.size() < 14 || !h.startsWith("gimp xcf ") || h.at(13) != '\0')
        return std::nullopt;
    const QByteArray v = h.mid(9, 4);
    if (v == "file")
        return 0;
    if (v.at(0) != 'v' || !std::isdigit(uchar(v.at(1))) || !std::isdigit(uchar(v.at(2))) ||
        !std::isdigit(uchar(v.at(3))))
        return std::nullopt;
    return (v.at(1) - '0') * 100 + (v.at(2) - '0') * 10 + (v.at(3) - '0');
}

std::optional<Header> parseHeader(const QByteArray& data, QString* error) {
    const auto version = parseVersion(data);
    if (!version) {
        setError(error, Tr::tr("Keine XCF-Datei."));
        return std::nullopt;
    }
    Reader r(data);
    r.seek(14);
    Header h;
    h.version = *version;
    h.width = r.u32();
    h.height = r.u32();
    h.baseType = r.u32();
    const quint32 code = h.version >= 4 ? r.u32() : 150;
    if (!r.ok()) {
        setError(error, damaged());
        return std::nullopt;
    }
    if (h.width == 0 || h.height == 0 || h.width > quint32(xcf::kMaxEdge) ||
        h.height > quint32(xcf::kMaxEdge)) {
        setError(error,
                 Tr::tr("Die Bildgröße %1 × %2 ist ungültig oder zu groß.").arg(h.width).arg(h.height));
        return std::nullopt;
    }
    if (h.baseType > 2) {
        setError(error, damaged());
        return std::nullopt;
    }
    const auto p = precisionFor(h.version, code);
    if (!p || (h.baseType == 2 && p->bytes != 1)) {
        setError(error, Tr::tr("Die Farbtiefe der XCF-Datei wird nicht unterstützt."));
        return std::nullopt;
    }
    h.precision = *p;
    h.propsOffset = r.pos();
    return h;
}

QString colorModelName(quint32 baseType) {
    switch (baseType) {
    case 1:
        return QStringLiteral("Gray");
    case 2:
        return QStringLiteral("Indexed");
    default:
        return QStringLiteral("RGB");
    }
}

// ---- the decoder state ----------------------------------------------------------------------------

struct Layer {
    quint32 width = 0, height = 0, type = 0;
    bool visible = true;
    float opacity = 1.0f;
    quint32 mode = NormalLegacy;
    qint32 compositeMode = 0; // 0 = default of the mode
    qint32 x = 0, y = 0;
    bool applyMask = true;
    bool group = false;
    QVector<quint32> path;
    qint64 hierarchy = 0;
    qint64 mask = 0;
    QVector<int> children;
};

class Compositor {
public:
    Compositor(const QByteArray& data, const Header& h, int maxEdge) : m_data(data), m_r(data), m_h(h) {
        m_full = QSize(int(h.width), int(h.height));
        m_k = decutil::subsampleFactor(m_full, maxEdge, xcf::kMaxCanvasPixels);
        m_canvasSize = QSize((m_full.width() + m_k - 1) / m_k, (m_full.height() + m_k - 1) / m_k);
    }

    std::optional<xcf::Result> run(int maxEdge, QString* error);

private:
    bool fail(QString* error, const QString& why) {
        qCDebug(lcXcf) << "damaged:" << why;
        setError(error, damaged());
        return false;
    }
    void approximate(const QString& note) {
        if (!m_result.notes.contains(note))
            m_result.notes << note;
        m_result.approximate = true;
    }
    qint64 readPointer(Reader& r) const { return m_h.version >= 11 ? qint64(r.u64()) : qint64(r.u32()); }
    bool readString(Reader& r) {
        const quint32 len = r.u32();
        return r.skip(len);
    }
    bool readImageProperties(QString* error);
    void readParasites(qint64 start, qint64 end);
    bool readLayer(qint64 offset, Layer& layer, QString* error);
    bool buildTree(QVector<int>& roots);
    bool compositeList(QImage& canvas, const QVector<int>& items, int depth, QString* error);
    bool compositeItem(QImage& canvas, int index, int depth, QString* error);
    QRect sampleRect(qint32 x, qint32 y, quint32 w, quint32 h) const;
    // Reads the first level of a hierarchy and hands every sampled pixel to `put`.
    template <typename Put>
    bool readHierarchy(qint64 offset, quint32 width, quint32 height, int bpp, qint32 ox, qint32 oy,
                       const QRect& rect, Put&& put, QString* error);
    bool decodeTile(qint64 offset, qint64 limit, int bpp, int pixels, QByteArray& out) const;
    bool loadLayerPixels(const Layer& L, const QRect& rect, QImage& out, QString* error);
    bool loadMask(const Layer& L, const QRect& rect, QVector<float>& out, QString* error);
    void composite(QImage& canvas, const QImage& src, QPoint at, const QVector<float>* mask, quint32 mode,
                   float opacity, qint32 compositeMode);

    const QByteArray& m_data;
    Reader m_r;
    Header m_h;
    QSize m_full;
    QSize m_canvasSize;
    int m_k = 1;
    int m_compression = 0;
    QVector<float> m_colormap; // r, g, b (0..1) per entry
    QColorSpace m_iccSpace;
    QVector<Layer> m_layers;
    xcf::Result m_result;
};

bool Compositor::readImageProperties(QString* error) {
    Reader& r = m_r;
    r.seek(m_h.propsOffset);
    for (int i = 0; i < kMaxProperties; ++i) {
        const quint32 type = r.u32();
        const quint32 len = r.u32();
        if (!r.ok())
            return fail(error, QStringLiteral("image properties"));
        if (type == PropEnd)
            return true;
        const qint64 start = r.pos();
        qint64 end = start + qint64(len);
        switch (type) {
        case PropCompression:
            m_compression = r.u8();
            break;
        case PropColormap: {
            const quint32 n = r.u32();
            if (n > 256)
                return fail(error, QStringLiteral("colormap size"));
            m_colormap.resize(int(n) * 3);
            for (int j = 0; j < int(n) * 3; ++j)
                m_colormap[j] = r.u8() / 255.0f;
            // XCF v0 files may carry a wrong length for this property.
            if (m_h.version == 0)
                end = r.pos();
            break;
        }
        case PropParasites:
            readParasites(start, end);
            break;
        default:
            break;
        }
        if (!r.ok() || !r.seek(end))
            return fail(error, QStringLiteral("image property %1").arg(type));
    }
    return fail(error, QStringLiteral("too many image properties"));
}

void Compositor::readParasites(qint64 start, qint64 end) {
    if (end > m_r.size() || end < start)
        return;
    Reader r(m_r.data() + start, end - start);
    while (r.ok() && r.remaining() > 0) {
        const quint32 nameLen = r.u32();
        const QByteArray name = r.bytes(nameLen);
        r.u32(); // flags
        const quint32 size = r.u32();
        const QByteArray payload = r.bytes(size);
        if (!r.ok())
            return;
        if ((name == QByteArray("icc-profile", 12) || name == "icc-profile") && m_h.baseType != 1) {
            const QColorSpace cs = QColorSpace::fromIccProfile(payload);
            if (cs.isValid() && cs.colorModel() == QColorSpace::ColorModel::Rgb)
                m_iccSpace = cs;
        }
    }
}

bool Compositor::readLayer(qint64 offset, Layer& L, QString* error) {
    Reader& r = m_r;
    if (!r.seek(offset))
        return fail(error, QStringLiteral("layer offset"));
    L.width = r.u32();
    L.height = r.u32();
    L.type = r.u32();
    if (!readString(r) || !r.ok())
        return fail(error, QStringLiteral("layer header"));
    if (L.width == 0 || L.height == 0 || L.width > quint32(xcf::kMaxEdge) ||
        L.height > quint32(xcf::kMaxEdge) || L.type > 5)
        return fail(error, QStringLiteral("layer size/type"));
    for (int i = 0;; ++i) {
        if (i >= kMaxProperties)
            return fail(error, QStringLiteral("too many layer properties"));
        const quint32 type = r.u32();
        const quint32 len = r.u32();
        if (!r.ok())
            return fail(error, QStringLiteral("layer properties"));
        if (type == PropEnd)
            break;
        const qint64 end = r.pos() + qint64(len);
        switch (type) {
        case PropOpacity:
            L.opacity = std::min(r.u32(), 255u) / 255.0f;
            break;
        case PropFloatOpacity: {
            const float f = r.f32();
            L.opacity = std::isfinite(f) ? clamp01(f) : 1.0f;
            break;
        }
        case PropVisible:
            L.visible = r.u32() != 0;
            break;
        case PropMode:
            L.mode = r.u32();
            break;
        case PropCompositeMode:
            L.compositeMode = r.i32();
            break;
        case PropOffsets:
            L.x = r.i32();
            L.y = r.i32();
            break;
        case PropApplyMask:
            L.applyMask = r.u32() != 0;
            break;
        case PropGroupItem:
            L.group = true;
            break;
        case PropItemPath:
            for (quint32 j = 0; j + 4 <= len && j < 4 * 64; j += 4)
                L.path << r.u32();
            break;
        default:
            if (type >= PropFirstEffectProp && type < 1000)
                approximate(QStringLiteral("layer effects / property %1 ignored").arg(type));
            break;
        }
        if (!r.ok() || !r.seek(end))
            return fail(error, QStringLiteral("layer property %1").arg(type));
    }
    L.hierarchy = readPointer(r);
    L.mask = readPointer(r);
    if (!r.ok())
        return fail(error, QStringLiteral("layer pointers"));
    if (!L.group && opFor(L.mode) == Op::Unsupported)
        approximate(QStringLiteral("layer mode %1 rendered as normal").arg(L.mode));
    return true;
}

bool Compositor::buildTree(QVector<int>& roots) {
    QHash<QVector<quint32>, int> groups;
    for (int i = 0; i < m_layers.size(); ++i) {
        Layer& L = m_layers[i];
        if (L.path.size() <= 1) {
            roots << i;
        } else {
            const int parent = groups.value(L.path.mid(0, L.path.size() - 1), -1);
            if (parent < 0) {
                approximate(QStringLiteral("layer with unknown parent group"));
                roots << i;
            } else {
                m_layers[parent].children << i;
            }
        }
        if (L.group && !L.path.isEmpty())
            groups.insert(L.path, i);
        else if (L.group)
            groups.insert({quint32(roots.size() - 1)}, i);
    }
    return true;
}

QRect Compositor::sampleRect(qint32 x, qint32 y, quint32 w, quint32 h) const {
    auto range = [this](qint64 o, qint64 len, int canvas) {
        const qint64 lo = std::max<qint64>(o, 0);
        const qint64 hi = o + len; // exclusive (full resolution)
        const qint64 a = std::min<qint64>((lo + m_k - 1) / m_k, canvas);
        const qint64 b = std::min<qint64>(canvas, hi <= 0 ? 0 : (hi + m_k - 1) / m_k);
        return std::pair<int, int>(int(a), int(std::max(a, b))); // empty if outside
    };
    const auto [x0, x1] = range(x, w, m_canvasSize.width());
    const auto [y0, y1] = range(y, h, m_canvasSize.height());
    return QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

bool Compositor::decodeTile(qint64 offset, qint64 limit, int bpp, int pixels, QByteArray& out) const {
    const qint64 expected = qint64(bpp) * pixels;
    out.resize(expected);
    if (offset < 0 || offset >= m_r.size())
        return false;
    limit = std::min(limit, m_r.size() - offset);
    const uchar* src = m_r.data() + offset;
    auto* dst = reinterpret_cast<uchar*>(out.data());
    switch (m_compression) {
    case 0:
        if (limit < expected)
            return false;
        std::memcpy(dst, src, size_t(expected));
        return true;
    case 1: { // RLE per byte plane
        qint64 s = 0;
        for (int plane = 0; plane < bpp; ++plane) {
            int i = 0;
            while (i < pixels) {
                if (s >= limit)
                    return false;
                const int n = src[s++];
                int count;
                if (n >= 128) { // literal run
                    if (n == 128) {
                        if (s + 2 > limit)
                            return false;
                        count = (src[s] << 8) | src[s + 1];
                        s += 2;
                    } else {
                        count = 256 - n;
                    }
                    if (count > pixels - i || s + count > limit)
                        return false;
                    for (int k = 0; k < count; ++k)
                        dst[qint64(i + k) * bpp + plane] = src[s + k];
                    s += count;
                } else { // repeated byte
                    if (n == 127) {
                        if (s + 2 > limit)
                            return false;
                        count = (src[s] << 8) | src[s + 1];
                        s += 2;
                    } else {
                        count = n + 1;
                    }
                    if (count > pixels - i || s >= limit)
                        return false;
                    const uchar v = src[s++];
                    for (int k = 0; k < count; ++k)
                        dst[qint64(i + k) * bpp + plane] = v;
                }
                i += count;
            }
        }
        return true;
    }
    case 2: { // zlib
        uLongf destLen = uLongf(expected);
        uLong srcLen = uLong(limit);
        const int rc = uncompress2(dst, &destLen, src, &srcLen);
        return (rc == Z_OK || rc == Z_BUF_ERROR) && qint64(destLen) == expected;
    }
    default:
        return false;
    }
}

template <typename Put>
bool Compositor::readHierarchy(qint64 offset, quint32 width, quint32 height, int bpp, qint32 ox, qint32 oy,
                               const QRect& rect, Put&& put, QString* error) {
    Reader r(m_data);
    if (!r.seek(offset))
        return fail(error, QStringLiteral("hierarchy offset"));
    const quint32 hw = r.u32(), hh = r.u32(), hbpp = r.u32();
    const qint64 level = readPointer(r);
    if (!r.ok() || hw != width || hh != height || int(hbpp) != bpp)
        return fail(error, QStringLiteral("hierarchy header"));
    if (!r.seek(level))
        return fail(error, QStringLiteral("level offset"));
    const quint32 lw = r.u32(), lh = r.u32();
    if (!r.ok() || lw != width || lh != height)
        return fail(error, QStringLiteral("level header"));
    const int tilesX = int((lw + kTile - 1) / kTile), tilesY = int((lh + kTile - 1) / kTile);
    const qint64 tileCount = qint64(tilesX) * tilesY;
    if (tileCount > r.remaining() / (m_h.version >= 11 ? 8 : 4))
        return fail(error, QStringLiteral("tile table"));
    QVector<qint64> tiles(tileCount + 1);
    for (qint64 i = 0; i < tileCount; ++i) {
        tiles[i] = readPointer(r);
        if (!r.ok() || tiles[i] <= 0 || tiles[i] >= r.size())
            return fail(error, QStringLiteral("tile pointer"));
    }
    tiles[tileCount] = readPointer(r); // 0 terminates the list; ignored if missing
    QByteArray buf;
    const qint64 x0 = qint64(rect.left()) * m_k, x1 = qint64(rect.right()) * m_k; // full-res canvas
    const qint64 y0 = qint64(rect.top()) * m_k, y1 = qint64(rect.bottom()) * m_k;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int tw = std::min<int>(kTile, int(lw) - tx * kTile);
            const int th = std::min<int>(kTile, int(lh) - ty * kTile);
            // Skip tiles without a sampled canvas pixel.
            const qint64 cx0 = qint64(ox) + tx * kTile, cy0 = qint64(oy) + ty * kTile;
            if (cx0 + tw - 1 < x0 || cx0 > x1 || cy0 + th - 1 < y0 || cy0 > y1)
                continue;
            const qint64 index = qint64(ty) * tilesX + tx;
            const qint64 off = tiles[index];
            const qint64 next = tiles[index + 1];
            const qint64 limit = next > off ? next - off : m_r.size() - off;
            if (!decodeTile(off, limit, bpp, tw * th, buf))
                return fail(error, QStringLiteral("tile data"));
            const auto* px = reinterpret_cast<const uchar*>(buf.constData());
            for (int y = 0; y < th; ++y) {
                const qint64 cy = cy0 + y;
                if (cy < y0 || cy > y1 || cy % m_k != 0)
                    continue;
                const int sy = int(cy / m_k) - rect.top();
                for (int x = 0; x < tw; ++x) {
                    const qint64 cx = cx0 + x;
                    if (cx < x0 || cx > x1 || cx % m_k != 0)
                        continue;
                    put(int(cx / m_k) - rect.left(), sy, px + (qint64(y) * tw + x) * bpp);
                }
            }
        }
    }
    return true;
}

bool Compositor::loadLayerPixels(const Layer& L, const QRect& rect, QImage& out, QString* error) {
    static constexpr int kComponents[] = {3, 4, 1, 2, 1, 2};
    const int comps = kComponents[L.type];
    const Precision& pr = m_h.precision;
    if ((L.type >= 4) != (m_h.baseType == 2))
        approximate(QStringLiteral("layer type does not match the image type"));
    if (L.type >= 4 && pr.bytes != 1)
        return fail(error, QStringLiteral("indexed layer with deep precision"));
    out = QImage(rect.size(), QImage::Format_RGBA32FPx4);
    if (out.isNull()) {
        setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
        return false;
    }
    out.fill(Qt::transparent);
    const int bpp = comps * pr.bytes;
    const int ncolors = int(m_colormap.size() / 3);
    auto put = [&](int x, int y, const uchar* p) {
        float c[4] = {0, 0, 0, 1};
        switch (L.type) {
        case 0:
        case 1:
            for (int i = 0; i < comps; ++i)
                c[i] = readComponent(p + i * pr.bytes, pr);
            break;
        case 2:
        case 3:
            c[0] = c[1] = c[2] = readComponent(p, pr);
            if (comps == 2)
                c[3] = readComponent(p + pr.bytes, pr);
            break;
        default: { // indexed
            const int idx = p[0];
            if (idx < ncolors) {
                c[0] = m_colormap[idx * 3];
                c[1] = m_colormap[idx * 3 + 1];
                c[2] = m_colormap[idx * 3 + 2];
            }
            if (comps == 2)
                c[3] = p[1] / 255.0f;
            break;
        }
        }
        if (pr.linear) {
            for (int i = 0; i < 3; ++i)
                c[i] = decutil::linearToSrgb(c[i]);
        }
        float* d = reinterpret_cast<float*>(out.scanLine(y)) + x * 4;
        d[0] = clamp01(c[0]);
        d[1] = clamp01(c[1]);
        d[2] = clamp01(c[2]);
        d[3] = clamp01(c[3]);
    };
    return readHierarchy(L.hierarchy, L.width, L.height, bpp, L.x, L.y, rect, put, error);
}

bool Compositor::loadMask(const Layer& L, const QRect& rect, QVector<float>& out, QString* error) {
    Reader r(m_data);
    if (!r.seek(L.mask))
        return fail(error, QStringLiteral("mask offset"));
    const quint32 w = r.u32(), h = r.u32();
    if (!readString(r))
        return fail(error, QStringLiteral("mask header"));
    for (int i = 0;; ++i) {
        const quint32 type = r.u32();
        const quint32 len = r.u32();
        if (!r.ok() || i >= kMaxProperties)
            return fail(error, QStringLiteral("mask properties"));
        if (type == PropEnd)
            break;
        if (!r.skip(len))
            return fail(error, QStringLiteral("mask property"));
    }
    const qint64 hierarchy = readPointer(r);
    if (!r.ok())
        return fail(error, QStringLiteral("mask pointer"));
    if (w != L.width || h != L.height) {
        approximate(QStringLiteral("mask size differs from its layer; mask ignored"));
        out.clear();
        return true;
    }
    out = QVector<float>(qsizetype(rect.width()) * rect.height(), 0.0f);
    const Precision& pr = m_h.precision;
    const int stride = rect.width();
    auto put = [&](int x, int y, const uchar* p) {
        out[qsizetype(y) * stride + x] = clamp01(readComponent(p, pr));
    };
    return readHierarchy(hierarchy, w, h, pr.bytes, L.x, L.y, rect, put, error);
}

// Pseudo-random threshold per canvas pixel for "dissolve".
float noiseAt(int x, int y) {
    quint32 h = quint32(x) * 0x9E3779B1u ^ (quint32(y) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return float(h & 0xFFFFFF) / float(0x1000000);
}

void Compositor::composite(QImage& canvas, const QImage& src, QPoint at, const QVector<float>* mask,
                           quint32 mode, float opacity, qint32 compositeMode) {
    const Op op = opFor(mode);
    const bool legacy = isLegacyMode(mode);
    // Composite mode: 1 union, 2 clip to backdrop (others: like the mode's default).
    bool clipToBackdrop = legacy;
    if (compositeMode != 0) {
        const qint32 cm = compositeMode < 0 ? -compositeMode : compositeMode;
        if (cm == 1)
            clipToBackdrop = false;
        else if (cm == 2)
            clipToBackdrop = true;
        else
            approximate(QStringLiteral("composite mode %1 rendered as union").arg(cm));
    }
    for (int y = 0; y < src.height(); ++y) {
        const int cy = at.y() + y;
        if (cy < 0 || cy >= canvas.height())
            continue;
        const auto* s = reinterpret_cast<const float*>(src.constScanLine(y));
        auto* d = reinterpret_cast<float*>(canvas.scanLine(cy));
        for (int x = 0; x < src.width(); ++x) {
            const int cx = at.x() + x;
            if (cx < 0 || cx >= canvas.width())
                continue;
            const float* sp = s + x * 4;
            float as = sp[3] * opacity;
            if (mask)
                as *= (*mask)[qsizetype(y) * src.width() + x];
            if (op == Op::Dissolve)
                as = noiseAt(cx, cy) < as ? 1.0f : 0.0f;
            if (as <= 0.0f)
                continue;
            float* dp = d + cx * 4;
            const float ab = dp[3];
            switch (op) {
            case Op::Normal:
            case Op::Dissolve:
            case Op::Unsupported: {
                const float ao = as + ab * (1.0f - as);
                for (int i = 0; i < 3; ++i)
                    dp[i] = (sp[i] * as + dp[i] * ab * (1.0f - as)) / ao;
                dp[3] = ao;
                break;
            }
            case Op::Behind: {
                const float ao = ab + as * (1.0f - ab);
                for (int i = 0; i < 3; ++i)
                    dp[i] = (dp[i] * ab + sp[i] * as * (1.0f - ab)) / ao;
                dp[3] = ao;
                break;
            }
            default:
                if (clipToBackdrop) {
                    if (ab <= 0.0f)
                        break;
                    for (int i = 0; i < 3; ++i)
                        dp[i] = dp[i] * (1.0f - as) + blend(op, dp[i], sp[i]) * as;
                } else {
                    const float ao = as + ab * (1.0f - as);
                    for (int i = 0; i < 3; ++i) {
                        const float mixed = (1.0f - ab) * sp[i] + ab * blend(op, dp[i], sp[i]);
                        dp[i] = (mixed * as + dp[i] * ab * (1.0f - as)) / ao;
                    }
                    dp[3] = ao;
                }
                break;
            }
        }
    }
}

bool Compositor::compositeList(QImage& canvas, const QVector<int>& items, int depth, QString* error) {
    if (depth > xcf::kMaxGroupDepth)
        return fail(error, QStringLiteral("groups nested too deep"));
    // Layers are stored top first: composite bottom-up.
    for (auto it = items.crbegin(); it != items.crend(); ++it)
        if (!compositeItem(canvas, *it, depth, error))
            return false;
    return true;
}

bool Compositor::compositeItem(QImage& canvas, int index, int depth, QString* error) {
    const Layer& L = m_layers[index];
    if (!L.visible || L.opacity <= 0.0f)
        return true;
    const QRect rect = sampleRect(L.x, L.y, L.width, L.height);
    QVector<float> mask;
    if (L.mask > 0 && L.applyMask && !rect.isEmpty() && !loadMask(L, rect, mask, error))
        return false;
    if (L.group) {
        if (L.mode == PassThrough) {
            // Children blend onto the backdrop; the group's opacity/mask mix the result in.
            QImage result = canvas.copy();
            if (result.isNull()) {
                setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
                return false;
            }
            if (!compositeList(result, L.children, depth + 1, error))
                return false;
            for (int y = 0; y < canvas.height(); ++y) {
                auto* d = reinterpret_cast<float*>(canvas.scanLine(y));
                const auto* s = reinterpret_cast<const float*>(result.constScanLine(y));
                for (int x = 0; x < canvas.width(); ++x) {
                    float f = L.opacity;
                    if (L.mask > 0 && L.applyMask && !mask.isEmpty()) {
                        f = rect.contains(x, y)
                                ? f * mask[qsizetype(y - rect.top()) * rect.width() + (x - rect.left())]
                                : 0.0f;
                    }
                    if (f <= 0.0f)
                        continue;
                    float* dp = d + x * 4;
                    const float* sp = s + x * 4;
                    // Mix in premultiplied space.
                    const float a = dp[3] + (sp[3] - dp[3]) * f;
                    for (int i = 0; i < 3; ++i) {
                        const float pd = dp[i] * dp[3], ps = sp[i] * sp[3];
                        const float p = pd + (ps - pd) * f;
                        dp[i] = a > 0.0f ? clamp01(p / a) : 0.0f;
                    }
                    dp[3] = a;
                }
            }
            ++m_result.layersComposited;
            return true;
        }
        QImage tmp(canvas.size(), QImage::Format_RGBA32FPx4);
        if (tmp.isNull()) {
            setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
            return false;
        }
        tmp.fill(Qt::transparent);
        if (!compositeList(tmp, L.children, depth + 1, error))
            return false;
        QVector<float> fullMask;
        if (!mask.isEmpty()) { // the group mask covers the group's bounds; outside: hidden
            fullMask = QVector<float>(qsizetype(canvas.width()) * canvas.height(), 0.0f);
            for (int y = 0; y < rect.height(); ++y)
                std::copy_n(mask.constData() + qsizetype(y) * rect.width(), rect.width(),
                            fullMask.data() + qsizetype(rect.top() + y) * canvas.width() + rect.left());
        }
        composite(canvas, tmp, QPoint(0, 0), fullMask.isEmpty() ? nullptr : &fullMask, L.mode, L.opacity,
                  L.compositeMode);
        ++m_result.layersComposited;
        return true;
    }
    if (rect.isEmpty())
        return true; // entirely outside the canvas
    QImage pixels;
    if (!loadLayerPixels(L, rect, pixels, error))
        return false;
    composite(canvas, pixels, rect.topLeft(), mask.isEmpty() ? nullptr : &mask, L.mode, L.opacity,
              L.compositeMode);
    ++m_result.layersComposited;
    return true;
}

std::optional<xcf::Result> Compositor::run(int maxEdge, QString* error) {
    m_result.fullSize = m_full;
    m_result.version = m_h.version;
    m_result.bitsPerComponent = m_h.precision.bits();
    m_result.colorModel = colorModelName(m_h.baseType);
    if (m_h.version > 12)
        approximate(QStringLiteral("XCF version %1 is newer than supported (12)").arg(m_h.version));
    if (!readImageProperties(error))
        return std::nullopt;
    if (m_compression > 2) {
        setError(error, Tr::tr("Die Komprimierung der XCF-Datei wird nicht unterstützt."));
        return std::nullopt;
    }
    if (m_h.baseType == 2 && m_colormap.isEmpty())
        approximate(QStringLiteral("indexed image without colormap"));
    // Layer pointers (top layer first), zero-terminated; channels follow (ignored).
    QVector<qint64> offsets;
    for (;;) {
        const qint64 p = readPointer(m_r);
        if (!m_r.ok() || offsets.size() >= xcf::kMaxLayers) {
            fail(error, QStringLiteral("layer list"));
            return std::nullopt;
        }
        if (p == 0)
            break;
        offsets << p;
    }
    m_layers.resize(offsets.size());
    for (int i = 0; i < offsets.size(); ++i)
        if (!readLayer(offsets[i], m_layers[i], error))
            return std::nullopt;
    QVector<int> roots;
    buildTree(roots);

    QImage canvas(m_canvasSize, QImage::Format_RGBA32FPx4);
    if (canvas.isNull()) {
        setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
        return std::nullopt;
    }
    canvas.fill(Qt::transparent);
    if (!compositeList(canvas, roots, 0, error))
        return std::nullopt;

    const bool deep = m_h.precision.bytes > 1;
    const bool useIcc = m_iccSpace.isValid() && !m_h.precision.linear && m_h.precision.kind == Kind::Int &&
                        m_h.precision.bytes <= 2;
    canvas.setColorSpace(useIcc ? m_iccSpace : QColorSpace(QColorSpace::SRgb));
    canvas.convertTo(deep ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
    if (canvas.isNull()) {
        setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
        return std::nullopt;
    }
    if (maxEdge > 0 && std::max(canvas.width(), canvas.height()) > maxEdge) {
        const QColorSpace cs = canvas.colorSpace();
        canvas = canvas.scaled(decutil::fitLongEdge(m_full, maxEdge), Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        canvas.setColorSpace(cs);
    }
    m_result.image = std::move(canvas);
    if (m_result.approximate)
        qCInfo(lcXcf).noquote() << "approximate rendering:" << m_result.notes.join(QStringLiteral("; "));
    return std::move(m_result);
}

} // namespace

// ---- xcf -----------------------------------------------------------------------------------------

namespace xcf {

bool matchesSignature(const QByteArray& header) {
    return parseVersion(header).has_value();
}

bool isSupportedMode(quint32 mode) {
    return opFor(mode) != Op::Unsupported;
}

std::optional<Probe> probe(const QByteArray& data, QString* error) {
    const auto h = parseHeader(data, error);
    if (!h)
        return std::nullopt;
    Probe p;
    p.size = QSize(int(h->width), int(h->height));
    p.version = h->version;
    p.bitsPerComponent = h->precision.bits();
    p.isFloat = h->precision.isFloat();
    p.colorModel = colorModelName(h->baseType);
    return p;
}

std::optional<Result> decode(const QByteArray& data, int maxEdge, QString* error) {
    const auto h = parseHeader(data, error);
    if (!h)
        return std::nullopt;
    Compositor d(data, *h, maxEdge);
    return d.run(maxEdge, error);
}

} // namespace xcf

// ---- XcfDecoder -----------------------------------------------------------------------------------

bool XcfDecoder::canDecode(const QString& path, const QByteArray& header) const {
    Q_UNUSED(path);
    return xcf::matchesSignature(header);
}

std::optional<ImageInfo> XcfDecoder::info(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return std::nullopt;
    }
    const auto p = xcf::probe(f.read(64), error);
    if (!p)
        return std::nullopt;
    ImageInfo info;
    info.pixelSize = info.displaySize = p->size;
    info.hasAlpha = true;
    info.bitsPerComponent = p->bitsPerComponent;
    info.colorModel = p->colorModel == QLatin1String("Gray") ? p->colorModel : QStringLiteral("RGB");
    info.formatName = QStringLiteral("XCF");
    return info;
}

std::optional<DecodedImage> XcfDecoder::decode(const QString& path, const DecodeRequest& req,
                                               QString* error) {
    if (req.page > 0 || req.frame > 0) {
        setError(error, Tr::tr("Seite %1 existiert nicht.").arg(std::max(req.page, req.frame) + 1));
        return std::nullopt;
    }
    decutil::FileBytes fb;
    if (!decutil::readFile(path, fb, kMaxFileBytes, error, Tr::tr("Die Datei kann nicht gelesen werden."),
                           Tr::tr("Die Datei ist leer (0 Byte)."), Tr::tr("Die Datei ist zu groß.")))
        return std::nullopt;
    const auto p = xcf::probe(fb.data, error);
    if (!p)
        return std::nullopt;
    const int edge = decutil::targetEdge(p->size, p->bitsPerComponent > 8 ? 8 : 4, req);
    auto r = xcf::decode(fb.data, edge, error);
    if (!r)
        return std::nullopt;
    DecodedImage out;
    out.fullSize = r->fullSize;
    out.isPreview = r->image.width() < r->fullSize.width() || r->image.height() < r->fullSize.height();
    out.image = std::move(r->image);
    return out;
}

Metadata XcfDecoder::metadata(const QString& path) {
    Metadata md;
    const auto i = info(path, nullptr);
    if (!i)
        return md;
    addMetadata(md, QStringLiteral("file.format"), i->formatName);
    addMetadata(md, QStringLiteral("file.dimensions"),
                QStringLiteral("%1 × %2").arg(i->displaySize.width()).arg(i->displaySize.height()));
    addMetadata(md, QStringLiteral("file.bitDepth"), i->bitsPerComponent);
    return md;
}

} // namespace lf
