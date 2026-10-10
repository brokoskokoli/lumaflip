// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10 (M4): no PDF renderer on this platform yet (core/PdfDocument.h).

#include "core/PdfDocument.h"

#include <QCoreApplication>

namespace lf::pdf {

namespace {
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PdfDocument)
};
QString unsupported() {
    return Tr::tr("PDF-Dokumente werden auf diesem System nicht unterstützt.");
}
} // namespace

bool available() {
    return false;
}

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

} // namespace lf::pdf
