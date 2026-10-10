// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the own "Rest" format decoders (QOI, PCX, PNM/PAM/PFM, SGI, ORA/KRA; SPEC
// 4.4, run in the decoder helper). The first byte chooses the format. Invariants: a decoded
// image has the probed size, no decoder allocates more than kMaxPixels, and each input
// finishes quickly (the driver's -timeout; code review M3 #3/#4).

#include "FuzzTarget.h"
#include "core/SimpleDecoders.h"

#include <cstdio>

using namespace lf;
using simplefmt::Format;

QList<QByteArray> lf::fuzz::seeds() {
    auto tagged = [](Format f, const QByteArray& d) { return QByteArray(1, char(int(f))) + d; };
    QByteArray pcx(128, '\0');
    pcx[0] = 0x0A;
    pcx[1] = 5;
    pcx[2] = 1;
    pcx[3] = 8;
    pcx[8] = 1; // 2 x 2
    pcx[10] = 1;
    pcx[65] = 3;
    pcx[66] = 2;
    pcx.append(QByteArray("\xC2\x10\xC2\x20\xC2\x30\xC2\x40\xC2\x50\xC2\x60", 12));
    QByteArray sgi(512, '\0');
    sgi[0] = 0x01;
    sgi[1] = char(0xDA);
    sgi[3] = 1;
    sgi[5] = 3;
    sgi[7] = 2; // 2 x 2 x 3
    sgi[9] = 2;
    sgi[11] = 3;
    sgi.append(QByteArray(12, '\x7F'));
    QByteArray qoi("qoif");
    qoi.append(QByteArray("\x00\x00\x00\x02\x00\x00\x00\x02\x04\x00", 10));
    qoi.append(QByteArray("\xFE\x10\x20\x30\xC2\x00\x00\x00\x00\x00\x00\x00\x01", 13));
    return {
        tagged(Format::Pnm, "P3\n# c\n2 2\n255\n1 2 3 4 5 6 7 8 9 10 11 12\n"),
        tagged(Format::Pnm, QByteArray("P6 2 1 65535\n") + QByteArray(12, 'a')),
        tagged(Format::Pnm,
               QByteArray("P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n") +
                   QByteArray(8, 'b')),
        tagged(Format::Pnm, "P1\n3 2\n0 1 0 1 1 0\n"),
        tagged(Format::Pnm,
               QByteArray("Pf\n2 1\n-1.0\n") + QByteArray("\x00\x00\x80\x3F\x00\x00\x00\x40", 8)),
        tagged(Format::Pcx, pcx),
        tagged(Format::Sgi, sgi),
        tagged(Format::Qoi, qoi),
    };
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > (4 << 20))
        return 0;
    const QByteArray in = fuzz::bytes(data + 1, size - 1);
    const auto format = Format(data[0] % 6);
    QString error;
    std::optional<simplefmt::Raster> r;
    switch (format) {
    case Format::Qoi:
        r = simplefmt::decodeQoi(in, &error);
        break;
    case Format::Pcx:
        r = simplefmt::decodePcx(in, &error);
        break;
    case Format::Pnm:
        r = simplefmt::decodePnm(in, &error);
        break;
    case Format::Sgi:
        r = simplefmt::decodeSgi(in, &error);
        break;
    case Format::Ora:
    case Format::Kra:
        r = simplefmt::decodeLayeredData(format, in, 64, &error);
        break;
    }
    if (format != Format::Ora && format != Format::Kra) {
        const auto p = simplefmt::probe(format, in);
        if (r) {
            if (!p)
                fuzz::fail("decoded without a valid header");
            if (r->image.size() != p->size)
                fuzz::fail("decoded size differs from the probed size");
        }
    }
    static const bool trace = qEnvironmentVariableIntValue("LF_FUZZ_TRACE") == 1;
    if (trace)
        std::fprintf(stderr, "format %d, %zu bytes: %s\n", int(format), size - 1,
                     r ? "decoded" : qPrintable(error));
    if (r && qint64(r->image.width()) * r->image.height() > simplefmt::kMaxPixels)
        fuzz::fail("image larger than kMaxPixels");
    if (!r && error.isEmpty())
        fuzz::fail("failure without an error text");
    return 0;
}
