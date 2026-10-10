// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the ZIP reader of OpenRaster/Krita (SPEC 4.4 "Rest" formats, run in the
// decoder helper). Every member is listed and read with a size limit.

#include "FuzzTarget.h"
#include "core/ZipReader.h"

#include <QBuffer>
#include <QDataStream>

#include <zlib.h>

using namespace lf;

namespace {

// A minimal ZIP (stored or deflated members) for the seeds.
QByteArray makeZip(const QList<std::pair<QByteArray, QByteArray>>& members, bool compressed) {
    QByteArray out, central;
    auto le16 = [](QByteArray& a, quint16 v) { a.append(char(v & 255)).append(char(v >> 8)); };
    auto le32 = [](QByteArray& a, quint32 v) {
        for (int s = 0; s <= 24; s += 8)
            a.append(char((v >> s) & 255));
    };
    for (const auto& [name, data] : members) {
        QByteArray body = data;
        quint16 method = 0;
        if (compressed && name != "mimetype") {
            z_stream z{};
            deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
            QByteArray buf(int(compressBound(uLong(data.size())) + 64), '\0');
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
            z.avail_in = uInt(data.size());
            z.next_out = reinterpret_cast<Bytef*>(buf.data());
            z.avail_out = uInt(buf.size());
            deflate(&z, Z_FINISH);
            buf.resize(qsizetype(z.total_out));
            deflateEnd(&z);
            body = buf;
            method = 8;
        }
        const quint32 crc =
            quint32(crc32(0, reinterpret_cast<const Bytef*>(data.constData()), uInt(data.size())));
        const quint32 offset = quint32(out.size());
        le32(out, 0x04034b50);
        le16(out, 20);
        le16(out, 0);
        le16(out, method);
        le16(out, 0);
        le16(out, 0);
        le32(out, crc);
        le32(out, quint32(body.size()));
        le32(out, quint32(data.size()));
        le16(out, quint16(name.size()));
        le16(out, 0);
        out += name;
        out += body;
        le32(central, 0x02014b50);
        le16(central, 20);
        le16(central, 20);
        le16(central, 0);
        le16(central, method);
        le16(central, 0);
        le16(central, 0);
        le32(central, crc);
        le32(central, quint32(body.size()));
        le32(central, quint32(data.size()));
        le16(central, quint16(name.size()));
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0);
        le32(central, 0);
        le32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(out.size());
    out += central;
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, quint16(members.size()));
    le16(out, quint16(members.size()));
    le32(out, quint32(central.size()));
    le32(out, cdOffset);
    le16(out, 0);
    return out;
}

} // namespace

QList<QByteArray> lf::fuzz::seeds() {
    const QList<std::pair<QByteArray, QByteArray>> ora = {
        {"mimetype", "image/openraster"},
        {"stack.xml", "<image w=\"2\" h=\"2\"><stack><layer src=\"data/a.png\"/></stack></image>"},
        {"data/a.png", QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, 'p')},
        {"mergedimage.png", QByteArray("hello zip ").repeated(50)}};
    return {makeZip(ora, false), makeZip(ora, true), QByteArray("PK\x05\x06", 4) + QByteArray(18, '\0')};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (8 << 20))
        return 0;
    ZipReader zip(fuzz::bytes(data, size));
    if (!zip.isValid())
        return 0;
    const QStringList names = zip.names();
    for (const QString& n : names) {
        (void)zip.member(n);
        if (const auto content = zip.read(n, 1 << 20); content && content->size() > (1 << 20))
            fuzz::fail("read() returned more than the limit");
    }
    return 0;
}
