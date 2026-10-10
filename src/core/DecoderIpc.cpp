// SPDX-License-Identifier: GPL-3.0-or-later
#include "DecoderIpc.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <QtNumeric>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lf::ipc {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::DecoderHelper)
};

constexpr QDataStream::Version kStreamVersion = QDataStream::Qt_6_8;

template <typename T> void putOptional(QDataStream& s, const std::optional<T>& v) {
    s << bool(v.has_value());
    if (v)
        s << *v;
}
template <typename T> void getOptional(QDataStream& s, std::optional<T>& v) {
    bool has = false;
    s >> has;
    if (has) {
        T value{};
        s >> value;
        v = value;
    } else {
        v.reset();
    }
}

void put(QDataStream& s, const DecodeRequest& r) {
    s << qint32(r.page) << qint32(r.frame) << qint32(r.maxPixelSize) << qint64(r.maxBytes)
      << r.preferEmbeddedPreview << r.hdr << r.hdrFloat;
}
void get(QDataStream& s, DecodeRequest& r) {
    qint32 page = 0, frame = 0, px = 0;
    qint64 bytes = 0;
    s >> page >> frame >> px >> bytes >> r.preferEmbeddedPreview >> r.hdr >> r.hdrFloat;
    r.page = page;
    r.frame = frame;
    r.maxPixelSize = px;
    r.maxBytes = bytes;
}

void put(QDataStream& s, const ImageInfo& i) {
    s << i.pixelSize << i.displaySize << qint32(i.exifOrientation) << qint32(i.frameCount)
      << qint32(i.pageCount) << i.hasAlpha << i.isHdr << i.isAnimated << qint32(i.bitsPerComponent)
      << i.colorModel << i.profileName << i.formatName;
}
void get(QDataStream& s, ImageInfo& i) {
    qint32 o = 1, frames = 1, pages = 1, bits = 8;
    s >> i.pixelSize >> i.displaySize >> o >> frames >> pages >> i.hasAlpha >> i.isHdr >> i.isAnimated >>
        bits >> i.colorModel >> i.profileName >> i.formatName;
    i.exifOrientation = o;
    i.frameCount = frames;
    i.pageCount = pages;
    i.bitsPerComponent = bits;
}

void put(QDataStream& s, const DecodedImage& d) {
    s << d.fullSize << d.isPreview << d.headroom << qint32(d.frameDurationMs) << qint32(d.page)
      << qint32(d.frame) << qint32(d.pageCount) << qint32(d.frameCount);
}
void get(QDataStream& s, DecodedImage& d) {
    qint32 dur = 0, page = 0, frame = 0, pages = 1, frames = 1;
    s >> d.fullSize >> d.isPreview >> d.headroom >> dur >> page >> frame >> pages >> frames;
    d.frameDurationMs = dur;
    d.page = page;
    d.frame = frame;
    d.pageCount = pages;
    d.frameCount = frames;
}

void put(QDataStream& s, const PixelHeader& p) {
    s << qint32(p.width) << qint32(p.height) << qint32(p.format) << p.bytesPerLine << p.iccProfile
      << p.shmSize;
}
void get(QDataStream& s, PixelHeader& p) {
    qint32 w = 0, h = 0, f = 0;
    s >> w >> h >> f >> p.bytesPerLine >> p.iccProfile >> p.shmSize;
    p.width = w;
    p.height = h;
    p.format = f;
}

// Metadata values cross the process boundary as a closed set of plain types. A QVariant
// stream would let a compromised helper make the app construct any registered type (e.g.
// a QImage, decoded by the app's image plugins - exactly what the helper is for).
enum class ValueKind : quint8 { Invalid, Bool, Int, UInt, Double, String, DateTime, Map, List, Bytes };
constexpr int kMaxValueDepth = 8;
constexpr qint32 kMaxValueItems = 100000;

void putValue(QDataStream& s, const QVariant& v, int depth = 0) {
    if (depth > kMaxValueDepth) {
        s << quint8(ValueKind::Invalid);
        return;
    }
    switch (v.typeId()) {
    case QMetaType::Bool:
        s << quint8(ValueKind::Bool) << v.toBool();
        return;
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::Char:
    case QMetaType::SChar:
        s << quint8(ValueKind::Int) << qint64(v.toLongLong());
        return;
    case QMetaType::UInt:
    case QMetaType::ULong:
    case QMetaType::ULongLong:
    case QMetaType::UShort:
    case QMetaType::UChar:
        s << quint8(ValueKind::UInt) << quint64(v.toULongLong());
        return;
    case QMetaType::Double:
    case QMetaType::Float:
        s << quint8(ValueKind::Double) << v.toDouble();
        return;
    case QMetaType::QDateTime:
        s << quint8(ValueKind::DateTime) << v.toDateTime();
        return;
    case QMetaType::QByteArray:
        s << quint8(ValueKind::Bytes) << v.toByteArray();
        return;
    case QMetaType::QVariantMap: {
        const QVariantMap m = v.toMap();
        s << quint8(ValueKind::Map) << qint32(m.size());
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            s << it.key();
            putValue(s, it.value(), depth + 1);
        }
        return;
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        const QVariantList l = v.toList();
        s << quint8(ValueKind::List) << qint32(l.size());
        for (const QVariant& x : l)
            putValue(s, x, depth + 1);
        return;
    }
    default:
        if (!v.isValid()) {
            s << quint8(ValueKind::Invalid);
            return;
        }
        s << quint8(ValueKind::String) << v.toString();
        return;
    }
}

QVariant getValue(QDataStream& s, int depth = 0) {
    quint8 kind = 0;
    s >> kind;
    auto corrupt = [&] {
        s.setStatus(QDataStream::ReadCorruptData);
        return QVariant();
    };
    if (depth > kMaxValueDepth)
        return corrupt();
    switch (ValueKind(kind)) {
    case ValueKind::Invalid:
        return QVariant();
    case ValueKind::Bool: {
        bool b = false;
        s >> b;
        return b;
    }
    case ValueKind::Int: {
        qint64 i = 0;
        s >> i;
        return QVariant::fromValue(i);
    }
    case ValueKind::UInt: {
        quint64 u = 0;
        s >> u;
        return QVariant::fromValue(u);
    }
    case ValueKind::Double: {
        double d = 0;
        s >> d;
        return d;
    }
    case ValueKind::String: {
        QString t;
        s >> t;
        return t;
    }
    case ValueKind::DateTime: {
        QDateTime t;
        s >> t;
        return t;
    }
    case ValueKind::Bytes: {
        QByteArray b;
        s >> b;
        return b;
    }
    case ValueKind::Map: {
        qint32 n = 0;
        s >> n;
        if (n < 0 || n > kMaxValueItems)
            return corrupt();
        QVariantMap m;
        for (qint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
            QString key;
            s >> key;
            m.insert(key, getValue(s, depth + 1));
        }
        return m;
    }
    case ValueKind::List: {
        qint32 n = 0;
        s >> n;
        if (n < 0 || n > kMaxValueItems)
            return corrupt();
        QVariantList l;
        for (qint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i)
            l.append(getValue(s, depth + 1));
        return l;
    }
    }
    return corrupt();
}

void put(QDataStream& s, const Metadata& m) {
    putValue(s, m.groups);
    s << qint32(m.entries.size());
    for (const MetadataEntry& e : m.entries) {
        s << e.key << e.label;
        putValue(s, e.value);
    }
    putOptional(s, m.captured);
    putOptional(s, m.latitude);
    putOptional(s, m.longitude);
    s << qint32(m.rating);
}
void get(QDataStream& s, Metadata& m) {
    qint32 n = 0;
    m.groups = getValue(s).toMap();
    s >> n;
    m.entries.clear();
    if (n < 0 || n > kMaxValueItems) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    for (qint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
        MetadataEntry e;
        s >> e.key >> e.label;
        e.value = getValue(s);
        m.entries.append(e);
    }
    getOptional(s, m.captured);
    getOptional(s, m.latitude);
    getOptional(s, m.longitude);
    qint32 rating = -1;
    s >> rating;
    m.rating = rating;
}

} // namespace

QByteArray encode(const Request& r) {
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(kStreamVersion);
    s << kProtocolVersion << quint8(1) << r.id << quint8(r.op) << r.backend << r.path;
    put(s, r.decode);
    s << r.shm;
    return out;
}

QByteArray encode(const Response& r) {
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(kStreamVersion);
    s << kProtocolVersion << quint8(2) << r.id << r.ok << r.error << r.sandboxed << r.pid;
    put(s, r.info);
    put(s, r.decoded);
    put(s, r.pixels);
    put(s, r.metadata);
    return out;
}

std::optional<Request> decodeRequest(const QByteArray& payload) {
    QDataStream s(payload);
    s.setVersion(kStreamVersion);
    quint32 version = 0;
    quint8 kind = 0, op = 0;
    Request r;
    s >> version >> kind;
    if (version != kProtocolVersion || kind != 1)
        return std::nullopt;
    s >> r.id >> op >> r.backend >> r.path;
    get(s, r.decode);
    s >> r.shm;
    if (s.status() != QDataStream::Ok)
        return std::nullopt;
    r.op = Op(op);
    return r;
}

std::optional<Response> decodeResponse(const QByteArray& payload) {
    QDataStream s(payload);
    s.setVersion(kStreamVersion);
    quint32 version = 0;
    quint8 kind = 0;
    Response r;
    s >> version >> kind;
    if (version != kProtocolVersion || kind != 2)
        return std::nullopt;
    s >> r.id >> r.ok >> r.error >> r.sandboxed >> r.pid;
    get(s, r.info);
    get(s, r.decoded);
    get(s, r.pixels);
    get(s, r.metadata);
    if (s.status() != QDataStream::Ok)
        return std::nullopt;
    return r;
}

QByteArray shmName(qint64 pid, quint32 requestId) {
    // "/lfd.<pid>.<id>": at most 5 + 10 + 1 + 10 = 26 characters (PSHMNAMLEN is 31).
    return kShmPrefix + QByteArray::number(pid) + '.' + QByteArray::number(requestId);
}

qsizetype parseFrame(const QByteArray& buffer, QByteArray* payload) {
    if (buffer.size() < 4)
        return 0;
    const auto* b = reinterpret_cast<const uchar*>(buffer.constData());
    const quint32 len = quint32(b[0]) | (quint32(b[1]) << 8) | (quint32(b[2]) << 16) | (quint32(b[3]) << 24);
    if (len > kMaxFrameBytes)
        return -1;
    if (quint64(buffer.size()) - 4 < len)
        return 0;
    *payload = buffer.mid(4, qsizetype(len));
    return 4 + qsizetype(len);
}

PixelHeader describeImage(const QImage& image) {
    PixelHeader h;
    h.width = image.width();
    h.height = image.height();
    h.format = int(image.format());
    h.bytesPerLine = image.bytesPerLine();
    h.shmSize = qint64(image.bytesPerLine()) * image.height();
    h.iccProfile = image.colorSpace().isValid() ? image.colorSpace().iccProfile() : QByteArray();
    return h;
}

bool validatePixelHeader(const PixelHeader& h, QString* error) {
    auto invalid = [&] {
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders sind ungültig.");
        return false;
    };
    if (h.width <= 0 || h.height <= 0 || h.bytesPerLine <= 0 || h.shmSize <= 0)
        return invalid();
    if (h.format <= QImage::Format_Invalid || h.format >= QImage::NImageFormats)
        return invalid();
    const auto fmt = QImage::Format(h.format);
    // Indexed and 1-bit formats need a colour table the protocol does not carry.
    if (fmt == QImage::Format_Indexed8 || fmt == QImage::Format_Mono || fmt == QImage::Format_MonoLSB)
        return invalid();
    const int bpp = QImage::toPixelFormat(fmt).bitsPerPixel();
    if (bpp < 8 || bpp % 8 != 0)
        return invalid();
    qint64 rowBytes = 0, total = 0;
    if (qMulOverflow(qint64(h.width), qint64(bpp / 8), &rowBytes) || h.bytesPerLine < rowBytes)
        return invalid();
    if (qMulOverflow(h.bytesPerLine, qint64(h.height), &total) || total != h.shmSize)
        return invalid();
    if (quint64(h.shmSize) > quint64(std::numeric_limits<size_t>::max()) ||
        h.bytesPerLine > qint64(std::numeric_limits<qsizetype>::max()))
        return invalid();
    return true;
}

#ifdef Q_OS_UNIX

namespace {

bool waitReadable(int fd, int timeoutMs, IoStatus* status) {
    pollfd p{fd, POLLIN, 0};
    for (;;) {
        const int r = poll(&p, 1, timeoutMs);
        if (r > 0)
            return true;
        if (r == 0) {
            *status = IoStatus::Timeout;
            return false;
        }
        if (errno != EINTR) {
            *status = IoStatus::Error;
            return false;
        }
    }
}

using Clock = std::chrono::steady_clock;

// Milliseconds left until `deadline` (0 when it passed).
int remainingMs(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return int(std::clamp<qint64>(left, 0, std::numeric_limits<int>::max()));
}

// Reads exactly `size` bytes until `deadline` (nullptr = no limit).
IoStatus readExactly(int fd, char* data, size_t size, const Clock::time_point* deadline) {
    size_t done = 0;
    while (done < size) {
        IoStatus st = IoStatus::Ok;
        if (deadline && !waitReadable(fd, remainingMs(*deadline), &st))
            return st;
        const ssize_t n = read(fd, data + done, size - done);
        if (n == 0)
            return IoStatus::Closed;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return IoStatus::Error;
        }
        done += size_t(n);
    }
    return IoStatus::Ok;
}

} // namespace

IoStatus writeFrame(int fd, const QByteArray& payload) {
    if (quint64(payload.size()) > kMaxFrameBytes)
        return IoStatus::TooLarge;
    const quint32 len = quint32(payload.size());
    const unsigned char header[4] = {uchar(len & 0xFF), uchar((len >> 8) & 0xFF), uchar((len >> 16) & 0xFF),
                                     uchar((len >> 24) & 0xFF)};
    QByteArray all(reinterpret_cast<const char*>(header), 4);
    all += payload;
    size_t done = 0;
    while (done < size_t(all.size())) {
        const ssize_t n = write(fd, all.constData() + done, size_t(all.size()) - done);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return errno == EPIPE ? IoStatus::Closed : IoStatus::Error;
        }
        done += size_t(n);
    }
    return IoStatus::Ok;
}

IoStatus readFrame(int fd, QByteArray* payload, int timeoutMs) {
    // One absolute deadline for the whole frame: a peer that trickles bytes cannot stretch it.
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
    const Clock::time_point* limit = timeoutMs < 0 ? nullptr : &deadline;
    unsigned char header[4];
    IoStatus st = readExactly(fd, reinterpret_cast<char*>(header), 4, limit);
    if (st != IoStatus::Ok)
        return st;
    const quint32 len = quint32(header[0]) | (quint32(header[1]) << 8) | (quint32(header[2]) << 16) |
                        (quint32(header[3]) << 24);
    if (len > kMaxFrameBytes)
        return IoStatus::TooLarge;
    payload->resize(qsizetype(len));
    return readExactly(fd, payload->data(), len, limit);
}

bool fillSharedImage(const QByteArray& name, const QImage& image, QString* error) {
    const qint64 size = qint64(image.bytesPerLine()) * image.height();
    if (image.isNull() || size <= 0) {
        if (error)
            *error = Tr::tr("Das Bild ist leer.");
        return false;
    }
    const int fd = name.startsWith(kShmPrefix) ? shm_open(name.constData(), O_RDWR, 0) : -1;
    if (fd < 0) {
        if (error)
            *error = Tr::tr("Gemeinsamer Speicher ist nicht verfügbar (%1).")
                         .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    // The descriptor keeps the memory alive: the name is not needed any more (and must not
    // outlive the app should it die now).
    shm_unlink(name.constData());
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && st.st_size >= size;
    void* mem = ok ? mmap(nullptr, size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    ok = mem != MAP_FAILED;
    if (ok) {
        std::memcpy(mem, image.constBits(), size_t(size));
        munmap(mem, size_t(size));
    }
    close(fd);
    if (!ok && error)
        *error = Tr::tr("Nicht genug Speicher für das Bild (%1 × %2 Pixel).")
                     .arg(image.width())
                     .arg(image.height());
    return ok;
}

int createShared(const QByteArray& name, qint64 size, QString* error) {
    const int fd = size > 0 ? shm_open(name.constData(), O_RDWR | O_CREAT | O_EXCL, 0600) : -1;
    if (fd < 0) {
        const int code = size > 0 ? errno : EINVAL;
        if (error)
            *error = Tr::tr("Gemeinsamer Speicher ist nicht verfügbar (%1).")
                         .arg(QString::fromLocal8Bit(strerror(code)));
        return -1;
    }
    if (ftruncate(fd, off_t(size)) != 0) {
        close(fd);
        shm_unlink(name.constData());
        if (error)
            *error = Tr::tr("Gemeinsamer Speicher ist nicht verfügbar (%1).")
                         .arg(QString::fromLocal8Bit(strerror(ENOMEM)));
        return -1;
    }
    return fd;
}

namespace {
struct Mapping {
    void* address;
    size_t size;
};
void releaseMapping(void* info) {
    auto* m = static_cast<Mapping*>(info);
    munmap(m->address, m->size);
    delete m;
}
} // namespace

QImage mapSharedImage(int fd, const PixelHeader& h, QString* error) {
    if (fd < 0) {
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders fehlen.");
        return {};
    }
    struct stat st{};
    const bool sane = validatePixelHeader(h, nullptr) && fstat(fd, &st) == 0 && st.st_size >= h.shmSize;
    // Read-write mapping: a QImage on read-only memory deep-copies itself on the first
    // non-const access (e.g. setColorSpace). Nobody else has the object any more.
    void* mem =
        sane ? mmap(nullptr, size_t(h.shmSize), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (mem == MAP_FAILED) {
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders sind ungültig.");
        return {};
    }
    auto* m = new Mapping{mem, size_t(h.shmSize)};
    QImage img(static_cast<uchar*>(mem), h.width, h.height, qsizetype(h.bytesPerLine),
               QImage::Format(h.format), releaseMapping, m);
    if (img.isNull()) {
        releaseMapping(m);
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders sind ungültig.");
        return {};
    }
    if (!h.iccProfile.isEmpty())
        img.setColorSpace(QColorSpace::fromIccProfile(h.iccProfile));
    return img;
}

bool unlinkShared(const QByteArray& name) {
    return shm_unlink(name.constData()) == 0;
}

bool sharedExists(const QByteArray& name) {
    const int fd = shm_open(name.constData(), O_RDONLY, 0);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

#else // no POSIX: the helper process is not used on this platform (yet)

IoStatus writeFrame(int, const QByteArray&) {
    return IoStatus::Error;
}
IoStatus readFrame(int, QByteArray*, int) {
    return IoStatus::Error;
}
bool fillSharedImage(const QByteArray&, const QImage&, QString*) {
    return false;
}
int createShared(const QByteArray&, qint64, QString*) {
    return -1;
}
QImage mapSharedImage(int, const PixelHeader&, QString*) {
    return {};
}
bool unlinkShared(const QByteArray&) {
    return false;
}
bool sharedExists(const QByteArray&) {
    return false;
}

#endif

} // namespace lf::ipc
