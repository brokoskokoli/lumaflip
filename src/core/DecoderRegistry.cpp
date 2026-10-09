// SPDX-License-Identifier: GPL-3.0-or-later
#include "Decoder.h"
#include "ImageFormats.h"
#include "Platform.h"
#include "QtImageDecoder.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QtDebug>
#include <algorithm>

namespace lf {


namespace {

constexpr qint64 kHeaderBytes = 64;

QString tr(const char* text) {
    return QCoreApplication::translate("lf::DecoderRegistry", text);
}

QByteArray readHeader(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(kHeaderBytes);
}

// Error for "no backend can decode this file" - distinguishes unreadable files.
QString unsupportedError(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists())
        return tr("Die Datei existiert nicht.");
    if (!fi.isReadable())
        return tr("Die Datei kann nicht gelesen werden.");
    return tr("Das Dateiformat wird nicht unterstützt.");
}

template <typename T, typename Fn>
std::optional<T> tryBackends(const QList<std::shared_ptr<Decoder>>& backends, const QString& path,
                             QString* error, Fn&& fn) {
    if (backends.isEmpty()) {
        if (error)
            *error = unsupportedError(path);
        return std::nullopt;
    }
    QStringList errors;
    for (const auto& d : backends) {
        QString err;
        if (auto r = fn(*d, &err))
            return r;
        errors << QStringLiteral("%1: %2").arg(d->name(), err.isEmpty() ? tr("unbekannter Fehler") : err);
    }
    if (error)
        *error = errors.join(QLatin1Char('\n'));
    return std::nullopt;
}

} // namespace

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
    QList<std::shared_ptr<Decoder>> result;
    if (m_decoders.isEmpty())
        return result;
    const QByteArray header = readHeader(path);
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

std::optional<ImageInfo> DecoderRegistry::info(const QString& path, QString* error) const {
    return tryBackends<ImageInfo>(candidates(path), path, error,
                                  [&](Decoder& d, QString* err) { return d.info(path, err); });
}

std::optional<DecodedImage> DecoderRegistry::decode(const QString& path, const DecodeRequest& req,
                                                    QString* error) const {
    return tryBackends<DecodedImage>(candidates(path), path, error,
                                     [&](Decoder& d, QString* err) -> std::optional<DecodedImage> {
                                         auto r = d.decode(path, req, err);
                                         if (r && r->image.isNull()) { // empty image counts as failure
                                             if (err)
                                                 *err = tr("Leeres Bild");
                                             return std::nullopt;
                                         }
                                         return r;
                                     });
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
        registerPortableDecoders(*r);
        if (r->m_decoders.isEmpty())
            qWarning("lf::DecoderRegistry: no decoder backends registered");
        return r;
    }();
    return *registry;
}

} // namespace lf
