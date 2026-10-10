// SPDX-License-Identifier: GPL-3.0-or-later
// Build tool (Windows): renders the app icon SVG (packaging/macos/icon/lumaflip.svg) into a
// multi-size .ico for the executable's resource and the installer. Each size is stored as
// PNG (Windows Vista+ reads PNG entries). The macOS SVG draws its body on the macOS icon
// grid (824 x 824 centred on 1024 x 1024); Windows icons use the full square, so the
// margin is cropped.
//
// Usage: lf_make_ico <input.svg> <output.ico>

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <vector>

namespace {

void putU16(QByteArray& out, quint16 v) {
    out.append(char(v & 0xFF));
    out.append(char(v >> 8));
}

void putU32(QByteArray& out, quint32 v) {
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: lf_make_ico <input.svg> <output.ico>\n");
        return 2;
    }
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) {
        std::fprintf(stderr, "lf_make_ico: cannot read %s\n", argv[1]);
        return 1;
    }
    const QRectF full = svg.viewBoxF();
    const qreal margin = full.width() * 100.0 / 1024.0;
    svg.setViewBox(full.adjusted(margin, margin, -margin, -margin));

    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<QByteArray> pngs;
    for (int size : sizes) {
        QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            svg.render(&p, QRectF(0, 0, size, size));
        }
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!img.convertToFormat(QImage::Format_ARGB32).save(&buf, "PNG")) {
            std::fprintf(stderr, "lf_make_ico: PNG encoding failed\n");
            return 1;
        }
        pngs.push_back(png);
    }

    QByteArray ico;
    const int count = int(std::size(sizes));
    putU16(ico, 0);            // reserved
    putU16(ico, 1);            // type: icon
    putU16(ico, quint16(count));
    quint32 offset = 6 + 16 * quint32(count);
    for (int i = 0; i < count; ++i) {
        const int s = sizes[i];
        ico.append(char(s >= 256 ? 0 : s)); // width (0 = 256)
        ico.append(char(s >= 256 ? 0 : s)); // height
        ico.append(char(0));                // palette
        ico.append(char(0));                // reserved
        putU16(ico, 1);                     // planes
        putU16(ico, 32);                    // bits per pixel
        putU32(ico, quint32(pngs[size_t(i)].size()));
        putU32(ico, offset);
        offset += quint32(pngs[size_t(i)].size());
    }
    for (const QByteArray& png : pngs)
        ico.append(png);

    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly) || out.write(ico) != ico.size()) {
        std::fprintf(stderr, "lf_make_ico: cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
