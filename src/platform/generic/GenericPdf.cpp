// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10 (M4) without CoreGraphics (core/PdfDocument.h): PDF pages rendered with QtPdf
// (PDFium) when the build has it (LF_HAVE_QTPDF; CMake finds Qt6::Pdf), else no PDF support.
// Runs in the decoder helper like the macOS renderer: PDFium needs no window system.

#include "core/PdfDocument.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QPainter>

#if LF_HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#endif

namespace lf::pdf {

namespace {
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PdfDocument)
};

#if LF_HAVE_QTPDF
bool open(const QString& path, QPdfDocument& doc, QString* error) {
    const QPdfDocument::Error e = doc.load(path);
    if (e == QPdfDocument::Error::None && doc.status() == QPdfDocument::Status::Ready)
        return true;
    if (error) {
        if (e == QPdfDocument::Error::IncorrectPassword)
            *error = Tr::tr("Das PDF-Dokument ist kennwortgeschützt.");
        else
            *error = QFile::exists(path) ? Tr::tr("Das PDF-Dokument ist beschädigt.")
                                         : Tr::tr("Die Datei existiert nicht.");
    }
    return false;
}

bool checkPage(const QPdfDocument& doc, int index, QString* error) {
    if (index >= 0 && index < doc.pageCount())
        return true;
    if (error)
        *error = Tr::tr("Die Seite gibt es im PDF-Dokument nicht.");
    return false;
}
#else
QString unsupported() {
    return Tr::tr("PDF-Dokumente werden auf diesem System nicht unterstützt.");
}
#endif
} // namespace

bool available() {
    return LF_HAVE_QTPDF != 0;
}

#if LF_HAVE_QTPDF

int pageCount(const QString& path, QString* error) {
    QPdfDocument doc;
    if (!open(path, doc, error))
        return -1;
    return std::min(doc.pageCount(), 100000);
}

QSizeF pageSize(const QString& path, int index, QString* error) {
    QPdfDocument doc;
    if (!open(path, doc, error) || !checkPage(doc, index, error))
        return {};
    return doc.pagePointSize(index);
}

std::optional<DecodedImage> renderPage(const QString& path, int index, const DecodeRequest& req,
                                       QString* error) {
    QPdfDocument doc;
    if (!open(path, doc, error) || !checkPage(doc, index, error))
        return std::nullopt;
    const QSizeF points = doc.pagePointSize(index);
    const QSize full = fullPixelSize(points);
    const QSize target = targetPixelSize(points, req);
    if (target.isEmpty()) {
        if (error)
            *error = Tr::tr("Die Seite ist leer.");
        return std::nullopt;
    }
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Annotations);
    const QImage page = doc.render(index, target, options);
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    if (page.isNull() || image.isNull()) {
        if (error)
            *error = Tr::tr("Nicht genug Speicher für die Seite.");
        return std::nullopt;
    }
    image.fill(Qt::white); // pages are drawn on paper
    {
        QPainter p(&image);
        p.drawImage(QRect(QPoint(0, 0), target), page);
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    DecodedImage out;
    out.image = std::move(image);
    out.fullSize = full;
    out.isPreview = target.width() < full.width() || target.height() < full.height();
    return out;
}

#else

int pageCount(const QString&, QString* error) {
    if (error)
        *error = unsupported();
    return -1;
}

QSizeF pageSize(const QString&, int, QString* error) {
    if (error)
        *error = unsupported();
    return {};
}

std::optional<DecodedImage> renderPage(const QString&, int, const DecodeRequest&, QString* error) {
    if (error)
        *error = unsupported();
    return std::nullopt;
}

#endif

} // namespace lf::pdf
