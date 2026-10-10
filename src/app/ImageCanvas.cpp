// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageCanvas.h"

#include "ColorPipeline.h"
#include "core/StartupTrace.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFuture>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QtConcurrent/QtConcurrentRun>
#include <rhi/qrhi.h>

#include <QFloat16>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

Q_LOGGING_CATEGORY(lcCanvas, "lf.view.canvas")

namespace lf {

namespace {

// Images up to this many pixels are converted synchronously (a few ms at most).
constexpr qint64 kSyncConvertPixels = 512 * 1024;
// Upload budget per frame; the rest continues in the next frame (P-07/P-08).
constexpr qint64 kUploadBudgetBytes = 192ll * 1024 * 1024;
// Tile padding (texels) so that filtering and mip levels do not show seams.
constexpr int kTilePadding = 64;
constexpr int kUboSize = 256;

QShader loadShader(const QString& name) {
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    qCWarning(lcCanvas) << "Shader not found:" << name;
    return {};
}

struct UploadPlan {
    QImage::Format targetFormat = QImage::Format_Invalid; // Invalid = upload as is
    QRhiTexture::Format texFormat = QRhiTexture::RGBA8;
    bool premultiplied = false;
};

// Texture format for an image that needs no CPU format conversion.
bool directFormat(QImage::Format f, QRhiTexture::Format* tex, bool* premul) {
    *premul = false;
    switch (f) {
    case QImage::Format_RGBA8888_Premultiplied:
        *premul = true;
        [[fallthrough]];
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
        *tex = QRhiTexture::RGBA8;
        return true;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    case QImage::Format_ARGB32_Premultiplied:
        *premul = true;
        [[fallthrough]];
    case QImage::Format_ARGB32:
    case QImage::Format_RGB32:
        *tex = QRhiTexture::BGRA8;
        return true;
#endif
    case QImage::Format_RGBA16FPx4_Premultiplied:
        *premul = true;
        [[fallthrough]];
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBX16FPx4:
        *tex = QRhiTexture::RGBA16F;
        return true;
    default:
        return false;
    }
}

QImage::Format convertedFormat(const QImage& img) {
    switch (img.format()) {
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA32FPx4:
        return QImage::Format_RGBA16FPx4;
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        return QImage::Format_RGBA16FPx4_Premultiplied;
    case QImage::Format_RGBX64:
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_Grayscale16:
        return QImage::Format_RGBX16FPx4;
    default:
        return img.hasAlphaChannel() ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBX8888;
    }
}

// Conversion done off the GUI thread for large images.
QImage convertImage(QImage img, QImage::Format format, bool toOutput, QColorSpace output) {
    if (toOutput) {
        QImage out = img.convertedToColorSpace(output, format);
        if (!out.isNull())
            return out;
        qCWarning(lcCanvas) << "Color conversion failed, showing unmanaged" << img.colorSpace();
        img.setColorSpace(QColorSpace());
    }
    if (img.format() != format)
        img = img.convertToFormat(format);
    return img;
}

struct Tile {
    QRect core;   // loaded-image px
    QRect padded; // loaded-image px (texture contents)
    std::unique_ptr<QRhiTexture> tex;
    std::unique_ptr<QRhiBuffer> ubo;
    std::unique_ptr<QRhiShaderResourceBindings> srb[2]; // [0] smooth, [1] nearest magnification
    bool uploaded = false;
};

struct Entry {
    DecodedImagePtr src;
    QImage image; // upload source (possibly converted)
    bool converting = false;
    bool cpuColor = false; // image was converted to the output space on the CPU
    QColorSpace convertedFor;
    QRhiTexture::Format texFormat = QRhiTexture::RGBA8;
    bool premultiplied = false;
    ColorPipeline color;
    std::unique_ptr<QRhiTexture> srcLut;
    bool srcLutUploaded = false;
    std::vector<Tile> tiles;
    int tileSizeUsed = 0;
    quint64 lastUse = 0;
    // GPU state of the previous conversion (output color space changed while the new
    // CPU conversion runs): drawn instead of nothing until the replacement is uploaded.
    std::shared_ptr<Entry> stale;

    bool ready() const { return !converting && !image.isNull(); }
    bool complete() const {
        if (!ready() || tiles.empty())
            return false;
        for (const Tile& t : tiles)
            if (!t.uploaded)
                return false;
        return true;
    }
    void releaseGpu() {
        stale.reset();
        for (Tile& t : tiles) {
            t.srb[0].reset();
            t.srb[1].reset();
            t.ubo.reset();
            t.tex.reset();
            t.uploaded = false;
        }
        tiles.clear();
        srcLut.reset();
        srcLutUploaded = false;
    }
};

qint64 bytesPerPixel(QRhiTexture::Format f) {
    switch (f) {
    case QRhiTexture::RGBA16F:
        return 8;
    case QRhiTexture::RGBA32F:
        return 16;
    default:
        return 4;
    }
}

} // namespace

struct ImageCanvas::Private {
    ImageCanvas* q = nullptr;
    QRhi* rhi = nullptr;
    bool rhiFailed = false;
    int maxTexture = 0;
    int forcedTileSize = 0;

    std::vector<std::shared_ptr<Entry>> entries;
    std::shared_ptr<Entry> current;
    std::shared_ptr<Entry> fallback; // previous stage of the same file
    std::vector<DecodedImagePtr> preload;
    quint64 useCounter = 0;

    QTransform view;
    QColor base{30, 30, 30};
    bool checker = false;
    QColor checker1{255, 255, 255}, checker2{204, 204, 204};
    double checkerCell = 16;
    bool nearest = false;
    QColorSpace output{QColorSpace::SRgb};
    ColorPipeline dstTransfer; // only the destination TRC part is used
    bool dstLutDirty = true;
    float exposure = 1.0f, maxValue = 1.0f;

    // F-SLIDE-02 crossfade: the previous image (with its transform) fades out while the
    // current one is drawn on top with increasing opacity.
    std::shared_ptr<Entry> fadeFrom;
    QTransform fadeView;
    int fadeMs = 0;
    QElapsedTimer fadeClock; // started when the new image is on the GPU
    int crossfadeFrames = 0;
    int reusedFrames = 0;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiGraphicsPipeline> blendPipeline; // like `pipeline`, with alpha blending
    std::unique_ptr<QRhiShaderResourceBindings> layoutSrb;
    std::unique_ptr<QRhiBuffer> vbuf;
    std::unique_ptr<QRhiSampler> sampler[2];
    std::unique_ptr<QRhiTexture> dummyLut;
    std::unique_ptr<QRhiTexture> dstLut;
    std::unique_ptr<QRhiBuffer> layoutUbo;
    bool staticUploaded = false;

    std::shared_ptr<Entry> findEntry(const DecodedImagePtr& img) const {
        for (const auto& e : entries)
            if (e->src == img)
                return e;
        return nullptr;
    }

    void prepareEntry(const std::shared_ptr<Entry>& e) {
        const QImage& src = e->src->image;
        const QColorSpace srcCs = effectiveSourceColorSpace(src);
        QRhiTexture::Format tex;
        bool premul = false;
        const bool direct = directFormat(src.format(), &tex, &premul);
        const bool cpuColor = needsCpuConversion(srcCs);
        e->cpuColor = cpuColor;
        e->convertedFor = output;
        e->releaseGpu();
        if (direct && !cpuColor) {
            e->image = src;
            e->texFormat = tex;
            e->premultiplied = premul;
            e->converting = false;
            updateColor(e);
            return;
        }
        const QImage::Format fmt = direct ? src.format() : convertedFormat(src);
        directFormat(fmt, &e->texFormat, &e->premultiplied);
        if (qint64(src.width()) * src.height() <= kSyncConvertPixels) {
            e->image = convertImage(src, fmt, cpuColor, output);
            e->converting = false;
            updateColor(e);
            return;
        }
        e->converting = true;
        e->image = QImage();
        std::weak_ptr<Entry> weak = e;
        const QColorSpace out = output;
        QtConcurrent::run([src, fmt, cpuColor, out] {
            return convertImage(src, fmt, cpuColor, out);
        }).then(q, [this, weak, out](QImage converted) {
            auto e = weak.lock();
            if (!e || e->convertedFor != out)
                return; // dropped meanwhile or output changed (a new conversion runs)
            e->image = std::move(converted);
            e->converting = false;
            updateColor(e);
            q->update();
            emit q->conversionFinished();
        });
    }

    void updateColor(const std::shared_ptr<Entry>& e) {
        const QColorSpace srcCs = e->cpuColor ? output : effectiveSourceColorSpace(e->image);
        e->color = makeColorPipeline(srcCs, output);
        e->srcLutUploaded = false;
    }

    void setOutput(const QColorSpace& cs) {
        const QColorSpace sane = sanitizeOutputColorSpace(cs);
        if (sane == output)
            return;
        output = sane;
        fillDestinationTransfer(dstTransfer, output);
        dstLutDirty = true;
        for (const auto& e : entries) {
            if (e->cpuColor) {
                // Keep the uploaded tiles of the old conversion as a fallback: releasing
                // them before the new conversion is ready would show blank frames.
                std::shared_ptr<Entry> old = e->complete() ? detachGpu(*e) : std::move(e->stale);
                prepareEntry(e);
                e->stale = std::move(old);
            } else if (e->ready()) {
                updateColor(e);
            }
        }
    }

    // Moves the drawable state of `e` into a new entry (for drawing until replaced).
    static std::shared_ptr<Entry> detachGpu(Entry& e) {
        auto old = std::make_shared<Entry>();
        old->src = e.src;
        old->image = e.image;
        old->cpuColor = e.cpuColor;
        old->convertedFor = e.convertedFor;
        old->texFormat = e.texFormat;
        old->premultiplied = e.premultiplied;
        old->color = e.color; // pixels were converted for the previous output: shown as is
        old->srcLut = std::move(e.srcLut);
        old->srcLutUploaded = e.srcLutUploaded;
        old->tiles = std::move(e.tiles);
        old->tileSizeUsed = e.tileSizeUsed;
        e.tiles.clear();
        e.srcLutUploaded = false;
        return old;
    }

    int tileLimit() const {
        int limit = maxTexture > 0 ? maxTexture : 4096;
        if (forcedTileSize > 0)
            limit = std::min(limit, forcedTileSize);
        return limit;
    }

    void layoutTiles(Entry& e) {
        const int limit = tileLimit();
        e.tiles.clear();
        e.tileSizeUsed = limit;
        const int w = e.image.width(), h = e.image.height();
        if (w <= limit && h <= limit) {
            Tile t;
            t.core = t.padded = QRect(0, 0, w, h);
            e.tiles.push_back(std::move(t));
            return;
        }
        const int pad = std::min(kTilePadding, limit / 8);
        const int core = limit - 2 * pad;
        for (int y = 0; y < h; y += core) {
            for (int x = 0; x < w; x += core) {
                Tile t;
                t.core = QRect(x, y, std::min(core, w - x), std::min(core, h - y));
                t.padded = t.core.adjusted(-pad, -pad, pad, pad).intersected(QRect(0, 0, w, h));
                e.tiles.push_back(std::move(t));
            }
        }
    }

    // Uploads (part of) an entry. Returns false if the budget ran out.
    bool upload(Entry& e, QRhiResourceUpdateBatch* rub, qint64& budget) {
        if (!e.ready())
            return false;
        if (e.tiles.empty() || e.tileSizeUsed != tileLimit())
            layoutTiles(e);
        if (!e.color.srcLut.isEmpty() && !e.srcLutUploaded) {
            if (!e.srcLut) {
                e.srcLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
                e.srcLut->create();
                for (Tile& t : e.tiles) {
                    t.srb[0].reset();
                    t.srb[1].reset();
                }
            }
            uploadLut(e.srcLut.get(), e.color.srcLut, rub);
            e.srcLutUploaded = true;
        }
        const qint64 bpp = bytesPerPixel(e.texFormat);
        for (Tile& t : e.tiles) {
            if (t.uploaded)
                continue;
            const qint64 bytes = qint64(t.padded.width()) * t.padded.height() * bpp;
            if (budget <= 0)
                return false;
            budget -= bytes;
            if (!t.tex) {
                t.tex.reset(rhi->newTexture(e.texFormat, t.padded.size(), 1,
                                            QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips));
                if (!t.tex->create()) {
                    qCWarning(lcCanvas) << "Texture creation failed" << t.padded.size();
                    t.tex.reset();
                    t.uploaded = true; // do not retry every frame
                    continue;
                }
            }
            // Raw data path: copies only the tile's rows (QImage based sub-uploads
            // would copy the whole image per tile for 32-bit formats).
            const qsizetype bpl = e.image.bytesPerLine();
            const uchar* first =
                e.image.constBits() + qsizetype(t.padded.y()) * bpl + qsizetype(t.padded.x()) * bpp;
            const qsizetype size = qsizetype(t.padded.height() - 1) * bpl + qsizetype(t.padded.width()) * bpp;
            QRhiTextureSubresourceUploadDescription desc(
                QByteArray::fromRawData(reinterpret_cast<const char*>(first), size));
            desc.setDataStride(quint32(bpl));
            rub->uploadTexture(t.tex.get(), QRhiTextureUploadDescription({0, 0, desc}));
            rub->generateMips(t.tex.get());
            t.uploaded = true;
        }
        e.stale.reset(); // replacement fully uploaded
        return true;
    }

    void uploadLut(QRhiTexture* tex, const QVector<float>& data, QRhiResourceUpdateBatch* rub) {
        QVector<qfloat16> half(data.size());
        for (qsizetype i = 0; i < data.size(); ++i)
            half[i] = qfloat16(data[i]);
        QRhiTextureSubresourceUploadDescription desc(half.constData(),
                                                     quint32(half.size() * sizeof(qfloat16)));
        rub->uploadTexture(tex, QRhiTextureUploadDescription({0, 0, desc}));
    }

    QRhiShaderResourceBindings* srbFor(Entry& e, Tile& t) {
        const int idx = nearest ? 1 : 0;
        if (!t.ubo) {
            t.ubo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
            t.ubo->create();
        }
        if (!t.srb[idx]) {
            QRhiTexture* src = e.srcLut ? e.srcLut.get() : dummyLut.get();
            t.srb[idx].reset(rhi->newShaderResourceBindings());
            t.srb[idx]->setBindings(
                {QRhiShaderResourceBinding::uniformBuffer(
                     0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                     t.ubo.get()),
                 QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                           t.tex.get(), sampler[idx].get()),
                 QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, src,
                                                           sampler[0].get()),
                 QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                           dstLut.get(), sampler[0].get())});
            t.srb[idx]->create();
        }
        return t.srb[idx].get();
    }

    void fillUbo(const Entry& e, const Tile& t, QSize target, QRhiResourceUpdateBatch* rub,
                 const QTransform& xf, float opacity) {
        const QSize full = e.src->fullSize.isEmpty() ? e.image.size() : e.src->fullSize;
        const double sx = double(full.width()) / e.image.width();
        const double sy = double(full.height()) / e.image.height();
        const QRectF coreFull(t.core.x() * sx, t.core.y() * sy, t.core.width() * sx, t.core.height() * sy);

        QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix();
        mvp.ortho(0, target.width(), target.height(), 0, -1, 1);
        mvp *= QMatrix4x4(xf);
        mvp.translate(float(coreFull.x()), float(coreFull.y()));
        mvp.scale(float(coreFull.width()), float(coreFull.height()));

        QMatrix4x4 cm; // identity; upper 3x3 from the pipeline (row-major input)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cm(r, c) = e.color.matrix[r * 3 + c];

        float data[kUboSize / 4] = {};
        std::memcpy(data, mvp.constData(), 64);
        std::memcpy(data + 16, cm.constData(), 64);
        float* v = data + 32;
        const QSizeF ps = t.padded.size();
        // uvRect
        v[0] = float((t.core.x() - t.padded.x()) / ps.width());
        v[1] = float((t.core.y() - t.padded.y()) / ps.height());
        v[2] = float(t.core.width() / ps.width());
        v[3] = float(t.core.height() / ps.height());
        // texInfo
        v[4] = float(ps.width());
        v[5] = float(ps.height());
        v[6] = 1.0f;
        // transfer
        const bool identity = e.color.identity && exposure == 1.0f;
        v[8] = float(int(e.color.srcKind));
        v[9] = e.color.srcGamma;
        v[10] = float(int(identity ? TransferKind::Linear : dstTransfer.dstKind));
        v[11] = dstTransfer.dstGamma;
        // flags
        v[12] = e.premultiplied ? 1.0f : 0.0f;
        v[13] = identity ? 1.0f : 0.0f;
        v[14] = checker ? 1.0f : 0.0f;
        v[15] = float(std::max(1.0, checkerCell));
        const QColor bg = checker ? checker1 : base;
        v[16] = bg.redF();
        v[17] = bg.greenF();
        v[18] = bg.blueF();
        v[19] = 1.0f;
        v[20] = checker2.redF();
        v[21] = checker2.greenF();
        v[22] = checker2.blueF();
        v[23] = 1.0f;
        v[24] = exposure;
        v[25] = maxValue;
        v[26] = opacity; // output alpha: blended over the previous image during a crossfade
        rub->updateDynamicBuffer(t.ubo.get(), 0, kUboSize, data);
    }

    // Drops entries that are neither current, fallback nor in the preload list;
    // their GPU memory is released immediately.
    void prune() {
        std::vector<std::shared_ptr<Entry>> keep;
        for (const auto& e : entries) {
            const bool wanted = e == current || e == fallback || e == fadeFrom ||
                                std::find(preload.begin(), preload.end(), e->src) != preload.end();
            if (wanted)
                keep.push_back(e);
            else
                e->releaseGpu();
        }
        entries = std::move(keep);
    }

    void touch(const std::shared_ptr<Entry>& e) {
        if (e)
            e->lastUse = ++useCounter;
    }

    void releaseAllGpu() {
        for (const auto& e : entries)
            e->releaseGpu();
        pipeline.reset();
        blendPipeline.reset();
        layoutSrb.reset();
        layoutUbo.reset();
        vbuf.reset();
        sampler[0].reset();
        sampler[1].reset();
        dummyLut.reset();
        dstLut.reset();
        staticUploaded = false;
        dstLutDirty = true;
    }
};

ImageCanvas::ImageCanvas(QWidget* parent) : QRhiWidget(parent), d(new Private) {
    d->q = this;
    fillDestinationTransfer(d->dstTransfer, d->output);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

ImageCanvas::~ImageCanvas() {
    // GPU objects must go before the QRhi (owned by QRhiWidget) is destroyed.
    d->releaseAllGpu();
    d->entries.clear();
    delete d;
}

void ImageCanvas::setCurrent(DecodedImagePtr image, bool sameFile) {
    if (image && image->image.isNull())
        image.reset();
    if (d->current && d->current->src == image)
        return;
    std::shared_ptr<Entry> prev = d->current;
    d->fallback = (sameFile && prev && prev->complete()) ? prev : (sameFile ? d->fallback : nullptr);
    if (!image) {
        d->current.reset();
        d->fallback.reset();
        d->fadeFrom.reset();
    } else {
        auto e = d->findEntry(image);
        if (!e) {
            e = std::make_shared<Entry>();
            e->src = image;
            d->entries.push_back(e);
            d->prepareEntry(e);
        }
        d->current = e;
        d->touch(e);
    }
    d->prune();
    update();
}

DecodedImagePtr ImageCanvas::current() const {
    return d->current ? d->current->src : nullptr;
}

void ImageCanvas::setPreload(const QList<DecodedImagePtr>& images) {
    d->preload.clear();
    for (const auto& img : images) {
        if (!img || img->image.isNull())
            continue;
        d->preload.push_back(img);
        if (!d->findEntry(img)) {
            auto e = std::make_shared<Entry>();
            e->src = img;
            d->entries.push_back(e);
            d->prepareEntry(e);
        }
    }
    d->prune();
    update();
}

void ImageCanvas::replaceFrame(DecodedImagePtr frame) {
    if (!frame || frame->image.isNull())
        return;
    const std::shared_ptr<Entry> e = d->current;
    QRhiTexture::Format tex = QRhiTexture::RGBA8;
    bool premul = false;
    const bool reusable = e && e->src != frame && e->ready() && !e->cpuColor && e->stale == nullptr &&
                          !e->tiles.empty() && e != d->fadeFrom && e->image.size() == frame->image.size() &&
                          directFormat(frame->image.format(), &tex, &premul) && tex == e->texFormat &&
                          premul == e->premultiplied &&
                          !needsCpuConversion(effectiveSourceColorSpace(frame->image)) &&
                          std::find(d->preload.begin(), d->preload.end(), e->src) == d->preload.end();
    if (!reusable) {
        setCurrent(std::move(frame), true);
        return;
    }
    // Same GPU textures, new contents: only the texel upload (and mips) per frame.
    const bool newColor = e->image.colorSpace() != frame->image.colorSpace();
    e->src = std::move(frame);
    e->image = e->src->image;
    if (newColor)
        d->updateColor(e);
    for (Tile& t : e->tiles)
        t.uploaded = false;
    if (d->fallback == e)
        d->fallback.reset();
    d->touch(e);
    ++d->reusedFrames;
    update();
}

void ImageCanvas::startCrossfade(int ms) {
    if (ms <= 0 || !d->current || !d->current->complete())
        return; // nothing on screen to fade from
    d->fadeFrom = d->current;
    d->fadeView = d->view;
    d->fadeMs = ms;
    d->fadeClock.invalidate();
}

bool ImageCanvas::isCrossfading() const {
    return d->fadeFrom != nullptr;
}

int ImageCanvas::crossfadeFramesRendered() const {
    return d->crossfadeFrames;
}

int ImageCanvas::framesReusingTextures() const {
    return d->reusedFrames;
}

void ImageCanvas::setViewTransform(const QTransform& imageToViewport) {
    if (d->view == imageToViewport)
        return;
    d->view = imageToViewport;
    update();
}

void ImageCanvas::setBackground(const QColor& base, bool checker, const QColor& checker1,
                                const QColor& checker2, double checkerCellDevicePx) {
    d->base = base;
    d->checker = checker;
    d->checker1 = checker1;
    d->checker2 = checker2;
    d->checkerCell = checkerCellDevicePx;
    update();
}

void ImageCanvas::setUpscaleNearest(bool nearest) {
    if (d->nearest == nearest)
        return;
    d->nearest = nearest;
    update();
}

void ImageCanvas::setOutputColorSpace(const QColorSpace& cs) {
    d->setOutput(cs);
    update();
}

QColorSpace ImageCanvas::outputColorSpace() const {
    return d->output;
}

void ImageCanvas::setExposure(float multiplier, float maxValue) {
    d->exposure = multiplier;
    d->maxValue = maxValue;
    update();
}

void ImageCanvas::setMaxTileSize(int px) {
    d->forcedTileSize = px;
    for (const auto& e : d->entries)
        e->releaseGpu();
    update();
}

int ImageCanvas::maxTextureSize() const {
    return d->maxTexture;
}

bool ImageCanvas::isRhiAvailable() const {
    return !d->rhiFailed;
}

bool ImageCanvas::isIdle() const {
    if (!d->current)
        return true;
    return d->current->complete();
}

int ImageCanvas::currentTileCount() const {
    return d->current ? int(d->current->tiles.size()) : 0;
}

qint64 ImageCanvas::gpuBytes() const {
    qint64 sum = 0;
    for (const auto& e : d->entries)
        for (const Tile& t : e->tiles)
            if (t.tex)
                sum += qint64(t.padded.width()) * t.padded.height() * bytesPerPixel(e->texFormat) * 4 / 3;
    return sum;
}

void ImageCanvas::initialize(QRhiCommandBuffer*) {
    trace::once("canvas initialize (RHI ready)");
    if (d->rhi != rhi()) {
        d->releaseAllGpu();
        d->rhi = rhi();
    }
    if (!d->rhi) {
        d->rhiFailed = true;
        return;
    }
    d->maxTexture = d->rhi->resourceLimit(QRhi::TextureSizeMax);
    if (d->pipeline &&
        !d->pipeline->renderPassDescriptor()->isCompatible(renderTarget()->renderPassDescriptor())) {
        d->pipeline.reset();
        d->blendPipeline.reset();
    }
    if (d->pipeline)
        return;

    QRhi* rhi = d->rhi;
    if (!d->vbuf) {
        d->vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
        d->vbuf->create();
        d->staticUploaded = false;
        for (int i = 0; i < 2; ++i) {
            const auto mag = i == 1 ? QRhiSampler::Nearest : QRhiSampler::Linear;
            d->sampler[i].reset(rhi->newSampler(mag, QRhiSampler::Linear, QRhiSampler::Linear,
                                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            d->sampler[i]->create();
        }
        d->dummyLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
        d->dummyLut->create();
        d->dstLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
        d->dstLut->create();
        d->dstLutDirty = true;
        d->layoutUbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
        d->layoutUbo->create();
        d->layoutSrb.reset(rhi->newShaderResourceBindings());
        d->layoutSrb->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 d->layoutUbo.get()),
             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                       d->dummyLut.get(), d->sampler[0].get()),
             QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                       d->dummyLut.get(), d->sampler[0].get()),
             QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                       d->dstLut.get(), d->sampler[0].get())});
        d->layoutSrb->create();
    }

    auto makePipeline = [&](bool blend) {
        std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
        p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        p->setShaderStages(
            {{QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/image.vert.qsb"))},
             {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/image.frag.qsb"))}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        p->setVertexInputLayout(layout);
        p->setShaderResourceBindings(d->layoutSrb.get());
        p->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        p->setSampleCount(sampleCount());
        if (blend) {
            // Color: new over old by the opacity in the alpha channel; alpha stays opaque.
            QRhiGraphicsPipeline::TargetBlend b;
            b.enable = true;
            b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            b.srcAlpha = QRhiGraphicsPipeline::Zero;
            b.dstAlpha = QRhiGraphicsPipeline::One;
            p->setTargetBlends({b});
        }
        if (!p->create()) {
            qCWarning(lcCanvas) << "Pipeline creation failed" << (blend ? "(blend)" : "");
            p.reset();
        }
        return p;
    };
    d->pipeline = makePipeline(false);
    d->blendPipeline = makePipeline(true);
}

void ImageCanvas::render(QRhiCommandBuffer* cb) {
    if (d->current)
        trace::once("canvas render begin with image");
    QRhi* rhi = d->rhi;
    QRhiResourceUpdateBatch* rub = rhi->nextResourceUpdateBatch();
    if (!d->staticUploaded) {
        static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        rub->uploadStaticBuffer(d->vbuf.get(), quad);
        QVector<float> zero(ColorPipeline::kLutSize * 4, 0.0f);
        d->uploadLut(d->dummyLut.get(), zero, rub);
        d->staticUploaded = true;
    }
    if (d->dstLutDirty) {
        if (!d->dstTransfer.dstLut.isEmpty())
            d->uploadLut(d->dstLut.get(), d->dstTransfer.dstLut, rub);
        else
            d->uploadLut(d->dstLut.get(), QVector<float>(ColorPipeline::kLutSize * 4, 0.0f), rub);
        d->dstLutDirty = false;
    }

    qint64 budget = kUploadBudgetBytes;
    bool moreWork = false;
    Entry* draw = nullptr;
    if (d->current) {
        d->upload(*d->current, rub, budget);
        if (d->current->complete())
            draw = d->current.get();
        else if (d->fallback && d->fallback->complete())
            draw = d->fallback.get();
        else if (d->current->stale && d->current->stale->complete())
            draw = d->current->stale.get(); // output changed, re-conversion pending
        else if (d->current->ready())
            draw = d->current.get(); // partially uploaded huge image: draw what is there
        moreWork = !d->current->complete() && d->current->ready();
        if (d->current->complete())
            d->fallback.reset();
    }
    // Preload neighbours once the current image is on the GPU.
    if (!moreWork) {
        for (const auto& e : d->entries) {
            if (e == d->current || e->complete() || !e->ready())
                continue;
            if (budget <= 0) {
                moreWork = true;
                break;
            }
            d->upload(*e, rub, budget);
            if (!e->complete())
                moreWork = true;
        }
    }

    // F-SLIDE-02: crossfade from the previous image once the new one is complete.
    Entry* fade = nullptr;
    float opacity = 1.0f;
    if (d->fadeFrom && (d->fadeFrom == d->current || !d->current || !d->fadeFrom->complete()))
        d->fadeFrom.reset();
    if (d->fadeFrom) {
        if (!d->current->complete()) {
            fade = d->fadeFrom.get(); // keep the old image until the new one can fade in
            draw = nullptr;
        } else {
            if (!d->fadeClock.isValid())
                d->fadeClock.start();
            opacity = float(std::clamp(double(d->fadeClock.elapsed()) / std::max(1, d->fadeMs), 0.0, 1.0));
            if (opacity >= 1.0f) {
                d->fadeFrom.reset();
                d->fallback.reset();
                d->prune();
            } else {
                fade = d->fadeFrom.get();
                ++d->crossfadeFrames;
                moreWork = true;
            }
        }
    }

    if (d->current)
        trace::once("canvas uploads queued");
    const QColor clear = d->base;
    const QSize target = renderTarget()->pixelSize();
    auto prepare = [&](Entry* e, const QTransform& xf, float alpha) {
        if (!e || !d->pipeline)
            return;
        for (Tile& t : e->tiles) {
            if (t.uploaded && t.tex) {
                d->srbFor(*e, t);
                d->fillUbo(*e, t, target, rub, xf, alpha);
            }
        }
    };
    prepare(fade, d->fadeView, 1.0f);
    prepare(draw, d->view, fade ? opacity : 1.0f);
    cb->beginPass(renderTarget(), clear, {1.0f, 0}, rub);
    auto drawEntry = [&](Entry* e, QRhiGraphicsPipeline* p) {
        if (!e || !p)
            return;
        cb->setGraphicsPipeline(p);
        cb->setViewport({0, 0, float(target.width()), float(target.height())});
        const QRhiCommandBuffer::VertexInput vin(d->vbuf.get(), 0);
        for (Tile& t : e->tiles) {
            if (!t.uploaded || !t.tex)
                continue;
            cb->setShaderResources(d->srbFor(*e, t));
            cb->setVertexInput(0, 1, &vin);
            cb->draw(4);
        }
    };
    drawEntry(fade, d->pipeline.get());
    drawEntry(draw, fade && d->blendPipeline ? d->blendPipeline.get() : d->pipeline.get());
    cb->endPass();
    if (draw)
        trace::once("canvas first frame with image recorded");
    else
        trace::once("canvas first frame (empty) recorded");
    if (moreWork)
        update();
}

void ImageCanvas::releaseResources() {
    d->releaseAllGpu();
    d->rhi = nullptr;
}

} // namespace lf
