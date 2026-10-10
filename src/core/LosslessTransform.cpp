// SPDX-License-Identifier: GPL-3.0-or-later
// Saving the rotation (F-EDIT-02), see LosslessTransform.h.

#include "LosslessTransform.h"

#include "Decoder.h"
#include "ImageFormats.h"
#include "UndoBackupStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <array>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#ifdef LF_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::LosslessTransform)
};

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
            *error = Tr::tr("Die JPEG-Bibliothek konnte nicht gestartet werden.");
        return DctResult::Failed;
    }
    auto fail = [&](DctResult r) {
        if (error && r == DctResult::Failed)
            *error = Tr::tr("Die JPEG-Daten konnten nicht verlustfrei umgeordnet werden (%1).")
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
        *error = Tr::tr("Verlustfreies Drehen der JPEG-Daten ist in dieser Version nicht verfügbar.");
    return DctResult::Unavailable;
#endif
}

qsizetype jpeg::primaryImageEnd(const QByteArray& d) {
    qsizetype pos = walkSegments(d, [](const Segment&) { return true; });
    if (pos < 0)
        return -1;
    const qsizetype n = d.size();
    while (pos + 1 < n) {
        if (uchar(d[pos]) != 0xFF)
            return -1;
        qsizetype m = pos + 1;
        while (m < n && uchar(d[m]) == 0xFF)
            ++m;
        if (m >= n)
            return -1;
        const uchar marker = uchar(d[m]);
        if (marker == 0xD9)
            return m + 1;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos = m + 1;
            continue;
        }
        if (m + 3 > n)
            return -1;
        const qsizetype len = be16(d, m + 1);
        if (len < 2 || m + 1 + len > n)
            return -1;
        pos = m + 1 + len;
        if (marker != 0xDA)
            continue; // DHT, DQT, DRI, … between the scans
        // Entropy-coded data: ends at the first marker that is no stuffing/restart marker.
        while (pos + 1 < n) {
            if (uchar(d[pos]) != 0xFF) {
                ++pos;
                continue;
            }
            const uchar next = uchar(d[pos + 1]);
            if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
                pos += 2;
                continue;
            }
            if (next == 0xFF) {
                ++pos; // fill byte
                continue;
            }
            break; // a marker: handled by the outer loop
        }
    }
    return -1;
}

bool jpeg::hasAdditionalImages(const QByteArray& data) {
    if (!isJpeg(data))
        return false;
    bool mpf = false;
    walkSegments(data, [&](const Segment& s) {
        if (s.marker == 0xE2 && s.length >= 4 && data.mid(s.payload, 4) == QByteArray("MPF\0", 4))
            mpf = true;
        return !mpf;
    });
    if (mpf)
        return true;
    const qsizetype end = primaryImageEnd(data);
    if (end < 0)
        return false; // no complete primary image: nothing behind it either
    for (qsizetype i = end; i < data.size(); ++i)
        if (uchar(data[i]) != 0x00 && uchar(data[i]) != 0xFF)
            return true; // anything but padding (gain map, second view, maker trailer)
    return false;
}

// ---- file identity ----

FileStamp fileStamp(const QString& path) {
    FileStamp s;
#ifdef Q_OS_UNIX
    struct stat st{};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISREG(st.st_mode))
        return s;
    s.size = qint64(st.st_size);
#ifdef Q_OS_DARWIN
    s.mtimeNs = qint64(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    s.mtimeNs = qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
    s.inode = quint64(st.st_ino);
    s.device = quint64(st.st_dev);
#else
    const QFileInfo fi(path);
    if (!fi.isFile())
        return s;
    s.size = fi.size();
    s.mtimeNs = fi.lastModified().toMSecsSinceEpoch() * 1'000'000;
#endif
    if (s.size <= FileStamp::kHashLimit) {
        QFile f(path);
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly) || !h.addData(&f))
            return FileStamp{};
        s.hash = h.result();
    }
    return s;
}

QString resolvedFilePath(const QString& path) {
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

// ---- durability (test seam) ----

namespace {
std::mutex g_observerMutex;
std::function<void(platform::DurabilityStep, const QString&)> g_observer;
} // namespace

void platform::reportDurabilityStep(DurabilityStep step, const QString& path) {
    std::lock_guard g(g_observerMutex);
    if (g_observer)
        g_observer(step, path);
}

void platform::setDurabilityObserver(std::function<void(DurabilityStep, const QString&)> observer) {
    std::lock_guard g(g_observerMutex);
    g_observer = std::move(observer);
}

// ---- safe file writes ----

QString filesafety::siblingTempPath(const QString& path) {
    const QFileInfo fi(path);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    return fi.dir().filePath(
        QStringLiteral(".%1.lumaflip-%2.%3").arg(fi.completeBaseName(), id, fi.suffix()));
}

bool filesafety::writeNewFile(const QString& path, const QByteArray& data, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write(data) != data.size() || !f.flush()) {
        if (error)
            *error = Tr::tr("Die Datei konnte nicht geschrieben werden (Ordner schreibgeschützt?).");
        f.close();
        QFile::remove(path);
        return false;
    }
    f.close();
    return true;
}

QByteArray filesafety::readFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht gelesen werden.").arg(QFileInfo(path).fileName());
        return {};
    }
    return f.readAll();
}

// ---- planning and saving ----

namespace {

QByteArray readAll(const QString& path, QString* error) {
    return filesafety::readFile(path, error);
}

QString siblingTempPath(const QString& path) {
    return filesafety::siblingTempPath(path);
}

bool writeFile(const QString& path, const QByteArray& data, QString* error) {
    return filesafety::writeNewFile(path, data, error);
}

bool isJpegType(const QString& typeId) {
    return typeId == QLatin1String("public.jpeg");
}

QString messageChangedSincePlan() {
    return Tr::tr("Die Datei wurde inzwischen verändert – nichts gespeichert. Bitte die Drehung erneut "
                  "speichern.");
}

QString messageAdditionalImages() {
    return Tr::tr("Die Datei enthält weitere Bilder (z. B. eine HDR-Gain-Map oder ein zweites Stereobild), "
                  "die beim verlustfreien Umordnen der JPEG-Daten verloren gingen.");
}

} // namespace

RotationPlan planRotationSave(const RotationRequest& r) {
    RotationPlan plan;
    if (!QFileInfo::exists(r.path)) {
        plan.message = Tr::tr("Die Datei existiert nicht mehr.");
        return plan;
    }
    // A symbolic link: the real file is saved (next to itself), the link stays a link.
    plan.target = resolvedFilePath(r.path);
    const QFileInfo fi(plan.target);
    if (!fi.isFile()) {
        plan.message = Tr::tr("Die Datei existiert nicht mehr.");
        return plan;
    }
    plan.typeId = platform::imageTypeIdentifier(plan.target);
    if (plan.typeId.isEmpty()) {
        plan.message = Tr::tr("Das Dateiformat wird zum Speichern nicht erkannt.");
        return plan;
    }
    // By content type and, as a safety net, by suffix (a DNG may look like a plain TIFF).
    if (platform::isCameraRawType(plan.typeId) || isRawSuffix(fi.suffix().toLower()) ||
        isRawSuffix(QFileInfo(r.path).suffix().toLower())) {
        plan.message = Tr::tr("RAW-Dateien werden nie verändert. Die Drehung gilt nur in der Anzeige; "
                              "zum dauerhaften Drehen bitte den RAW-Entwickler benutzen.");
        return plan;
    }
    if (platform::imageCount(plan.target) > 1) {
        plan.message = Tr::tr("Animierte oder mehrseitige Dateien können nicht gedreht gespeichert werden.");
        return plan;
    }
    if (!fi.isWritable() || !QFileInfo(fi.absolutePath()).isWritable()) {
        plan.message = Tr::tr("„%1“ ist schreibgeschützt.").arg(fi.fileName());
        return plan;
    }
    plan.stamp = fileStamp(plan.target);
    if (!plan.stamp.isValid()) {
        plan.message = Tr::tr("„%1“ kann nicht gelesen werden.").arg(fi.fileName());
        return plan;
    }

    int current = 1;
    QByteArray bytes;
    const bool isJpeg = isJpegType(plan.typeId);
    if (isJpeg) {
        bytes = readAll(plan.target, &plan.message);
        if (bytes.isEmpty())
            return plan;
        current = std::max(1, jpeg::exifOrientation(bytes));
    } else {
        QString err;
        if (auto info = DecoderRegistry::instance().info(plan.target, &err))
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
        if (jpeg::hasAdditionalImages(bytes)) {
            // tj3Transform keeps only the primary image; MPF offsets would point into nothing
            // and a gain map would no longer match the rotated pixels: the tag is set instead.
            plan.notice = messageAdditionalImages() + QLatin1Char(' ') +
                          Tr::tr("Deshalb wurde nur die EXIF-Ausrichtung gesetzt.");
        } else {
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
                    Tr::tr("Breite oder Höhe des Bildes ist kein Vielfaches der JPEG-Blockgröße. Verlustfrei "
                           "drehen geht nur, wenn dabei ein schmaler Rand (bis zu 15 Pixel) abgeschnitten "
                           "wird. Alternativ wird nur die EXIF-Ausrichtung gesetzt.");
                return plan;
            }
            // DCT transform impossible (unusual JPEG): fall through to the orientation tag.
            plan.pixelOp = 1;
        }
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
        // "Lossless" only if the codec stores exactly the rotated pixels (color model, bit
        // depth, palette, alpha): tried in the temporary folder, the file is not touched.
        const QString probe =
            QDir(QDir::tempPath())
                .filePath(QStringLiteral("lumaflip-probe-%1.%2")
                              .arg(QUuid::createUuid().toString(QUuid::Id128).left(12), fi.suffix()));
        QString err;
        bool inexact = false;
        const bool exact =
            platform::reencodeTransformed(plan.target, probe, plan.pixelOp, true, &err, &inexact);
        QFile::remove(probe);
        if (exact) {
            plan.method = RotationMethod::ReencodeLossless;
            return plan;
        }
        if (!inexact) {
            plan.status = RotationPlan::Refused;
            plan.message = err.isEmpty() ? Tr::tr("Die Drehung kann nicht gespeichert werden.") : err;
            return plan;
        }
        plan.status = RotationPlan::NeedsReencode;
        plan.method = RotationMethod::ReencodeLossy;
        plan.message =
            Tr::tr("Dieses Format kann keine Ausrichtung speichern, und beim Neukodieren bliebe das "
                   "Bild nicht exakt gleich (Farbmodell, Bittiefe, Palette oder Transparenz würden "
                   "verändert).");
        return plan;
    }
    plan.status = RotationPlan::NeedsReencode;
    plan.method = RotationMethod::ReencodeLossy;
    plan.message =
        Tr::tr("Dieses Format kann keine Ausrichtung speichern. Die Drehung lässt sich nur sichern, "
               "indem das Bild neu kodiert wird – dabei geht etwas Bildqualität verloren.");
    return plan;
}

QString executeRotationSave(const RotationRequest& r, const RotationPlan& plan, RotationMethod method) {
    if (method == RotationMethod::None)
        return {};
    const QString target = plan.target.isEmpty() ? resolvedFilePath(r.path) : plan.target;
    QString error;
    const QString tmp = siblingTempPath(target);
    bool ok = false;
    switch (method) {
    case RotationMethod::PatchExif: {
        QByteArray data = readAll(target, &error);
        if (data.isEmpty())
            return error;
        if (jpeg::patchOrientation(data, plan.newOrientation) != jpeg::PatchResult::Patched)
            return Tr::tr("Die EXIF-Ausrichtung konnte nicht gesetzt werden.");
        ok = writeFile(tmp, data, &error);
        break;
    }
    case RotationMethod::JpegDct:
    case RotationMethod::JpegDctTrim: {
        const QByteArray data = readAll(target, &error);
        if (data.isEmpty())
            return error;
        if (jpeg::hasAdditionalImages(data))
            return messageAdditionalImages();
        QByteArray out;
        const auto dr =
            jpeg::transformDct(data, plan.pixelOp, method == RotationMethod::JpegDctTrim, &out, &error);
        if (dr != jpeg::DctResult::Ok)
            return error.isEmpty() ? Tr::tr("Verlustfreies Drehen ist für dieses Bild nicht möglich.")
                                   : error;
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
            if (auto info = DecoderRegistry::instance().info(target, &err))
                current = clampOrientation(info->exifOrientation);
            o = orientation::compose(current, r.quarterTurnsCW, r.flipH, r.flipV);
        }
        const QByteArray before = isJpegType(plan.typeId) ? readAll(target, &error) : QByteArray();
        if (isJpegType(plan.typeId) && jpeg::exifOrientation(before) != 0) {
            QByteArray data = before;
            if (jpeg::patchOrientation(data, o) != jpeg::PatchResult::Patched)
                return Tr::tr("Die EXIF-Ausrichtung konnte nicht gesetzt werden.");
            ok = writeFile(tmp, data, &error);
            break;
        }
        ok = platform::writeOrientationLossless(target, tmp, o, &error);
        if (ok && isJpegType(plan.typeId)) {
            // Never silently re-encoded: the compressed image data - and everything behind it
            // (MPF gain maps, second views) - must be unchanged.
            const QByteArray after = readAll(tmp, &error);
            if (jpeg::scanData(after) != jpeg::scanData(before) || jpeg::scanData(before).isEmpty()) {
                QFile::remove(tmp);
                return Tr::tr("Die Ausrichtung ließ sich nicht ohne Neukodieren speichern.");
            }
        }
        break;
    }
    case RotationMethod::ReencodeLossless:
    case RotationMethod::ReencodeLossy:
        ok = platform::reencodeTransformed(target, tmp, plan.pixelOp,
                                           method == RotationMethod::ReencodeLossless, &error);
        break;
    case RotationMethod::None:
        return {};
    }
    if (!ok) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Die Drehung konnte nicht gespeichert werden.") : error;
    }
    // The file must still be the one that was planned (and read): an edit made meanwhile by
    // another program is never overwritten.
    if (plan.stamp.isValid() && fileStamp(target) != plan.stamp) {
        QFile::remove(tmp);
        return messageChangedSincePlan();
    }
    if (!platform::replaceFile(tmp, target, r.keepModificationDate, &error)) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Die Datei konnte nicht ersetzt werden.") : error;
    }
    return {};
}

QString restoreFileFrom(const QString& backup, const QString& target) {
    QString error;
    const QString real = resolvedFilePath(target);
    const QString tmp = siblingTempPath(real);
    if (!platform::copyWithMetadata(backup, tmp, &error))
        return error;
    if (!platform::replaceFile(tmp, real, false, &error)) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Die Datei konnte nicht ersetzt werden.") : error;
    }
    return {};
}

// ---- FileEdit / RotationEdit ----

namespace {
const QString kBefore = QStringLiteral("before");
const QString kAfter = QStringLiteral("after");
} // namespace

FileEdit::FileEdit(QString target, Save save, std::shared_ptr<UndoBackupStore> store)
    : m_target(resolvedFilePath(target)), m_save(std::move(save)), m_store(std::move(store)) {}

FileEdit::~FileEdit() {
    if (m_id && m_store)
        m_store->release(m_id);
}

QString FileEdit::messageChangedUndo() {
    return Tr::tr("Datei wurde inzwischen verändert – Rückgängig nicht möglich");
}

QString FileEdit::messageChangedRedo() {
    return Tr::tr("Datei wurde inzwischen verändert – Wiederholen nicht möglich");
}

QString FileEdit::messageBackupDropped() {
    return Tr::tr("Die Sicherungskopie dieses Schritts wurde gelöscht, um Speicherplatz zu sparen – "
                  "Rückgängig/Wiederholen ist dafür nicht mehr möglich.");
}

QString FileEdit::apply() {
    return step(true);
}

QString FileEdit::revert() {
    return step(false);
}

QString FileEdit::step(bool forward) {
    std::lock_guard g(m_mutex);
    const QString& target = m_target;
    QString error;
    if (!m_id) {
        if (!forward)
            return Tr::tr("Es gibt nichts rückgängig zu machen.");
        // First save: backup of the original, then the save, then a copy of the result.
        m_id = m_store ? m_store->create() : 0;
        if (!m_id)
            return Tr::tr("Kein Platz für die Sicherungskopie – nichts gespeichert.");
        if (!m_store->addCopy(m_id, target, kBefore, true, &error))
            return error;
        const QString e = m_save ? m_save() : QString();
        if (!e.isEmpty())
            return e;
        m_done = true;
        m_expected = fileStamp(target);
        // For redo; a clone costs nothing while the file stays as saved. Without it, redo
        // reports the missing backup.
        m_store->addCopy(m_id, target, kAfter, false, &error);
        return {};
    }
    if (m_done == forward)
        return {}; // already in that state
    const QString backup = m_store->file(m_id, forward ? kAfter : kBefore);
    if (backup.isEmpty())
        return messageBackupDropped();
    if (fileStamp(target) != m_expected)
        return forward ? messageChangedRedo() : messageChangedUndo();
    if (const QString e = restoreFileFrom(backup, target); !e.isEmpty())
        return e;
    m_done = forward;
    m_expected = fileStamp(target);
    return {};
}

RotationEdit::RotationEdit(RotationRequest request, RotationPlan plan, RotationMethod method,
                           std::shared_ptr<UndoBackupStore> store) {
    if (plan.target.isEmpty())
        plan.target = resolvedFilePath(request.path);
    const QString target = plan.target;
    m_edit = std::make_unique<FileEdit>(
        target, [request, plan, method] { return executeRotationSave(request, plan, method); },
        std::move(store));
}

RotationEdit::~RotationEdit() = default;

QString describeRotationMethod(RotationMethod method) {

    switch (method) {
    case RotationMethod::None:
        return Tr::tr("Keine Drehung zu speichern");
    case RotationMethod::PatchExif:
    case RotationMethod::MetadataCopy:
        return Tr::tr("Drehung gespeichert (EXIF-Ausrichtung)");
    case RotationMethod::JpegDct:
        return Tr::tr("Drehung verlustfrei gespeichert");
    case RotationMethod::JpegDctTrim:
        return Tr::tr("Drehung verlustfrei gespeichert (Rand beschnitten)");
    case RotationMethod::ReencodeLossless:
        return Tr::tr("Drehung gespeichert (verlustfrei neu kodiert)");
    case RotationMethod::ReencodeLossy:
        return Tr::tr("Drehung gespeichert (neu kodiert)");
    }
    return {};
}

} // namespace lf
