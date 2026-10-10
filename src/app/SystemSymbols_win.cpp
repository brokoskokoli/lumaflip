// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the SF Symbols names map to glyphs of the system icon font (Segoe Fluent Icons on
// Windows 11, Segoe MDL2 Assets on Windows 10). Unknown names give a null image; callers then
// use their QStyle fallback (SystemSymbols.h).
#include "SystemSymbols.h"

#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QPainter>
#include <QPixmap>

namespace lf {

namespace {

char16_t glyphFor(const QString& name) {
    static const QHash<QString, char16_t> kGlyphs = {
        {QStringLiteral("gearshape"), 0xE713},
        {QStringLiteral("gear"), 0xE713},
        {QStringLiteral("macwindow"), 0xE737},
        {QStringLiteral("rectangle"), 0xE737},
        {QStringLiteral("photo"), 0xE8B9},
        {QStringLiteral("photo.on.rectangle"), 0xE8B9},
        {QStringLiteral("computermouse"), 0xE962},
        {QStringLiteral("keyboard"), 0xE765},
        {QStringLiteral("arrow.left.arrow.right"), 0xE8AB},
        {QStringLiteral("arrow.right"), 0xE72A},
        {QStringLiteral("star"), 0xE734},
        {QStringLiteral("star.fill"), 0xE735},
        {QStringLiteral("doc.richtext"), 0xE8A5},
        {QStringLiteral("doc"), 0xE8A5},
        {QStringLiteral("slider.horizontal.3"), 0xE9E9},
        {QStringLiteral("slider.horizontal.below.rectangle"), 0xE9E9},
    };
    return kGlyphs.value(name, 0);
}

QString iconFontFamily() {
    static const QString family = [] {
        for (const QString& f : {QStringLiteral("Segoe Fluent Icons"), QStringLiteral("Segoe MDL2 Assets")})
            if (QFontDatabase::hasFamily(f))
                return f;
        return QString();
    }();
    return family;
}

} // namespace

QImage systemSymbolMask(const QString& name, int pixelSize) {
    const char16_t glyph = glyphFor(name);
    const QString family = iconFontFamily();
    if (pixelSize <= 0 || !glyph || family.isEmpty())
        return {};
    QImage img(pixelSize, pixelSize, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    QFont font(family);
    font.setPixelSize(qMax(1, int(pixelSize * 0.8)));
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(img.rect(), Qt::AlignCenter, QString(QChar(glyph)));
    p.end();
    return img;
}

QIcon systemSymbolIcon(const QString& name, int size, const QColor& color) {
    QIcon icon;
    for (int scale : {1, 2}) {
        QImage img = systemSymbolMask(name, size * scale);
        if (img.isNull())
            return {};
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), color);
        p.end();
        img.setDevicePixelRatio(scale);
        icon.addPixmap(QPixmap::fromImage(img));
    }
    return icon;
}

} // namespace lf
