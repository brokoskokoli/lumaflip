// SPDX-License-Identifier: GPL-3.0-or-later
#include "ResvgDecoder.h"

#include "QtImageDecoder.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#ifndef LF_HAVE_RESVG
#define LF_HAVE_RESVG 0
#endif
#if LF_HAVE_RESVG
#include <resvg.h>
#endif

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::ResvgDecoder)
};

constexpr qint64 kMaxFileBytes = qint64(256) << 20;

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

[[maybe_unused]] QSize fitLongEdge(QSizeF s, int longEdge) {
    const double scale = double(longEdge) / std::max(s.width(), s.height());
    return QSize(std::max(1, int(std::lround(s.width() * scale))),
                 std::max(1, int(std::lround(s.height() * scale))));
}

std::optional<QByteArray> readFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return std::nullopt;
    }
    if (f.size() <= 0 || f.size() > kMaxFileBytes) {
        setError(error,
                 f.size() <= 0 ? Tr::tr("Die Datei ist leer (0 Byte).") : Tr::tr("Die Datei ist zu groß."));
        return std::nullopt;
    }
    QByteArray data = f.readAll();
    if (data.size() != f.size()) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return std::nullopt;
    }
    return data;
}

std::atomic<bool> g_fontsLoaded{false};

#if LF_HAVE_RESVG

Q_LOGGING_CATEGORY(lcResvg, "lf.resvg", QtInfoMsg)

QString errorText(int code) {
    switch (code) {
    case RESVG_ERROR_NOT_AN_UTF8_STR:
    case RESVG_ERROR_PARSING_FAILED:
    case RESVG_ERROR_MALFORMED_GZIP:
        return Tr::tr("Die SVG-Datei ist ungültig oder beschädigt.");
    case RESVG_ERROR_ELEMENTS_LIMIT_REACHED:
        return Tr::tr("Die SVG-Datei enthält zu viele Elemente.");
    case RESVG_ERROR_INVALID_SIZE:
        return Tr::tr("Die SVG-Datei hat keine gültige Größe.");
    case RESVG_ERROR_FILE_OPEN_FAILED:
        return Tr::tr("Die Datei kann nicht gelesen werden.");
    default:
        return Tr::tr("Die SVG-Datei ist ungültig oder beschädigt.");
    }
}

// Text needs the font database (system fonts, loaded once); everything else does not.
bool needsFonts(const QByteArray& data) {
    // SVGZ is compressed: assume text (cannot tell without inflating).
    if (data.startsWith("\x1F\x8B"))
        return true;
    return data.contains("<text") || data.contains(":text") || data.contains("@font-face");
}

class ResvgContext {
public:
    static ResvgContext& instance() {
        static ResvgContext* ctx = new ResvgContext; // process lifetime (workers may render at exit)
        return *ctx;
    }

    // Parses `data` with the shared options (serialised: the options carry the resources
    // directory of the document).
    resvg_render_tree* parse(const QByteArray& data, const QString& baseDir, int* code) {
        std::lock_guard<std::mutex> lock(m_mutex);
        resvg_options* opt = needsFonts(data) ? fontOptions() : m_plain;
        const QByteArray dir = baseDir.isEmpty() ? QByteArray() : QFile::encodeName(baseDir);
        resvg_options_set_resources_dir(opt, dir.isEmpty() ? nullptr : dir.constData());
        resvg_render_tree* tree = nullptr;
        *code = resvg_parse_tree_from_data(data.constData(), uintptr_t(data.size()), opt, &tree);
        resvg_options_set_resources_dir(opt, nullptr);
        if (*code != RESVG_OK) {
            if (tree)
                resvg_tree_destroy(tree);
            return nullptr;
        }
        return tree;
    }

private:
    ResvgContext() : m_plain(resvg_options_create()) {}

    resvg_options* fontOptions() {
        if (!m_fonts) {
            m_fonts = resvg_options_create();
            resvg_options_load_system_fonts(m_fonts);
            resvg_options_set_sans_serif_family(m_fonts, "Helvetica");
            g_fontsLoaded = true;
            qCDebug(lcResvg) << "system fonts loaded";
        }
        return m_fonts;
    }

    std::mutex m_mutex;
    resvg_options* m_plain = nullptr;
    resvg_options* m_fonts = nullptr;
};

struct TreeDeleter {
    void operator()(resvg_render_tree* t) const {
        if (t)
            resvg_tree_destroy(t);
    }
};
using TreePtr = std::unique_ptr<resvg_render_tree, TreeDeleter>;

TreePtr parseTree(const QByteArray& data, const QString& baseDir, QString* error) {
    int code = RESVG_OK;
    TreePtr tree(ResvgContext::instance().parse(data, baseDir, &code));
    if (!tree)
        setError(error, errorText(code));
    return tree;
}

std::optional<QSizeF> treeSize(const resvg_render_tree* tree, QString* error) {
    const resvg_size s = resvg_get_image_size(tree);
    if (!std::isfinite(s.width) || !std::isfinite(s.height) || s.width <= 0 || s.height <= 0) {
        setError(error, Tr::tr("Die SVG-Datei hat keine gültige Größe."));
        return std::nullopt;
    }
    return QSizeF(s.width, s.height);
}

QSize toIntrinsic(QSizeF s) {
    return QSize(std::clamp(int(std::ceil(s.width() - 1e-3)), 1, ResvgDecoder::kMaxEdge * 64),
                 std::clamp(int(std::ceil(s.height() - 1e-3)), 1, ResvgDecoder::kMaxEdge * 64));
}

QImage renderTree(const resvg_render_tree* tree, QSizeF intrinsic, QSize size, QString* error) {
    QImage img(size, QImage::Format_RGBA8888_Premultiplied); // resvg: premultiplied RGBA8
    if (img.isNull()) {
        setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
        return {};
    }
    img.fill(Qt::transparent);
    resvg_transform t = resvg_transform_identity();
    t.a = float(size.width() / intrinsic.width());
    t.d = float(size.height() / intrinsic.height());
    // resvg writes width * height * 4 bytes without padding: QImage rows of RGBA8888 are
    // 4-byte aligned, so bytesPerLine == width * 4.
    Q_ASSERT(img.bytesPerLine() == size.width() * 4);
    resvg_render(tree, t, uint32_t(size.width()), uint32_t(size.height()),
                 reinterpret_cast<char*>(img.bits()));
    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return img;
}

#endif // LF_HAVE_RESVG

[[maybe_unused]] QString unavailableText() {
    return Tr::tr("SVG über resvg ist in diesem Build nicht verfügbar.");
}

// Longer edge for the request: maxPixelSize (or the default) within maxBytes and kMaxEdge.
[[maybe_unused]] int targetEdge(QSizeF intrinsic, const DecodeRequest& req) {
    int edge = req.maxPixelSize > 0 ? req.maxPixelSize : ResvgDecoder::kDefaultLongEdge;
    edge = std::min(edge, ResvgDecoder::kMaxEdge);
    if (req.maxBytes > 0) {
        const double aspect =
            std::min(intrinsic.width(), intrinsic.height()) / std::max(intrinsic.width(), intrinsic.height());
        const double maxPixels = double(req.maxBytes) / 4.0;
        const int byBytes = std::max(1, int(std::floor(std::sqrt(maxPixels / std::max(aspect, 1e-6)))));
        edge = std::min(edge, byBytes);
    }
    return std::max(1, edge);
}

} // namespace

bool resvgAvailable() {
    return LF_HAVE_RESVG != 0;
}

std::shared_ptr<Decoder> createResvgDecoder() {
    if (!resvgAvailable())
        return nullptr;
    return std::make_shared<ResvgDecoder>();
}

bool ResvgDecoder::systemFontsLoaded() {
    return g_fontsLoaded;
}

bool ResvgDecoder::canDecode(const QString& path, const QByteArray& header) const {
    return resvgAvailable() && SvgDecoder().canDecode(path, header);
}

std::optional<QSize> ResvgDecoder::intrinsicSize(const QByteArray& data, QString* error) {
#if LF_HAVE_RESVG
    const TreePtr tree = parseTree(data, QString(), error);
    if (!tree)
        return std::nullopt;
    const auto s = treeSize(tree.get(), error);
    if (!s)
        return std::nullopt;
    return toIntrinsic(*s);
#else
    Q_UNUSED(data);
    setError(error, unavailableText());
    return std::nullopt;
#endif
}

QImage ResvgDecoder::render(const QByteArray& data, const QString& baseDir, QSize size, QString* error) {
#if LF_HAVE_RESVG
    if (size.isEmpty() || size.width() > kMaxEdge || size.height() > kMaxEdge) {
        setError(
            error,
            Tr::tr("Die Bildgröße %1 × %2 ist ungültig oder zu groß.").arg(size.width()).arg(size.height()));
        return {};
    }
    const TreePtr tree = parseTree(data, baseDir, error);
    if (!tree)
        return {};
    const auto s = treeSize(tree.get(), error);
    if (!s)
        return {};
    return renderTree(tree.get(), *s, size, error);
#else
    Q_UNUSED(data);
    Q_UNUSED(baseDir);
    Q_UNUSED(size);
    setError(error, unavailableText());
    return {};
#endif
}

std::optional<ImageInfo> ResvgDecoder::info(const QString& path, QString* error) {
    const auto data = readFile(path, error);
    if (!data)
        return std::nullopt;
    const auto size = intrinsicSize(*data, error);
    if (!size)
        return std::nullopt;
    ImageInfo info;
    info.pixelSize = info.displaySize = *size;
    info.hasAlpha = true;
    info.colorModel = QStringLiteral("RGB");
    info.formatName = QStringLiteral("SVG");
    return info;
}

std::optional<DecodedImage> ResvgDecoder::decode(const QString& path, const DecodeRequest& req,
                                                 QString* error) {
#if LF_HAVE_RESVG
    if (req.page > 0 || req.frame > 0) {
        setError(error, Tr::tr("Seite %1 existiert nicht.").arg(std::max(req.page, req.frame) + 1));
        return std::nullopt;
    }
    const auto data = readFile(path, error);
    if (!data)
        return std::nullopt;
    const TreePtr tree = parseTree(*data, QFileInfo(path).absolutePath(), error);
    if (!tree)
        return std::nullopt;
    const auto intrinsic = treeSize(tree.get(), error);
    if (!intrinsic)
        return std::nullopt;
    const QSize full = fitLongEdge(*intrinsic, kDefaultLongEdge);
    const QSize size = fitLongEdge(*intrinsic, targetEdge(*intrinsic, req));
    QImage img = renderTree(tree.get(), *intrinsic, size, error);
    if (img.isNull())
        return std::nullopt;
    DecodedImage out;
    out.fullSize = full;
    out.isPreview = size.width() < full.width() || size.height() < full.height();
    out.image = std::move(img);
    return out;
#else
    Q_UNUSED(path);
    Q_UNUSED(req);
    setError(error, unavailableText());
    return std::nullopt;
#endif
}

Metadata ResvgDecoder::metadata(const QString& path) {
    Metadata md;
    if (const auto info = this->info(path, nullptr)) {
        addMetadata(md, QStringLiteral("file.format"), info->formatName);
        addMetadata(md, QStringLiteral("file.dimensions"),
                    QStringLiteral("%1 × %2").arg(info->displaySize.width()).arg(info->displaySize.height()));
    }
    return md;
}

} // namespace lf
