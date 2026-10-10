// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageCanvas.h"

#include "ColorPipeline.h"
#include "core/HdrMapping.h"
#include "core/ImagePatch.h"
#include "core/PhotoOverlays.h"
#include "core/StartupTrace.h"

#include <QColorTransform>
#include <QElapsedTimer>
#include <QFile>
#include <QFuture>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QtConcurrent/QtConcurrentRun>
#include <rhi/qrhi.h>

#include <QFloat16>
#include <QRgbaFloat>
#include <algorithm>
#include <cmath>
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
constexpr int kUboSize = 288; // image.vert/.frag "buf": 18 x vec4 (mat4 = 4)

// Output space of the extended range presentation (EdrSurface): linear Display P3, values
// above 1 are brighter than SDR white.
QColorSpace extendedOutputSpace() {
    return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear);
}

// How a tile is drawn (fillUbo).
enum class PassMode { Widget, Extended, Overlay };
// Per-pass GPU state of a tile: the widget and the extended pass (EdrSurface) render into
// different swapchains whose frames may overlap, so neither writes uniforms the other's
// in-flight frame still reads.
inline int passIndex(PassMode mode) {
    return mode == PassMode::Extended ? 1 : 0;
}

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
    std::unique_ptr<QRhiBuffer> ubo[2]; // [passIndex()]
    // [passIndex()][0] smooth, [1] nearest magnification
    std::unique_ptr<QRhiShaderResourceBindings> srb[2][2];
    bool uploaded = false;

    void resetBindings() {
        for (auto& pass : srb)
            for (auto& s : pass)
                s.reset();
    }
};

struct Entry {
    DecodedImagePtr src;
    QImage image; // upload source (possibly converted)
    bool converting = false;
    bool cpuColor = false; // image was converted to the output space on the CPU
    QColorSpace convertedFor;
    QRhiTexture::Format texFormat = QRhiTexture::RGBA8;
    bool premultiplied = false;
    ColorPipeline color;    // source -> widget output (display profile)
    ColorPipeline colorExt; // source -> extended linear Display P3 (computed on demand)
    bool colorExtValid = false;
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
            t.resetBindings();
            for (auto& u : t.ubo)
                u.reset();
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
    overlays::Params photoOverlays; // F-INFO-07 (off by default)
    float exposure = 1.0f;          // F-COL-05 (linear multiplier)
    float edrHeadroom = 1.0f;       // F-COL-03: target of renderExtended()
    bool extended = false;          // extended range presentation active (EdrSurface)
    std::function<void()> requestExtended;
    int extFrames = 0;
    int extFramesAtSwitch = 0; // extFrames when the extended presentation was switched on
    // Background (base or first checker color, second checker color) in extended linear
    // Display P3, converted from the display space.
    float extBgRgb[3][3] = {}; // [2]: base (clear color)

    // Extended pass resources (render pass of the EDR swapchain).
    std::unique_ptr<QRhiGraphicsPipeline> extPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> extBlendPipeline;
    // UI overlay of the extended pass: the canvas' own buffer (never shared with the view,
    // so neither side's writes detach a full-view copy); only patches travel.
    QImage overlayImage;
    QRect overlayDirty;
    int overlayPatches = 0;     // tests
    ColorPipeline overlayColor; // display space -> extended linear P3
    bool overlayColorValid = false;
    std::unique_ptr<QRhiTexture> overlayTex;
    std::unique_ptr<QRhiTexture> overlayLut;
    std::unique_ptr<QRhiBuffer> overlayUbo;
    std::unique_ptr<QRhiShaderResourceBindings> overlaySrb;

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
        e->colorExtValid = false;
        e->srcLutUploaded = false;
    }

    // Pipeline into the extended output; its source LUT equals e.color's (same source).
    void ensureExtColor(Entry& e) {
        if (e.colorExtValid || !e.ready())
            return;
        const QColorSpace srcCs = e.cpuColor ? output : effectiveSourceColorSpace(e.image);
        e.colorExt = makeColorPipeline(srcCs, extendedOutputSpace());
        e.colorExtValid = true;
        if (e.color.srcLut.isEmpty() && !e.colorExt.srcLut.isEmpty())
            e.srcLutUploaded = false; // the widget pipeline needed none: upload it now
    }

    void requestFrame() {
        if (extended && requestExtended)
            requestExtended();
        else
            q->update();
    }

    // Background colors (display encoded) in extended linear Display P3.
    void updateExtendedBackground() {
        const QColorTransform t = output.transformationToColorSpace(extendedOutputSpace());
        const QColor* src[3] = {checker ? &checker1 : &base, &checker2, &base};
        for (int i = 0; i < 3; ++i) {
            const QRgbaFloat32 o = t.map(
                QRgbaFloat32{float(src[i]->redF()), float(src[i]->greenF()), float(src[i]->blueF()), 1.0f});
            extBgRgb[i][0] = std::max(0.0f, o.r);
            extBgRgb[i][1] = std::max(0.0f, o.g);
            extBgRgb[i][2] = std::max(0.0f, o.b);
        }
        overlayColorValid = false;
    }

    void setOutput(const QColorSpace& cs) {
        const QColorSpace sane = sanitizeOutputColorSpace(cs);
        if (sane == output)
            return;
        output = sane;
        fillDestinationTransfer(dstTransfer, output);
        dstLutDirty = true;
        updateExtendedBackground();
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
        old->colorExt = e.colorExt;
        old->colorExtValid = e.colorExtValid;
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
        if (extended)
            ensureExtColor(e);
        ensureSrcLut(e, rub);
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

    // Uploads the entry's source LUT if one is pending - also for entries that are complete
    // (fallback, fade, stale) when the extended pass needs a LUT the widget pass did not.
    // A newly created LUT texture invalidates the tiles' bindings (they used the dummy).
    void ensureSrcLut(Entry& e, QRhiResourceUpdateBatch* rub) {
        const QVector<float>& lut = !e.color.srcLut.isEmpty()
                                        ? e.color.srcLut
                                        : (e.colorExtValid ? e.colorExt.srcLut : e.color.srcLut);
        if (lut.isEmpty() || e.srcLutUploaded)
            return;
        if (!e.srcLut) {
            e.srcLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
            e.srcLut->create();
            for (Tile& t : e.tiles)
                t.resetBindings();
        }
        uploadLut(e.srcLut.get(), lut, rub);
        e.srcLutUploaded = true;
    }

    void uploadLut(QRhiTexture* tex, const QVector<float>& data, QRhiResourceUpdateBatch* rub) {
        QVector<qfloat16> half(data.size());
        for (qsizetype i = 0; i < data.size(); ++i)
            half[i] = qfloat16(data[i]);
        QRhiTextureSubresourceUploadDescription desc(half.constData(),
                                                     quint32(half.size() * sizeof(qfloat16)));
        rub->uploadTexture(tex, QRhiTextureUploadDescription({0, 0, desc}));
    }

    QRhiShaderResourceBindings* srbFor(Entry& e, Tile& t, PassMode mode) {
        const int pass = passIndex(mode);
        const int idx = nearest ? 1 : 0;
        std::unique_ptr<QRhiBuffer>& ubo = t.ubo[pass];
        std::unique_ptr<QRhiShaderResourceBindings>& srb = t.srb[pass][idx];
        if (!ubo) {
            ubo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
            ubo->create();
        }
        if (!srb) {
            QRhiTexture* src = e.srcLut ? e.srcLut.get() : dummyLut.get();
            srb.reset(rhi->newShaderResourceBindings());
            srb->setBindings(
                {QRhiShaderResourceBinding::uniformBuffer(
                     0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                     ubo.get()),
                 QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                           t.tex.get(), sampler[idx].get()),
                 QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, src,
                                                           sampler[0].get()),
                 QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                           dstLut.get(), sampler[0].get())});
            srb->create();
        }
        return srb.get();
    }

    // Peak of an entry's content (relative to SDR white) after exposure.
    float contentPeak(const Entry& e) const {
        const float headroom = e.src && e.src->headroom > 1.0f ? e.src->headroom : 1.0f;
        return headroom * exposure;
    }

    void fillUbo(const Entry& e, const Tile& t, QSize target, QRhiResourceUpdateBatch* rub,
                 const QTransform& xf, float opacity, PassMode mode = PassMode::Widget) {
        const QSize full = e.src->fullSize.isEmpty() ? e.image.size() : e.src->fullSize;
        const double sx = double(full.width()) / e.image.width();
        const double sy = double(full.height()) / e.image.height();
        const QRectF coreFull(t.core.x() * sx, t.core.y() * sy, t.core.width() * sx, t.core.height() * sy);

        QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix();
        mvp.ortho(0, target.width(), target.height(), 0, -1, 1);
        mvp *= QMatrix4x4(xf);
        mvp.translate(float(coreFull.x()), float(coreFull.y()));
        mvp.scale(float(coreFull.width()), float(coreFull.height()));

        const bool ext = mode == PassMode::Extended;
        const ColorPipeline& color = ext && e.colorExtValid ? e.colorExt : e.color;
        // F-COL-03/05: exposure and tone mapping to the pass's target (SDR: 1).
        const float targetHeadroom = ext ? std::max(1.0f, edrHeadroom) : 1.0f;
        const hdr::ToneCurve curve = hdr::makeToneCurve(contentPeak(e), targetHeadroom);
        fillUboData(color, e.premultiplied, mvp, t, rub, t.ubo[passIndex(mode)].get(), opacity, ext, exposure,
                    targetHeadroom, curve, false, contentPeak(e));
    }

    // Shared by image tiles and the overlay. `ext`: output is extended linear P3.
    void fillUboData(const ColorPipeline& color, bool premultiplied, const QMatrix4x4& mvp, const Tile& t,
                     QRhiResourceUpdateBatch* rub, QRhiBuffer* ubo, float opacity, bool ext,
                     float exposureMul, float maxValue, const hdr::ToneCurve& curve, bool overlay,
                     float peak = 1.0f) {
        QMatrix4x4 cm; // identity; upper 3x3 from the pipeline (row-major input)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cm(r, c) = color.matrix[r * 3 + c];

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
        v[6] = overlay ? 0.0f : 1.0f;
        v[7] = overlay ? 1.0f : 0.0f;
        // transfer. Identity (the widget's fast path, unchanged for SDR content): no color
        // conversion, no exposure and nothing to tone-map.
        const bool identity = color.identity && exposureMul == 1.0f && !curve.active;
        v[8] = float(int(color.srcKind));
        v[9] = color.srcGamma;
        v[10] = float(int(identity || ext ? TransferKind::Linear : dstTransfer.dstKind));
        v[11] = dstTransfer.dstGamma;
        // flags
        v[12] = premultiplied ? 1.0f : 0.0f;
        v[13] = identity ? 1.0f : 0.0f;
        v[14] = checker && !overlay ? 1.0f : 0.0f;
        v[15] = float(std::max(1.0, checkerCell));
        if (ext) {
            for (int i = 0; i < 3; ++i) {
                v[16 + i] = extBgRgb[0][i];
                v[20 + i] = extBgRgb[1][i];
            }
        } else {
            const QColor bg = checker ? checker1 : base;
            v[16] = bg.redF();
            v[17] = bg.greenF();
            v[18] = bg.blueF();
            v[20] = checker2.redF();
            v[21] = checker2.greenF();
            v[22] = checker2.blueF();
        }
        v[19] = 1.0f;
        v[23] = 1.0f;
        // range
        v[24] = exposureMul;
        v[25] = maxValue;
        v[26] = opacity; // output alpha: blended over the previous image during a crossfade
        v[27] = curve.active ? 1.0f : 0.0f;
        // tone (hdr::ToneCurve)
        v[28] = curve.knee;
        v[29] = curve.srcPq;
        v[30] = curve.kneePq;
        v[31] = curve.maxLumPq;
        // F-INFO-07 overlays (zero = off: the shader skips them).
        if (!overlay && photoOverlays.active()) {
            v[32] = photoOverlays.clipping ? 1.0f : 0.0f;
            v[33] = photoOverlays.peaking ? 1.0f : 0.0f;
            v[34] = overlays::highThreshold(photoOverlays, peak);
            v[35] = overlays::lowThreshold(photoOverlays);
            const QColor& pc = photoOverlays.peakingColor;
            // Output encoded (display space), linear for the extended pass.
            const float rgb[3] = {float(pc.redF()), float(pc.greenF()), float(pc.blueF())};
            for (int i = 0; i < 3; ++i)
                v[36 + i] = ext ? overlays::srgbToLinear(rgb[i]) : rgb[i];
            v[39] = overlays::peakingThreshold(photoOverlays.peakingSensitivity);
        }
        rub->updateDynamicBuffer(ubo, 0, kUboSize, data);
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

    void releaseExtendedGpu() {
        extPipeline.reset();
        extBlendPipeline.reset();
        overlaySrb.reset();
        overlayUbo.reset();
        overlayTex.reset();
        overlayLut.reset();
        overlayDirty = overlayImage.rect();
    }

    void releaseAllGpu() {
        for (const auto& e : entries)
            e->releaseGpu();
        releaseExtendedGpu();
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

    std::unique_ptr<QRhiGraphicsPipeline> makePipeline(bool blend, QRhiRenderPassDescriptor* rp,
                                                       int samples) {
        std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
        p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        p->setShaderStages(
            {{QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/image.vert.qsb"))},
             {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/image.frag.qsb"))}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        p->setVertexInputLayout(layout);
        p->setShaderResourceBindings(layoutSrb.get());
        p->setRenderPassDescriptor(rp);
        p->setSampleCount(samples);
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
    }

    // Shared static resources (vertex buffer, samplers, LUTs, layout).
    void ensureStaticResources() {
        if (vbuf)
            return;
        vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
        vbuf->create();
        staticUploaded = false;
        for (int i = 0; i < 2; ++i) {
            const auto mag = i == 1 ? QRhiSampler::Nearest : QRhiSampler::Linear;
            sampler[i].reset(rhi->newSampler(mag, QRhiSampler::Linear, QRhiSampler::Linear,
                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            sampler[i]->create();
        }
        dummyLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
        dummyLut->create();
        dstLut.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
        dstLut->create();
        dstLutDirty = true;
        layoutUbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
        layoutUbo->create();
        layoutSrb.reset(rhi->newShaderResourceBindings());
        layoutSrb->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                 layoutUbo.get()),
             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                       dummyLut.get(), sampler[0].get()),
             QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                       dummyLut.get(), sampler[0].get()),
             QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                       dstLut.get(), sampler[0].get())});
        layoutSrb->create();
    }

    void uploadStatic(QRhiResourceUpdateBatch* rub) {
        if (!staticUploaded) {
            static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
            rub->uploadStaticBuffer(vbuf.get(), quad);
            QVector<float> zero(ColorPipeline::kLutSize * 4, 0.0f);
            uploadLut(dummyLut.get(), zero, rub);
            staticUploaded = true;
        }
        if (dstLutDirty) {
            if (!dstTransfer.dstLut.isEmpty())
                uploadLut(dstLut.get(), dstTransfer.dstLut, rub);
            else
                uploadLut(dstLut.get(), QVector<float>(ColorPipeline::kLutSize * 4, 0.0f), rub);
            dstLutDirty = false;
        }
    }

    // What one frame shows (shared by the widget and the extended pass).
    struct FramePlan {
        Entry* draw = nullptr;
        Entry* fade = nullptr;
        float opacity = 1.0f;
        bool moreWork = false;
    };

    FramePlan planFrame(QRhiResourceUpdateBatch* rub) {
        FramePlan f;
        qint64 budget = kUploadBudgetBytes;
        if (current) {
            upload(*current, rub, budget);
            if (current->complete())
                f.draw = current.get();
            else if (fallback && fallback->complete())
                f.draw = fallback.get();
            else if (current->stale && current->stale->complete())
                f.draw = current->stale.get(); // output changed, re-conversion pending
            else if (current->ready())
                f.draw = current.get(); // partially uploaded huge image: draw what is there
            f.moreWork = !current->complete() && current->ready();
            if (current->complete())
                fallback.reset();
        }
        // Preload neighbours once the current image is on the GPU.
        if (!f.moreWork) {
            for (const auto& e : entries) {
                if (e == current || e->complete() || !e->ready())
                    continue;
                if (budget <= 0) {
                    f.moreWork = true;
                    break;
                }
                upload(*e, rub, budget);
                if (!e->complete())
                    f.moreWork = true;
            }
        }

        // F-SLIDE-02: crossfade from the previous image once the new one is complete.
        if (fadeFrom && (fadeFrom == current || !current || !fadeFrom->complete()))
            fadeFrom.reset();
        if (fadeFrom) {
            if (!current->complete()) {
                f.fade = fadeFrom.get(); // keep the old image until the new one can fade in
                f.draw = nullptr;
            } else {
                if (!fadeClock.isValid())
                    fadeClock.start();
                f.opacity = float(std::clamp(double(fadeClock.elapsed()) / std::max(1, fadeMs), 0.0, 1.0));
                if (f.opacity >= 1.0f) {
                    fadeFrom.reset();
                    fallback.reset();
                    prune();
                } else {
                    f.fade = fadeFrom.get();
                    ++crossfadeFrames;
                    f.moreWork = true;
                }
            }
        }
        return f;
    }

    void prepareDraw(Entry* e, const QTransform& xf, float alpha, QSize target, QRhiResourceUpdateBatch* rub,
                     PassMode mode) {
        if (!e)
            return;
        if (mode == PassMode::Extended)
            ensureExtColor(*e);
        ensureSrcLut(*e, rub); // before srbFor(): a new LUT texture resets the bindings
        for (Tile& t : e->tiles) {
            if (t.uploaded && t.tex) {
                srbFor(*e, t, mode);
                fillUbo(*e, t, target, rub, xf, alpha, mode);
            }
        }
    }

    void drawEntry(QRhiCommandBuffer* cb, Entry* e, QRhiGraphicsPipeline* p, QSize target, PassMode mode) {
        if (!e || !p)
            return;
        cb->setGraphicsPipeline(p);
        cb->setViewport({0, 0, float(target.width()), float(target.height())});
        const QRhiCommandBuffer::VertexInput vin(vbuf.get(), 0);
        for (Tile& t : e->tiles) {
            if (!t.uploaded || !t.tex)
                continue;
            cb->setShaderResources(srbFor(*e, t, mode));
            cb->setVertexInput(0, 1, &vin);
            cb->draw(4);
        }
    }

    // ---- UI overlay of the extended pass ----

    // Uploads the changed part of the overlay; false if there is none.
    bool prepareOverlay(QRhiResourceUpdateBatch* rub, QSize target) {
        if (overlayImage.isNull()) {
            overlaySrb.reset();
            overlayTex.reset();
            return false;
        }
        if (!overlayColorValid) {
            overlayColor = makeColorPipeline(output, extendedOutputSpace());
            overlayColorValid = true;
            if (!overlayColor.srcLut.isEmpty()) {
                if (!overlayLut) {
                    overlayLut.reset(
                        rhi->newTexture(QRhiTexture::RGBA16F, QSize(ColorPipeline::kLutSize, 1)));
                    overlayLut->create();
                    overlaySrb.reset();
                }
                uploadLut(overlayLut.get(), overlayColor.srcLut, rub);
            }
        }
        if (!overlayTex || overlayTex->pixelSize() != overlayImage.size()) {
            overlayTex.reset(rhi->newTexture(QRhiTexture::BGRA8, overlayImage.size()));
            if (!overlayTex->create()) {
                overlayTex.reset();
                return false;
            }
            overlaySrb.reset();
            overlayDirty = overlayImage.rect();
        }
        if (!overlayUbo) {
            overlayUbo.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUboSize));
            overlayUbo->create();
        }
        if (!overlaySrb) {
            overlaySrb.reset(rhi->newShaderResourceBindings());
            overlaySrb->setBindings(
                {QRhiShaderResourceBinding::uniformBuffer(
                     0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                     overlayUbo.get()),
                 QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                           overlayTex.get(), sampler[1].get()),
                 QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                           overlayLut ? overlayLut.get() : dummyLut.get(),
                                                           sampler[0].get()),
                 QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                           dstLut.get(), sampler[0].get())});
            overlaySrb->create();
        }
        const QRect dirty = overlayDirty.intersected(overlayImage.rect());
        if (!dirty.isEmpty()) {
            // Only the changed rows/columns (a blinking text cursor must not re-upload the view),
            // as a copy: the batch must not hold a reference to overlayImage (the next patch
            // would detach the whole buffer).
            QRhiTextureSubresourceUploadDescription desc(overlayImage.copy(dirty));
            desc.setDestinationTopLeft(dirty.topLeft());
            rub->uploadTexture(overlayTex.get(), QRhiTextureUploadDescription({0, 0, desc}));
        }
        overlayDirty = QRect();
        // Whole viewport, 1:1 (the overlay has the canvas' device pixel size).
        QMatrix4x4 mvp = rhi->clipSpaceCorrMatrix();
        mvp.ortho(0, target.width(), target.height(), 0, -1, 1);
        mvp.scale(float(overlayImage.width()), float(overlayImage.height()));
        Tile t;
        t.core = t.padded = overlayImage.rect();
        fillUboData(overlayColor, true, mvp, t, rub, overlayUbo.get(), 1.0f, true, 1.0f, 1.0f,
                    hdr::ToneCurve{}, true);
        return true;
    }
};

ImageCanvas::ImageCanvas(QWidget* parent) : QRhiWidget(parent), d(new Private) {
    d->q = this;
    fillDestinationTransfer(d->dstTransfer, d->output);
    d->updateExtendedBackground();
    setAttribute(Qt::WA_OpaquePaintEvent);
}

ImageCanvas::~ImageCanvas() {
    // GPU objects must go before the QRhi (owned by QRhiWidget) is destroyed.
    emit rhiAboutToBeReleased();
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
    d->requestFrame();
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
    d->requestFrame();
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
    d->requestFrame();
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
    d->requestFrame();
}

void ImageCanvas::setBackground(const QColor& base, bool checker, const QColor& checker1,
                                const QColor& checker2, double checkerCellDevicePx) {
    d->base = base;
    d->checker = checker;
    d->checker1 = checker1;
    d->checker2 = checker2;
    d->checkerCell = checkerCellDevicePx;
    d->updateExtendedBackground();
    d->requestFrame();
}

void ImageCanvas::setUpscaleNearest(bool nearest) {
    if (d->nearest == nearest)
        return;
    d->nearest = nearest;
    d->requestFrame();
}

void ImageCanvas::setOutputColorSpace(const QColorSpace& cs) {
    d->setOutput(cs);
    d->requestFrame();
}

QColorSpace ImageCanvas::outputColorSpace() const {
    return d->output;
}

void ImageCanvas::setHdrMapping(float exposureMultiplier, float extendedHeadroom) {
    const float exposure =
        std::isfinite(exposureMultiplier) && exposureMultiplier > 0 ? exposureMultiplier : 1.0f;
    const float headroom = std::isfinite(extendedHeadroom) ? std::max(1.0f, extendedHeadroom) : 1.0f;
    if (exposure == d->exposure && headroom == d->edrHeadroom)
        return;
    d->exposure = exposure;
    d->edrHeadroom = headroom;
    d->requestFrame();
}

void ImageCanvas::setPhotoOverlays(const overlays::Params& params) {
    if (d->photoOverlays == params)
        return;
    d->photoOverlays = params;
    d->requestFrame();
}

overlays::Params ImageCanvas::photoOverlays() const {
    return d->photoOverlays;
}

float ImageCanvas::exposureMultiplier() const {
    return d->exposure;
}

float ImageCanvas::extendedHeadroom() const {
    return d->edrHeadroom;
}

void ImageCanvas::setExtendedPresentation(bool on, std::function<void()> requestFrame) {
    d->requestExtended = std::move(requestFrame);
    if (d->extended == on)
        return;
    d->extended = on;
    d->extFramesAtSwitch = d->extFrames;
    // Both: the widget clears (extended) or shows the image again; the EDR surface draws.
    update();
    if (on && d->requestExtended)
        d->requestExtended();
}

bool ImageCanvas::extendedPresentation() const {
    return d->extended;
}

void ImageCanvas::setExtendedOverlay(QSize size, const QImage& patch, QPoint at) {
    if (size.isEmpty()) {
        clearExtendedOverlay();
        return;
    }
    if (d->overlayImage.size() != size) {
        d->overlayImage = QImage(size, QImage::Format_ARGB32_Premultiplied);
        if (d->overlayImage.isNull())
            return;
        d->overlayImage.fill(Qt::transparent);
        d->overlayDirty = d->overlayImage.rect();
    }
    const QRect written = copyImagePatch(d->overlayImage, patch, at);
    d->overlayDirty = d->overlayDirty.united(written);
    ++d->overlayPatches;
    d->requestFrame();
}

void ImageCanvas::clearExtendedOverlay() {
    const bool had = !d->overlayImage.isNull();
    d->overlayImage = QImage();
    d->overlayDirty = QRect();
    if (had)
        d->requestFrame();
}

int ImageCanvas::extendedOverlayPatches() const {
    return d->overlayPatches;
}

bool ImageCanvas::overlayBufferShared() const {
    return !d->overlayImage.isNull() && !d->overlayImage.isDetached();
}

QRhi* ImageCanvas::sharedRhi() const {
    return d->rhi;
}

int ImageCanvas::extendedFramesRendered() const {
    return d->extFrames;
}

bool ImageCanvas::hasExtendedOverlay() const {
    return !d->overlayImage.isNull();
}

void ImageCanvas::setMaxTileSize(int px) {
    d->forcedTileSize = px;
    for (const auto& e : d->entries)
        e->releaseGpu();
    d->requestFrame();
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
        if (d->rhi)
            emit rhiAboutToBeReleased();
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
    d->ensureStaticResources();
    d->pipeline = d->makePipeline(false, renderTarget()->renderPassDescriptor(), sampleCount());
    d->blendPipeline = d->makePipeline(true, renderTarget()->renderPassDescriptor(), sampleCount());
}

void ImageCanvas::render(QRhiCommandBuffer* cb) {
    if (d->current)
        trace::once("canvas render begin with image");
    QRhi* rhi = d->rhi;
    QRhiResourceUpdateBatch* rub = rhi->nextResourceUpdateBatch();
    d->uploadStatic(rub);
    if (d->extended && d->requestExtended && d->extFrames > d->extFramesAtSwitch) {
        // Covered by the extended range surface, which draws the image: only clear (no
        // uniform writes that could race with the surface's frames). Until its first frame
        // the widget keeps drawing (SDR preview), so switching never shows an empty view.
        cb->beginPass(renderTarget(), d->base, {1.0f, 0}, rub);
        cb->endPass();
        return;
    }

    const Private::FramePlan f = d->planFrame(rub);
    if (d->current)
        trace::once("canvas uploads queued");
    const QSize target = renderTarget()->pixelSize();
    d->prepareDraw(f.fade, d->fadeView, 1.0f, target, rub, PassMode::Widget);
    d->prepareDraw(f.draw, d->view, f.fade ? f.opacity : 1.0f, target, rub, PassMode::Widget);
    cb->beginPass(renderTarget(), d->base, {1.0f, 0}, rub);
    d->drawEntry(cb, f.fade, d->pipeline.get(), target, PassMode::Widget);
    d->drawEntry(cb, f.draw, f.fade && d->blendPipeline ? d->blendPipeline.get() : d->pipeline.get(), target,
                 PassMode::Widget);
    cb->endPass();
    if (f.draw)
        trace::once("canvas first frame with image recorded");
    else
        trace::once("canvas first frame (empty) recorded");
    if (f.moreWork)
        update();
}

bool ImageCanvas::renderExtended(QRhiCommandBuffer* cb, QRhiRenderTarget* rt) {
    QRhi* rhi = d->rhi;
    if (!rhi || !cb || !rt || !d->vbuf)
        return false;
    QRhiRenderPassDescriptor* rp = rt->renderPassDescriptor();
    if (d->extPipeline && !d->extPipeline->renderPassDescriptor()->isCompatible(rp)) {
        d->extPipeline.reset();
        d->extBlendPipeline.reset();
    }
    if (!d->extPipeline) {
        d->extPipeline = d->makePipeline(false, rp, rt->sampleCount());
        d->extBlendPipeline = d->makePipeline(true, rp, rt->sampleCount());
        if (!d->extPipeline || !d->extBlendPipeline)
            return false;
    }
    QRhiResourceUpdateBatch* rub = rhi->nextResourceUpdateBatch();
    d->uploadStatic(rub);
    const Private::FramePlan f = d->planFrame(rub);
    const QSize target = rt->pixelSize();
    d->prepareDraw(f.fade, d->fadeView, 1.0f, target, rub, PassMode::Extended);
    d->prepareDraw(f.draw, d->view, f.fade ? f.opacity : 1.0f, target, rub, PassMode::Extended);
    const bool overlay = d->prepareOverlay(rub, target);
    const QColor clear = QColor::fromRgbF(d->extBgRgb[2][0], d->extBgRgb[2][1], d->extBgRgb[2][2]);
    cb->beginPass(rt, clear, {1.0f, 0}, rub);
    d->drawEntry(cb, f.fade, d->extPipeline.get(), target, PassMode::Extended);
    d->drawEntry(cb, f.draw, f.fade ? d->extBlendPipeline.get() : d->extPipeline.get(), target,
                 PassMode::Extended);
    if (overlay) {
        cb->setGraphicsPipeline(d->extBlendPipeline.get());
        cb->setViewport({0, 0, float(target.width()), float(target.height())});
        const QRhiCommandBuffer::VertexInput vin(d->vbuf.get(), 0);
        cb->setShaderResources(d->overlaySrb.get());
        cb->setVertexInput(0, 1, &vin);
        cb->draw(4);
    }
    cb->endPass();
    ++d->extFrames;
    if (f.moreWork)
        d->requestFrame();
    return true;
}

void ImageCanvas::releaseResources() {
    emit rhiAboutToBeReleased();
    d->releaseAllGpu();
    d->rhi = nullptr;
}

} // namespace lf
