// SPDX-License-Identifier: GPL-3.0-or-later
#include "PdfDocument.h"

#include "VirtualPath.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace lf::pdf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PdfDocument)
};

struct PageRef {
    QString file;
    int index = -1;
};

std::optional<PageRef> pageOf(const QString& path) {
    const auto parts = vpath::split(path);
    if (!parts || vpath::containerKind(parts->container) != vpath::ContainerKind::Pdf)
        return std::nullopt;
    const int index = vpath::pdfPageIndex(parts->member);
    if (index < 0)
        return std::nullopt;
    return PageRef{parts->container, index};
}

QString noPage() {
    return Tr::tr("Die Seite gibt es im PDF-Dokument nicht.");
}

} // namespace

QSize fullPixelSize(const QSizeF& points) {
    if (!points.isValid() || points.isEmpty())
        return {};
    double w = points.width() * kFullDpi / 72.0;
    double h = points.height() * kFullDpi / 72.0;
    const double longer = std::max(w, h);
    if (longer > kMaxSide) {
        w *= kMaxSide / longer;
        h *= kMaxSide / longer;
    }
    return QSize(std::max(1, int(std::lround(w))), std::max(1, int(std::lround(h))));
}

QSize targetPixelSize(const QSizeF& points, const DecodeRequest& req) {
    const QSize full = fullPixelSize(points);
    if (full.isEmpty())
        return {};
    double scale = 1.0;
    const int longer = std::max(full.width(), full.height());
    if (req.maxPixelSize > 0 && req.maxPixelSize < longer)
        scale = double(req.maxPixelSize) / longer;
    if (req.maxBytes > 0) {
        const double bytes = double(full.width()) * full.height() * 4.0 * scale * scale;
        if (bytes > double(req.maxBytes))
            scale *= std::sqrt(double(req.maxBytes) / bytes);
    }
    return QSize(std::max(1, int(std::floor(full.width() * scale))),
                 std::max(1, int(std::floor(full.height() * scale))));
}

bool PdfPageDecoder::canDecode(const QString& path, const QByteArray&) const {
    return available() && pageOf(path).has_value();
}

std::optional<ImageInfo> PdfPageDecoder::info(const QString& path, QString* error) {
    const auto page = pageOf(path);
    if (!page) {
        if (error)
            *error = noPage();
        return std::nullopt;
    }
    const QSizeF points = pageSize(page->file, page->index, error);
    if (!points.isValid())
        return std::nullopt;
    ImageInfo info;
    info.pixelSize = info.displaySize = fullPixelSize(points);
    info.formatName = QStringLiteral("PDF");
    info.colorModel = QStringLiteral("RGB");
    return info;
}

std::optional<DecodedImage> PdfPageDecoder::decode(const QString& path, const DecodeRequest& req,
                                                   QString* error) {
    const auto page = pageOf(path);
    if (!page) {
        if (error)
            *error = noPage();
        return std::nullopt;
    }
    return renderPage(page->file, page->index, req, error);
}

Metadata PdfPageDecoder::metadata(const QString& path) {
    Metadata md;
    if (const auto page = pageOf(path)) {
        addMetadata(md, QStringLiteral("file.format"), QStringLiteral("PDF"));
        QString error;
        const QSizeF points = pageSize(page->file, page->index, &error);
        if (points.isValid()) {
            const QSize px = fullPixelSize(points);
            addMetadata(md, QStringLiteral("file.dimensions"),
                        QStringLiteral("%1 × %2").arg(px.width()).arg(px.height()));
        }
    }
    return md;
}

} // namespace lf::pdf
