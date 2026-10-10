// SPDX-License-Identifier: GPL-3.0-or-later
#include "DecoderIpc.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <cerrno>
#include <cstring>

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

void put(QDataStream& s, const Metadata& m) {
    s << m.groups << qint32(m.entries.size());
    for (const MetadataEntry& e : m.entries)
        s << e.key << e.label << e.value;
    putOptional(s, m.captured);
    putOptional(s, m.latitude);
    putOptional(s, m.longitude);
    s << qint32(m.rating);
}
void get(QDataStream& s, Metadata& m) {
    qint32 n = 0;
    s >> m.groups >> n;
    m.entries.clear();
    for (qint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
        MetadataEntry e;
        s >> e.key >> e.label >> e.value;
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
    // "/lfd.<pid>.<id>": at most 4 + 10 + 1 + 10 = 25 characters (PSHMNAMLEN is 31).
    return "/lfd." + QByteArray::number(pid) + '.' + QByteArray::number(requestId);
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

// Reads exactly `size` bytes; each wait for data is limited to `timeoutMs` (-1 = no limit).
IoStatus readExactly(int fd, char* data, size_t size, int timeoutMs) {
    size_t done = 0;
    while (done < size) {
        IoStatus st = IoStatus::Ok;
        if (timeoutMs >= 0 && !waitReadable(fd, timeoutMs, &st))
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
    unsigned char header[4];
    // The timeout covers the wait for the answer; once it started, the rest follows quickly.
    IoStatus st = readExactly(fd, reinterpret_cast<char*>(header), 4, timeoutMs);
    if (st != IoStatus::Ok)
        return st;
    const quint32 len = quint32(header[0]) | (quint32(header[1]) << 8) | (quint32(header[2]) << 16) |
                        (quint32(header[3]) << 24);
    if (len > kMaxFrameBytes)
        return IoStatus::TooLarge;
    payload->resize(qsizetype(len));
    return readExactly(fd, payload->data(), len, timeoutMs < 0 ? -1 : std::max(timeoutMs, 5000));
}

bool writeSharedImage(const QByteArray& name, const QImage& image, PixelHeader* header, QString* error) {
    const qint64 size = qint64(image.bytesPerLine()) * image.height();
    if (image.isNull() || size <= 0) {
        if (error)
            *error = Tr::tr("Das Bild ist leer.");
        return false;
    }
    const int fd = shm_open(name.constData(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (error)
            *error = Tr::tr("Gemeinsamer Speicher ist nicht verfügbar (%1).")
                         .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    bool ok = ftruncate(fd, off_t(size)) == 0;
    void* mem = ok ? mmap(nullptr, size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    ok = mem != MAP_FAILED;
    if (ok) {
        std::memcpy(mem, image.constBits(), size_t(size));
        munmap(mem, size_t(size));
    }
    close(fd);
    if (!ok) {
        shm_unlink(name.constData());
        if (error)
            *error = Tr::tr("Nicht genug Speicher für das Bild (%1 × %2 Pixel).")
                         .arg(image.width())
                         .arg(image.height());
        return false;
    }
    header->width = image.width();
    header->height = image.height();
    header->format = int(image.format());
    header->bytesPerLine = image.bytesPerLine();
    header->shmSize = size;
    header->iccProfile = image.colorSpace().isValid() ? image.colorSpace().iccProfile() : QByteArray();
    return true;
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

QImage readSharedImage(const QByteArray& name, const PixelHeader& h, QString* error) {
    // Read-write mapping: a QImage on read-only memory deep-copies itself on the first
    // non-const access (e.g. setColorSpace). Nobody else has the object any more.
    const int fd = shm_open(name.constData(), O_RDWR, 0);
    shm_unlink(name.constData()); // the mapping (if any) keeps the memory alive
    if (fd < 0) {
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders fehlen.");
        return {};
    }
    struct stat st{};
    const auto fmt = QImage::Format(h.format);
    const bool sane = fstat(fd, &st) == 0 && h.width > 0 && h.height > 0 &&
                      h.format > QImage::Format_Invalid && h.format < QImage::NImageFormats &&
                      h.bytesPerLine > 0 && h.shmSize == h.bytesPerLine * h.height &&
                      st.st_size >= h.shmSize &&
                      h.bytesPerLine >= qint64(h.width) * QImage::toPixelFormat(fmt).bitsPerPixel() / 8;
    void* mem =
        sane ? mmap(nullptr, size_t(h.shmSize), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (mem == MAP_FAILED) {
        if (error)
            *error = Tr::tr("Die Bilddaten des Decoders sind ungültig.");
        return {};
    }
    auto* m = new Mapping{mem, size_t(h.shmSize)};
    QImage img(static_cast<uchar*>(mem), h.width, h.height, qsizetype(h.bytesPerLine), fmt, releaseMapping,
               m);
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
bool writeSharedImage(const QByteArray&, const QImage&, PixelHeader*, QString*) {
    return false;
}
QImage readSharedImage(const QByteArray&, const PixelHeader&, QString*) {
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
