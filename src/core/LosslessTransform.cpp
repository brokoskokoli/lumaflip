// SPDX-License-Identifier: GPL-3.0-or-later
// Saving the rotation (F-EDIT-02), see LosslessTransform.h.

#include "LosslessTransform.h"

#include "Decoder.h"
#include "ImageFormats.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <array>

#ifdef LF_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::LosslessTransform", text);
}

// x' = m[0]*x + m[1]*y, y' = m[2]*x + m[3]*y (pixel coordinates, y down).
using Mat = std::array<int, 4>;
constexpr Mat kOrient[9] = {
    {1, 0, 0, 1},   // (unused)
    {1, 0, 0, 1},   // 1 identity
    {-1, 0, 0, 1},  // 2 mirror horizontal
    {-1, 0, 0, -1}, // 3 rotate 180
    {1, 0, 0, -1},  // 4 mirror vertical
    {0, 1, 1, 0},   // 5 transpose
    {0, -1, 1, 0},  // 6 rotate 90 clockwise
    {0, -1, -1, 0}, // 7 transverse
    {0, 1, -1, 0},  // 8 rotate 90 counter-clockwise
};

int clampOrientation(int o) {
    return o >= 1 && o <= 8 ? o : 1;
}

Mat mul(const Mat& a, const Mat& b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
            a[2] * b[1] + a[3] * b[3]};
}

int codeOf(const Mat& m) {
    for (int o = 1; o <= 8; ++o)
        if (kOrient[o] == m)
            return o;
    return 1;
}

// ---- JPEG byte level ----

quint16 be16(const QByteArray& d, qsizetype i) {
    return quint16((uchar(d[i]) << 8) | uchar(d[i + 1]));
}

struct Segment {
    uchar marker = 0;
    qsizetype start = 0;   // offset of 0xFF
    qsizetype payload = 0; // offset of the payload (after the length)
    qsizetype length = 0;  // payload length
};

// Calls fn(segment) for every marker segment before the first SOS; returns the offset of
// the SOS marker, or -1 if the structure is broken.
template <typename Fn> qsizetype walkSegments(const QByteArray& d, Fn fn) {
    if (d.size() < 4 || uchar(d[0]) != 0xFF || uchar(d[1]) != 0xD8)
        return -1;
    qsizetype pos = 2;
    while (pos + 4 <= d.size()) {
        if (uchar(d[pos]) != 0xFF)
            return -1;
        qsizetype m = pos + 1;
        while (m < d.size() && uchar(d[m]) == 0xFF)
            ++m; // fill bytes
        if (m >= d.size())
            return -1;
        const uchar marker = uchar(d[m]);
        if (marker == 0xDA)
            return pos;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos = m + 1;
            continue;
        }
        if (m + 3 > d.size())
            return -1;
        const qsizetype len = be16(d, m + 1);
        if (len < 2 || m + 1 + len > d.size())
            return -1;
        Segment s;
        s.marker = marker;
        s.start = pos;
        s.payload = m + 3;
        s.length = len - 2;
        if (!fn(s))
            return pos;
        pos = m + 1 + len;
    }
    return -1;
}

// TIFF structure inside the EXIF APP1 segment.
struct Tiff {
    qsizetype base = 0; // offset of the TIFF header in the file
    qsizetype size = 0;
    bool little = true;
    quint16 u16(const QByteArray& d, qsizetype off) const {
        const qsizetype i = base + off;
        return little ? quint16(uchar(d[i]) | (uchar(d[i + 1]) << 8)) : be16(d, i);
    }
    quint32 u32(const QByteArray& d, qsizetype off) const {
        const qsizetype i = base + off;
        if (little)
            return quint32(uchar(d[i])) | (quint32(uchar(d[i + 1])) << 8) | (quint32(uchar(d[i + 2])) << 16) |
                   (quint32(uchar(d[i + 3])) << 24);
        return (quint32(uchar(d[i])) << 24) | (quint32(uchar(d[i + 1])) << 16) |
               (quint32(uchar(d[i + 2])) << 8) | quint32(uchar(d[i + 3]));
    }
    void put16(QByteArray& d, qsizetype off, quint16 v) const {
        const qsizetype i = base + off;
        d[i] = char(little ? (v & 0xFF) : (v >> 8));
        d[i + 1] = char(little ? (v >> 8) : (v & 0xFF));
    }
    void put32(QByteArray& d, qsizetype off, quint32 v) const {
        for (int k = 0; k < 4; ++k) {
            const int shift = little ? 8 * k : 8 * (3 - k);
            d[base + off + k] = char((v >> shift) & 0xFF);
        }
    }
};

// Finds the EXIF TIFF block; false if there is none or it is malformed.
bool findExif(const QByteArray& d, Tiff* tiff) {
    bool found = false;
    walkSegments(d, [&](const Segment& s) {
        if (s.marker != 0xE1 || s.length < 14 || d.mid(s.payload, 6) != QByteArray("Exif\0\0", 6))
            return true;
        Tiff t;
        t.base = s.payload + 6;
        t.size = s.length - 6;
        const QByteArray order = d.mid(t.base, 2);
        if (order == "II")
            t.little = true;
        else if (order == "MM")
            t.little = false;
        else
            return true;
        if (t.u16(d, 2) != 42)
            return true;
        *tiff = t;
        found = true;
        return false;
    });
    return found;
}

// Offset (relative to the TIFF header) of the orientation entry in IFD0, or -1.
qsizetype orientationEntry(const QByteArray& d, const Tiff& t, qsizetype* nextIfdField = nullptr) {
    const quint32 ifd0 = t.u32(d, 4);
    if (ifd0 < 8 || qsizetype(ifd0) + 2 > t.size)
        return -1;
    const int n = t.u16(d, ifd0);
    const qsizetype entries = qsizetype(ifd0) + 2;
    if (entries + qsizetype(n) * 12 + 4 > t.size)
        return -1;
    if (nextIfdField)
        *nextIfdField = entries + qsizetype(n) * 12;
    for (int i = 0; i < n; ++i) {
        const qsizetype e = entries + qsizetype(i) * 12;
        if (t.u16(d, e) == 0x0112 && t.u16(d, e + 2) == 3 && t.u32(d, e + 4) == 1)
            return e;
    }
    return -2; // valid IFD, no tag
}

// Patches a one-digit XMP tiff:Orientation in place (attribute or element form).
void patchXmpOrientation(QByteArray& d, int orientation) {
    walkSegments(d, [&](const Segment& s) {
        static const QByteArray xmpId("http://ns.adobe.com/xap/1.0/\0", 29);
        if (s.marker != 0xE1 || s.length < xmpId.size() || d.mid(s.payload, xmpId.size()) != xmpId)
            return true;
        const qsizetype end = s.payload + s.length;
        for (const QByteArray& key : {QByteArray("tiff:Orientation=\""), QByteArray("tiff:Orientation='"),
                                      QByteArray("<tiff:Orientation>")}) {
            qsizetype at = d.indexOf(key, s.payload);
            while (at >= 0 && at + key.size() + 1 < end) {
                const qsizetype v = at + key.size();
                const char close = d[v + 1];
                if (d[v] >= '1' && d[v] <= '8' && (close == '"' || close == '\'' || close == '<'))
                    d[v] = char('0' + orientation);
                at = d.indexOf(key, v);
            }
        }
        return true;
    });
}

} // namespace

// ---- orientation ----

int orientation::multiply(int a, int b) {
    return codeOf(mul(kOrient[clampOrientation(a)], kOrient[clampOrientation(b)]));
}

int orientation::inverse(int o) {
    for (int i = 1; i <= 8; ++i)
        if (multiply(i, o) == 1)
            return i;
    return 1;
}

bool orientation::swapsAxes(int o) {
    return o >= 5 && o <= 8;
}

int orientation::compose(int exif, int quarterTurnsCW, bool flipH, bool flipV) {
    int view = 1;
    const int q = ((quarterTurnsCW % 4) + 4) % 4;
    for (int i = 0; i < q; ++i)
        view = multiply(6, view);
    if (flipH)
        view = multiply(2, view);
    if (flipV)
        view = multiply(4, view);
    return multiply(view, clampOrientation(exif));
}

// ---- JPEG ----

bool jpeg::isJpeg(const QByteArray& data) {
    return data.size() > 3 && uchar(data[0]) == 0xFF && uchar(data[1]) == 0xD8 && uchar(data[2]) == 0xFF;
}

QByteArray jpeg::scanData(const QByteArray& data) {
    const qsizetype sos = walkSegments(data, [](const Segment&) { return true; });
    return sos < 0 ? QByteArray() : data.mid(sos);
}

int jpeg::exifOrientation(const QByteArray& data) {
    Tiff t;
    if (!findExif(data, &t))
        return 0;
    const qsizetype e = orientationEntry(data, t);
    if (e < 0)
        return 0;
    const int o = t.u16(data, e + 8);
    return o >= 1 && o <= 8 ? o : 0;
}

jpeg::PatchResult jpeg::patchOrientation(QByteArray& data, int orientation, bool dropThumbnail) {
    if (!isJpeg(data) || orientation < 1 || orientation > 8)
        return PatchResult::Invalid;
    Tiff t;
    if (!findExif(data, &t))
        return PatchResult::NoTag;
    qsizetype next = -1;
    const qsizetype e = orientationEntry(data, t, &next);
    if (e == -1)
        return PatchResult::Invalid;
    if (dropThumbnail && next > 0)
        t.put32(data, next, 0); // IFD0 is the last IFD: the old thumbnail is not referenced any more
    if (e < 0)
        return PatchResult::NoTag;
    t.put16(data, e + 8, quint16(orientation));
    patchXmpOrientation(data, orientation);
    return PatchResult::Patched;
}

bool jpeg::dctTransformAvailable() {
#ifdef LF_HAVE_TURBOJPEG
    return true;
#else
    return false;
#endif
}

jpeg::DctResult jpeg::transformDct(const QByteArray& in, int op, bool trim, QByteArray* out, QString* error) {
#ifdef LF_HAVE_TURBOJPEG
    static constexpr int kOps[9] = {TJXOP_NONE,      TJXOP_NONE,  TJXOP_HFLIP,      TJXOP_ROT180, TJXOP_VFLIP,
                                    TJXOP_TRANSPOSE, TJXOP_ROT90, TJXOP_TRANSVERSE, TJXOP_ROT270};
    tjhandle h = tj3Init(TJINIT_TRANSFORM);
    if (!h) {
        if (error)
            *error = tr("Die JPEG-Bibliothek konnte nicht gestartet werden.");
        return DctResult::Failed;
    }
    auto fail = [&](DctResult r) {
        if (error && r == DctResult::Failed)
            *error = tr("Die JPEG-Daten konnten nicht verlustfrei umgeordnet werden (%1).")
                         .arg(QString::fromUtf8(tj3GetErrorStr(h)));
        tj3Destroy(h);
        return r;
    };
    const auto* src = reinterpret_cast<const unsigned char*>(in.constData());
    if (tj3DecompressHeader(h, src, size_t(in.size())) < 0)
        return fail(DctResult::Failed);
    const int w = tj3Get(h, TJPARAM_JPEGWIDTH), hgt = tj3Get(h, TJPARAM_JPEGHEIGHT);
    const int ss = tj3Get(h, TJPARAM_SUBSAMP);
    const int xop = kOps[clampOrientation(op)];
    if (!trim && ss >= 0 && ss < TJ_NUMSAMP) {
        // Partial iMCUs on the right/bottom edge cannot move to the left/top (turbojpeg.h).
        const bool partialRight = w % tjMCUWidth[ss] != 0;
        const bool partialBottom = hgt % tjMCUHeight[ss] != 0;
        bool perfect = true;
        switch (xop) {
        case TJXOP_HFLIP:
        case TJXOP_ROT270:
            perfect = !partialRight;
            break;
        case TJXOP_VFLIP:
        case TJXOP_ROT90:
            perfect = !partialBottom;
            break;
        case TJXOP_TRANSVERSE:
        case TJXOP_ROT180:
            perfect = !partialRight && !partialBottom;
            break;
        default:
            break;
        }
        if (!perfect)
            return fail(DctResult::NotPerfect);
    }
    tjtransform xf{};
    xf.op = xop;
    xf.options = trim ? TJXOPT_TRIM : TJXOPT_PERFECT;
    unsigned char* dst = nullptr;
    size_t dstSize = 0;
    if (tj3Transform(h, src, size_t(in.size()), 1, &dst, &dstSize, &xf) < 0) {
        tj3Free(dst);
        return fail(trim ? DctResult::Failed : DctResult::NotPerfect);
    }
    *out = QByteArray(reinterpret_cast<const char*>(dst), qsizetype(dstSize));
    tj3Free(dst);
    tj3Destroy(h);
    return DctResult::Ok;
#else
    Q_UNUSED(in);
    Q_UNUSED(op);
    Q_UNUSED(trim);
    Q_UNUSED(out);
    if (error)
        *error = tr("Verlustfreies Drehen der JPEG-Daten ist in dieser Version nicht verfügbar.");
    return DctResult::Unavailable;
#endif
}

// ---- planning and saving ----

namespace {

QByteArray readAll(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("„%1“ kann nicht gelesen werden.").arg(QFileInfo(path).fileName());
        return {};
    }
    return f.readAll();
}

// Temporary file next to `path` (same volume: the final rename is atomic), hidden.
QString siblingTempPath(const QString& path) {
    const QFileInfo fi(path);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    return fi.dir().filePath(
        QStringLiteral(".%1.lumaflip-%2.%3").arg(fi.completeBaseName(), id, fi.suffix()));
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write(data) != data.size() || !f.flush()) {
        if (error)
            *error = tr("Die Datei konnte nicht geschrieben werden (Ordner schreibgeschützt?).");
        f.close();
        QFile::remove(path);
        return false;
    }
    f.close();
    return true;
}

bool isJpegType(const QString& typeId) {
    return typeId == QLatin1String("public.jpeg");
}

} // namespace

RotationPlan planRotationSave(const RotationRequest& r) {
    RotationPlan plan;
    const QFileInfo fi(r.path);
    if (!fi.isFile()) {
        plan.message = tr("Die Datei existiert nicht mehr.");
        return plan;
    }
    plan.typeId = platform::imageTypeIdentifier(r.path);
    if (plan.typeId.isEmpty()) {
        plan.message = tr("Das Dateiformat wird zum Speichern nicht erkannt.");
        return plan;
    }
    // By content type and, as a safety net, by suffix (a DNG may look like a plain TIFF).
    if (platform::isCameraRawType(plan.typeId) || isRawSuffix(fi.suffix().toLower())) {
        plan.message = tr("RAW-Dateien werden nie verändert. Die Drehung gilt nur in der Anzeige; "
                          "zum dauerhaften Drehen bitte den RAW-Entwickler benutzen.");
        return plan;
    }
    if (platform::imageCount(r.path) > 1) {
        plan.message = tr("Animierte oder mehrseitige Dateien können nicht gedreht gespeichert werden.");
        return plan;
    }
    if (!fi.isWritable() || !QFileInfo(fi.absolutePath()).isWritable()) {
        plan.message = tr("„%1“ ist schreibgeschützt.").arg(fi.fileName());
        return plan;
    }

    int current = 1;
    QByteArray bytes;
    const bool isJpeg = isJpegType(plan.typeId);
    if (isJpeg) {
        bytes = readAll(r.path, &plan.message);
        if (bytes.isEmpty())
            return plan;
        current = std::max(1, jpeg::exifOrientation(bytes));
    } else {
        QString err;
        if (auto info = DecoderRegistry::instance().info(r.path, &err))
            current = clampOrientation(info->exifOrientation);
    }
    const int target = orientation::compose(current, r.quarterTurnsCW, r.flipH, r.flipV);
    plan.status = RotationPlan::Ready;
    if (target == current) {
        plan.method = RotationMethod::None;
        plan.newOrientation = current;
        return plan;
    }

    if (isJpeg && r.strategy == RotationStrategy::LosslessJpeg && jpeg::dctTransformAvailable()) {
        plan.pixelOp = target; // new pixels show as the view does; the tag becomes 1
        plan.newOrientation = 1;
        QByteArray probe;
        QString err;
        const jpeg::DctResult dr = jpeg::transformDct(bytes, target, false, &probe, &err);
        if (dr == jpeg::DctResult::Ok) {
            plan.method = RotationMethod::JpegDct;
            return plan;
        }
        if (dr == jpeg::DctResult::NotPerfect) {
            plan.status = RotationPlan::NeedsTrim;
            plan.method = RotationMethod::JpegDctTrim;
            plan.message =
                tr("Breite oder Höhe des Bildes ist kein Vielfaches der JPEG-Blockgröße. Verlustfrei "
                   "drehen geht nur, wenn dabei ein schmaler Rand (bis zu 15 Pixel) abgeschnitten "
                   "wird. Alternativ wird nur die EXIF-Ausrichtung gesetzt.");
            return plan;
        }
        // DCT transform impossible (unusual JPEG): fall through to the orientation tag.
    }

    plan.newOrientation = target;
    if (isJpeg && jpeg::exifOrientation(bytes) != 0) {
        plan.method = RotationMethod::PatchExif;
        return plan;
    }
    if (platform::supportsOrientationMetadata(plan.typeId)) {
        plan.method = RotationMethod::MetadataCopy;
        return plan;
    }
    // No orientation tag in this format: the pixels themselves must be rotated.
    plan.newOrientation = 1;
    plan.pixelOp = orientation::multiply(target, orientation::inverse(current));
    if (platform::isLosslessCodec(plan.typeId)) {
        plan.method = RotationMethod::ReencodeLossless;
        return plan;
    }
    plan.status = RotationPlan::NeedsReencode;
    plan.method = RotationMethod::ReencodeLossy;
    plan.message = tr("Dieses Format kann keine Ausrichtung speichern. Die Drehung lässt sich nur sichern, "
                      "indem das Bild neu kodiert wird – dabei geht etwas Bildqualität verloren.");
    return plan;
}

QString executeRotationSave(const RotationRequest& r, const RotationPlan& plan, RotationMethod method) {
    if (method == RotationMethod::None)
        return {};
    QString error;
    const QString tmp = siblingTempPath(r.path);
    bool ok = false;
    switch (method) {
    case RotationMethod::PatchExif: {
        QByteArray data = readAll(r.path, &error);
        if (data.isEmpty())
            return error;
        if (jpeg::patchOrientation(data, plan.newOrientation) != jpeg::PatchResult::Patched)
            return tr("Die EXIF-Ausrichtung konnte nicht gesetzt werden.");
        ok = writeFile(tmp, data, &error);
        break;
    }
    case RotationMethod::JpegDct:
    case RotationMethod::JpegDctTrim: {
        const QByteArray data = readAll(r.path, &error);
        if (data.isEmpty())
            return error;
        QByteArray out;
        const auto dr =
            jpeg::transformDct(data, plan.pixelOp, method == RotationMethod::JpegDctTrim, &out, &error);
        if (dr != jpeg::DctResult::Ok)
            return error.isEmpty() ? tr("Verlustfreies Drehen ist für dieses Bild nicht möglich.") : error;
        jpeg::patchOrientation(out, 1, true); // NoTag is fine: then the file has none
        ok = writeFile(tmp, out, &error);
        break;
    }
    case RotationMethod::MetadataCopy: {
        int o = plan.newOrientation;
        if (plan.method != RotationMethod::MetadataCopy && plan.method != RotationMethod::PatchExif) {
            // Chosen instead of a DCT transform: the orientation that matches the view.
            QString err;
            int current = 1;
            if (auto info = DecoderRegistry::instance().info(r.path, &err))
                current = clampOrientation(info->exifOrientation);
            o = orientation::compose(current, r.quarterTurnsCW, r.flipH, r.flipV);
        }
        const QByteArray before = isJpegType(plan.typeId) ? readAll(r.path, &error) : QByteArray();
        if (isJpegType(plan.typeId) && jpeg::exifOrientation(before) != 0) {
            QByteArray data = before;
            if (jpeg::patchOrientation(data, o) != jpeg::PatchResult::Patched)
                return tr("Die EXIF-Ausrichtung konnte nicht gesetzt werden.");
            ok = writeFile(tmp, data, &error);
            break;
        }
        ok = platform::writeOrientationLossless(r.path, tmp, o, &error);
        if (ok && isJpegType(plan.typeId)) {
            // Never silently re-encoded: the compressed image data must be unchanged.
            const QByteArray after = readAll(tmp, &error);
            if (jpeg::scanData(after) != jpeg::scanData(before) || jpeg::scanData(before).isEmpty()) {
                QFile::remove(tmp);
                return tr("Die Ausrichtung ließ sich nicht ohne Neukodieren speichern.");
            }
        }
        break;
    }
    case RotationMethod::ReencodeLossless:
    case RotationMethod::ReencodeLossy:
        ok = platform::reencodeTransformed(r.path, tmp, plan.pixelOp, &error);
        break;
    case RotationMethod::None:
        return {};
    }
    if (!ok) {
        QFile::remove(tmp);
        return error.isEmpty() ? tr("Die Drehung konnte nicht gespeichert werden.") : error;
    }
    if (!platform::replaceFile(tmp, r.path, r.keepModificationDate, &error)) {
        QFile::remove(tmp);
        return error.isEmpty() ? tr("Die Datei konnte nicht ersetzt werden.") : error;
    }
    return {};
}

QString restoreFileFrom(const QString& backup, const QString& target) {
    QString error;
    const QString tmp = siblingTempPath(target);
    if (!platform::copyWithMetadata(backup, tmp, &error))
        return error;
    if (!platform::replaceFile(tmp, target, false, &error)) {
        QFile::remove(tmp);
        return error.isEmpty() ? tr("Die Datei konnte nicht ersetzt werden.") : error;
    }
    return {};
}

QString describeRotationMethod(RotationMethod method) {

    switch (method) {
    case RotationMethod::None:
        return tr("Keine Drehung zu speichern");
    case RotationMethod::PatchExif:
    case RotationMethod::MetadataCopy:
        return tr("Drehung gespeichert (EXIF-Ausrichtung)");
    case RotationMethod::JpegDct:
        return tr("Drehung verlustfrei gespeichert");
    case RotationMethod::JpegDctTrim:
        return tr("Drehung verlustfrei gespeichert (Rand beschnitten)");
    case RotationMethod::ReencodeLossless:
        return tr("Drehung gespeichert (verlustfrei neu kodiert)");
    case RotationMethod::ReencodeLossy:
        return tr("Drehung gespeichert (neu kodiert)");
    }
    return {};
}

} // namespace lf
