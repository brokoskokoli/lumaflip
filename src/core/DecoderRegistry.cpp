// SPDX-License-Identifier: GPL-3.0-or-later
#include "Decoder.h"
#include "ImageFormats.h"
#include "Platform.h"
#include "QtImageDecoder.h"
#include "StartupTrace.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QtDebug>
#include <algorithm>

namespace lf {

namespace {

Q_LOGGING_CATEGORY(lcDecode, "lf.decode")

constexpr qint64 kHeaderBytes = 64;

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::DecoderRegistry)
};

enum class FileState { Readable, Missing, Unreadable };

QByteArray readHeader(const QString& path, FileState* state = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (state)
            *state = QFileInfo::exists(path) ? FileState::Unreadable : FileState::Missing;
        return {};
    }
    if (state)
        *state = FileState::Readable;
    return f.read(kHeaderBytes);
}

// Problems that are known before any backend is asked (F-FMT-02): the file is empty or
// cannot be read. A missing file is left to the backends (tests use virtual paths).
QString precheckError(FileState state, const QByteArray& header) {
    if (state == FileState::Unreadable)
        return DecoderRegistry::messageUnreadable();
    if (state == FileState::Readable && header.isEmpty())
        return DecoderRegistry::messageEmpty();
    return {};
}

// Error for "no backend can decode this file" - distinguishes unreadable files.
QString unsupportedError(FileState state) {
    switch (state) {
    case FileState::Missing:
        return DecoderRegistry::messageMissing();
    case FileState::Unreadable:
        return DecoderRegistry::messageUnreadable();
    case FileState::Readable:
        break;
    }
    return DecoderRegistry::messageUnsupported();
}

// German, user-facing reason after every backend failed; the backend details go to the log.
QString failedError(const QString& path, const QStringList& backendErrors) {
    const QFileInfo fi(path);
    if (!fi.exists())
        return DecoderRegistry::messageMissing();
    // Out of memory is not a defect of the file (both backends say "Speicher").
    for (const QString& e : backendErrors)
        if (e.contains(QStringLiteral("Speicher")))
            return DecoderRegistry::messageTooLarge();
    return DecoderRegistry::messageDamaged();
}

template <typename T, typename Fn>
std::optional<T> tryBackends(const DecoderRegistry& reg, const QString& path, QString* error, QString* detail,
                             Fn&& fn) {
    FileState state = FileState::Readable;
    const QByteArray header = readHeader(path, &state);
    if (const QString pre = precheckError(state, header); !pre.isEmpty()) {
        if (error)
            *error = pre;
        if (detail)
            *detail = pre;
        return std::nullopt;
    }
    const QList<std::shared_ptr<Decoder>> backends = reg.candidates(path, header);
    if (backends.isEmpty()) {
        if (error)
            *error = unsupportedError(state);
        if (detail)
            *detail = error ? *error : unsupportedError(state);
        return std::nullopt;
    }
    QStringList errors;
    QStringList details;
    for (const auto& d : backends) {
        QString err;
        if (auto r = fn(*d, &err))
            return r;
        errors << err;
        details << QStringLiteral("%1: %2").arg(d->name(),
                                                err.isEmpty() ? QStringLiteral("unknown error") : err);
    }
    const QString joined = details.join(QLatin1Char('\n'));
    qCDebug(lcDecode).noquote() << path << joined;
    if (error)
        *error = failedError(path, errors);
    if (detail)
        *detail = joined;
    return std::nullopt;
}

} // namespace

QString DecoderRegistry::messageMissing() {
    return Tr::tr("Die Datei existiert nicht.");
}
QString DecoderRegistry::messageEmpty() {
    return Tr::tr("Die Datei ist leer.");
}
QString DecoderRegistry::messageUnreadable() {
    return Tr::tr("Die Datei kann nicht gelesen werden.");
}
QString DecoderRegistry::messageDamaged() {
    return Tr::tr("Die Datei ist beschädigt oder unvollständig.");
}
QString DecoderRegistry::messageUnsupported() {
    return Tr::tr("Das Dateiformat wird nicht unterstützt.");
}
QString DecoderRegistry::messageTooLarge() {
    return Tr::tr("Das Bild ist zu groß, um es vollständig zu laden.");
}

void DecoderRegistry::add(std::shared_ptr<Decoder> decoder) {
    if (!decoder)
        return;
    // Keep sorted by descending priority; equal priorities keep insertion order.
    const int prio = decoder->priority();
    auto it = std::find_if(m_decoders.begin(), m_decoders.end(),
                           [prio](const auto& d) { return d->priority() < prio; });
    m_decoders.insert(it, std::move(decoder));
}

QList<std::shared_ptr<Decoder>> DecoderRegistry::candidates(const QString& path) const {
    if (m_decoders.isEmpty())
        return {};
    return candidates(path, readHeader(path));
}

QList<std::shared_ptr<Decoder>> DecoderRegistry::candidates(const QString& path,
                                                            const QByteArray& header) const {
    QList<std::shared_ptr<Decoder>> result;
    for (const auto& d : m_decoders) {
        if (d->canDecode(path, header))
            result.append(d);
    }
    return result;
}

bool DecoderRegistry::isSupported(const QString& path) const {
    return !candidates(path).isEmpty();
}

bool DecoderRegistry::isSupportedExtension(const QString& suffixLower) const {
    return m_extensions.contains(suffixLower.toLower());
}

void DecoderRegistry::addSupportedExtensions(const QStringList& suffixesLower) {
    for (const QString& s : suffixesLower)
        m_extensions.insert(s.toLower());
}

QStringList DecoderRegistry::supportedExtensions() const {
    QStringList l(m_extensions.cbegin(), m_extensions.cend());
    l.sort();
    return l;
}

std::optional<ImageInfo> DecoderRegistry::info(const QString& path, QString* error, QString* detail) const {
    return tryBackends<ImageInfo>(*this, path, error, detail,
                                  [&](Decoder& d, QString* err) { return d.info(path, err); });
}

std::optional<DecodedImage> DecoderRegistry::decode(const QString& path, const DecodeRequest& req,
                                                    QString* error, QString* detail) const {
    return tryBackends<DecodedImage>(*this, path, error, detail,
                                     [&](Decoder& d, QString* err) -> std::optional<DecodedImage> {
                                         auto r = d.decode(path, req, err);
                                         if (r && r->image.isNull()) { // empty image counts as failure
                                             if (err)
                                                 *err = Tr::tr("Das Bild ist leer.");
                                             return std::nullopt;
                                         }
                                         return r;
                                     });
}

namespace {
// F-FMT-03 fallback: frames through Decoder::decode() with DecodeRequest::frame.
class GenericFrameReader final : public FrameReader {
public:
    GenericFrameReader(std::shared_ptr<Decoder> decoder, QString path, int frames)
        : m_decoder(std::move(decoder)), m_path(std::move(path)), m_frames(frames) {}
    int frameCount() const override { return m_frames; }
    std::optional<DecodedImage> readFrame(int index, const DecodeRequest& req, QString* error) override {
        DecodeRequest r = req;
        r.frame = index;
        auto img = m_decoder->decode(m_path, r, error);
        if (img && img->image.isNull())
            return std::nullopt;
        if (img)
            img->frame = index;
        return img;
    }

private:
    std::shared_ptr<Decoder> m_decoder;
    QString m_path;
    int m_frames;
};
} // namespace

std::unique_ptr<FrameReader> DecoderRegistry::openFrames(const QString& path) const {
    for (const auto& d : candidates(path)) {
        if (auto r = d->openFrames(path))
            return r;
        QString error;
        if (auto info = d->info(path, &error))
            return std::make_unique<GenericFrameReader>(d, path, std::max(1, info->frameCount));
    }
    return nullptr;
}

Metadata DecoderRegistry::metadata(const QString& path) const {
    // Metadata has no error channel: take the first backend that delivers anything.
    Metadata first;
    bool haveFirst = false;
    for (const auto& d : candidates(path)) {
        Metadata m = d->metadata(path);
        if (!m.groups.isEmpty() || m.captured || m.latitude)
            return m;
        if (!haveFirst) {
            first = std::move(m);
            haveFirst = true;
        }
    }
    return first;
}

DecoderRegistry& DecoderRegistry::instance() {
    // Populated exactly once (thread-safe static init); read-only afterwards, so
    // concurrent use from worker threads needs no locking.
    static DecoderRegistry* registry = [] {
        auto* r = new DecoderRegistry;
#ifndef Q_OS_DARWIN
        // Portable default; on macOS the backends below declare exactly what they decode
        // (and ImageFormats derives its list from this registry).
        r->addSupportedExtensions(defaultImageSuffixes());
#endif
        registerPlatformDecoders(*r);
        registerPortableDecoders(*r); // loads the Qt image plugins (slow, see ImageFormats.cpp)
        trace::mark("decoder registry built");

        if (r->m_decoders.isEmpty())
            qWarning("lf::DecoderRegistry: no decoder backends registered");
        return r;
    }();
    return *registry;
}

} // namespace lf
