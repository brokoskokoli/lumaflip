// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Writes animated GIFs for tests and benchmarks (F-FMT-03): Qt has no GIF writer and the
// corpus only has a 3-frame animation. Frame i is filled with palette color i % 256.
// Uncompressed LZW (9-bit literal codes, a clear code before the table would grow), so
// no compressor is needed; files are about 9/8 byte per pixel and frame.

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QSize>
#include <QString>

namespace lf::test {

inline QColor gifPaletteColor(int index) {
    return QColor::fromHsv((index * 47) % 360, 200 + (index % 3) * 20, 120 + (index * 13) % 130);
}

inline QByteArray makeAnimatedGif(QSize size, int frames, int delayCentiseconds) {
    QByteArray out("GIF89a");
    auto u16 = [&](int v) { out.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)); };
    u16(size.width());
    u16(size.height());
    out.append(char(0xF7)); // global color table, 8 bits, 256 entries
    out.append(char(0)).append(char(0));
    for (int i = 0; i < 256; ++i) {
        const QColor c = gifPaletteColor(i);
        out.append(char(c.red())).append(char(c.green())).append(char(c.blue()));
    }
    // Loop forever (NETSCAPE2.0).
    out.append("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
    const int pixels = size.width() * size.height();
    for (int f = 0; f < frames; ++f) {
        out.append("\x21\xF9\x04\x00", 4); // graphic control: no disposal, no transparency
        u16(delayCentiseconds);
        out.append(char(0)).append(char(0));
        out.append(char(0x2C));
        u16(0);
        u16(0);
        u16(size.width());
        u16(size.height());
        out.append(char(0));
        out.append(char(8)); // LZW minimum code size
        QByteArray data;
        quint32 acc = 0;
        int bits = 0;
        auto code = [&](int c) {
            acc |= quint32(c) << bits;
            bits += 9;
            while (bits >= 8) {
                data.append(char(acc & 0xFF));
                acc >>= 8;
                bits -= 8;
            }
        };
        const int color = f % 256;
        int sinceClear = 0;
        code(256); // clear
        for (int p = 0; p < pixels; ++p) {
            if (sinceClear == 250) { // before the decoder's table needs 10-bit codes
                code(256);
                sinceClear = 0;
            }
            code(color);
            ++sinceClear;
        }
        code(257); // end of information
        if (bits > 0)
            data.append(char(acc & 0xFF));
        for (qsizetype at = 0; at < data.size(); at += 255) {
            const QByteArray block = data.mid(at, 255);
            out.append(char(block.size()));
            out.append(block);
        }
        out.append(char(0));
    }
    out.append(char(0x3B));
    return out;
}

inline bool writeAnimatedGif(const QString& path, QSize size, int frames, int delayCentiseconds) {
    QFile f(path);
    const QByteArray data = makeAnimatedGif(size, frames, delayCentiseconds);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace lf::test
