// SPDX-License-Identifier: GPL-3.0-or-later
// Exporting / converting images (F-EDIT-04), see ImageExport.h.

#include "ImageExport.h"

#include "Decoder.h"
#include "FileOps.h"
#include "LosslessTransform.h"
#include "Platform.h"
#include "VirtualPath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace lf::exporting {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::ImageExport)
};

bool fileExists(const QString& path) {
    return QFileInfo::exists(path);
}

QString sanitizeName(QString name) {
    for (QChar c : {QLatin1Char('/'), QLatin1Char(':'), QLatin1Char('\\')})
        name.replace(c, QLatin1Char('-'));
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1); // no hidden files
    return name;
}

// "name 2.jpg", "name 3.jpg" … not in `taken` and not existing.
QString uniqueName(const QString& folder, const QString& fileName, const QSet<QString>& taken,
                   const std::function<bool(const QString&)>& exists) {
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName(), suffix = fi.suffix();
    for (int n = 2;; ++n) {
        const QString name = suffix.isEmpty() ? QStringLiteral("%1 %2").arg(base).arg(n)
                                              : QStringLiteral("%1 %2.%3").arg(base).arg(n).arg(suffix);
        const QString path = QDir(folder).filePath(name);
        if (!taken.contains(path) && !exists(path))
            return path;
    }
}

} // namespace

QVector<Format> knownFormats() {
    return {
        {QStringLiteral("jpeg"), QStringLiteral("public.jpeg"), QStringLiteral("jpg"), QStringLiteral("JPEG"),
         true, true},
        {QStringLiteral("heic"), QStringLiteral("public.heic"), QStringLiteral("heic"),
         QStringLiteral("HEIC"), true, true},
        {QStringLiteral("avif"), QStringLiteral("public.avif"), QStringLiteral("avif"),
         QStringLiteral("AVIF"), true, false},
        {QStringLiteral("jxl"), QStringLiteral("public.jpeg-xl"), QStringLiteral("jxl"),
         QStringLiteral("JPEG XL"), true, false},
        {QStringLiteral("png"), QStringLiteral("public.png"), QStringLiteral("png"), QStringLiteral("PNG"),
         false, false},
        {QStringLiteral("tiff"), QStringLiteral("public.tiff"), QStringLiteral("tif"), QStringLiteral("TIFF"),
         false, false},
    };
}

QVector<Format> availableFormats(const QStringList& encoderTypes) {
    QVector<Format> out;
    for (const Format& f : knownFormats())
        if (encoderTypes.contains(f.typeId))
            out << f;
    return out;
}

QVector<Format> availableFormats() {
    static const QStringList types = platform::exportTypeIdentifiers();
    return availableFormats(types);
}

Format formatById(const QString& id) {
    for (const Format& f : knownFormats())
        if (f.id == id)
            return f;
    return {};
}

QString metadataModeId(MetadataMode m) {
    switch (m) {
    case MetadataMode::KeepAll:
        return QStringLiteral("all");
    case MetadataMode::RemoveGps:
        return QStringLiteral("noGps");
    case MetadataMode::RemoveAll:
        return QStringLiteral("none");
    }
    return QStringLiteral("all");
}

MetadataMode metadataModeFromId(const QString& id) {
    if (id == QLatin1String("noGps"))
        return MetadataMode::RemoveGps;
    if (id == QLatin1String("none"))
        return MetadataMode::RemoveAll;
    return MetadataMode::KeepAll;
}

QString colorModeId(ColorMode m) {
    switch (m) {
    case ColorMode::Keep:
        return QStringLiteral("keep");
    case ColorMode::SRgb:
        return QStringLiteral("srgb");
    case ColorMode::DisplayP3:
        return QStringLiteral("p3");
    }
    return QStringLiteral("keep");
}

ColorMode colorModeFromId(const QString& id) {
    if (id == QLatin1String("srgb"))
        return ColorMode::SRgb;
    if (id == QLatin1String("p3"))
        return ColorMode::DisplayP3;
    return ColorMode::Keep;
}

QString hdrModeId(HdrMode m) {
    return m == HdrMode::ToneMapSdr ? QStringLiteral("sdr") : QStringLiteral("keep");
}

HdrMode hdrModeFromId(const QString& id) {
    return id == QLatin1String("sdr") ? HdrMode::ToneMapSdr : HdrMode::Keep;
}

QString sourceBaseName(const QString& source) {
    if (const auto parts = vpath::split(source)) {
        const int page = vpath::pdfPageIndex(parts->member);
        if (vpath::containerKind(parts->container) == vpath::ContainerKind::Pdf && page >= 0)
            return QStringLiteral("%1-%2").arg(QFileInfo(parts->container).completeBaseName()).arg(page + 1);
        return QFileInfo(parts->member).completeBaseName();
    }
    return QFileInfo(source).completeBaseName();
}

QString targetFileName(const QString& source, const Options& options, int index, int count) {
    const Format f = formatById(options.formatId);
    QString pattern = options.pattern.trimmed();
    if (pattern.isEmpty())
        pattern = QStringLiteral("{name}-export");
    const int digits = int(QString::number(std::max(1, count)).size());
    QString name = pattern;
    name.replace(QLatin1String("{name}"), sourceBaseName(source));
    name.replace(QLatin1String("{n}"), QStringLiteral("%1").arg(index + 1, digits, 10, QLatin1Char('0')));
    name.replace(QLatin1String("{format}"), f.suffix);
    name = sanitizeName(name);
    if (name.isEmpty())
        name = sanitizeName(sourceBaseName(source) + QStringLiteral("-export"));
    if (name.isEmpty())
        name = QStringLiteral("export");
    return name + QLatin1Char('.') + (f.suffix.isEmpty() ? QStringLiteral("jpg") : f.suffix);
}

QString targetFolder(const QString& source, const Options& options) {
    if (!options.folder.isEmpty())
        return QDir(options.folder).absolutePath();
    return QFileInfo(vpath::containerOf(source)).absolutePath();
}

QVector<Item> planItems(const QStringList& sources, const Options& options,
                        const std::function<bool(const QString&)>& existsFn) {
    const auto exists = existsFn ? existsFn : std::function<bool(const QString&)>(fileExists);
    QVector<Item> items;
    QSet<QString> taken; // targets of earlier items
    QSet<QString> resolvedSources;
    for (const QString& s : sources)
        resolvedSources.insert(vpath::isMember(s) ? vpath::containerOf(s) : resolvedFilePath(s));
    const int count = int(sources.size());
    for (int i = 0; i < count; ++i) {
        Item item;
        item.source = sources.at(i);
        const QString folder = targetFolder(item.source, options);
        QString target = QDir(folder).filePath(targetFileName(item.source, options, i, count));
        // Never a source of this export (also through a symbolic link), never twice the same.
        const QString resolved = exists(target) ? resolvedFilePath(target) : target;
        if (taken.contains(target) || resolvedSources.contains(resolved) || resolvedSources.contains(target))
            target = uniqueName(folder, QFileInfo(target).fileName(), taken + resolvedSources, exists);
        taken.insert(target);
        item.target = target;
        items << item;
    }
    return items;
}

QVector<int> conflicts(const QVector<Item>& items, const std::function<bool(const QString&)>& existsFn) {
    const auto exists = existsFn ? existsFn : std::function<bool(const QString&)>(fileExists);
    QVector<int> out;
    for (int i = 0; i < items.size(); ++i)
        if (exists(items.at(i).target))
            out << i;
    return out;
}

QString exportOne(const QString& source, const QString& target, const Options& options, QString* notice) {
    if (QFileInfo::exists(target))
        return Tr::tr("„%1“ gibt es schon.").arg(QFileInfo(target).fileName());
    const QString tmp = filesafety::siblingTempPath(target);
    QString error;
    bool ok = false;
    if (vpath::isMember(source)) {
        // Archive member or PDF page: decoded pixels (read-only source).
        DecodeRequest req;
        req.hdr = options.hdr == HdrMode::Keep;
        auto img = DecoderRegistry::instance().decode(source, req, &error);
        ok = img && platform::exportDecoded(img->image, tmp, options, &error, notice);
    } else {
        ok = platform::exportImage(source, tmp, options, &error, notice);
    }
    if (!ok) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Das Bild konnte nicht exportiert werden.") : error;
    }
    if (const QString e = fileops::moveNoReplace(tmp, target); !e.isEmpty()) {
        QFile::remove(tmp);
        return e;
    }
    return {};
}

Result run(const QVector<Item>& items, const Options& options, const std::atomic_bool& cancel,
           const Progress& progress, TrashService* trash) {
    Result r;
    QSet<QString> sources;
    for (const Item& i : items)
        sources.insert(vpath::isMember(i.source) ? vpath::containerOf(i.source) : resolvedFilePath(i.source));
    QSet<QString> written;
    const int total = int(items.size());
    for (int n = 0; n < total; ++n) {
        if (cancel.load()) {
            r.cancelled = true;
            break;
        }
        const Item& item = items.at(n);
        if (progress)
            progress(n, total, item.source);
        QString target = item.target;
        const QString folder = QFileInfo(target).absolutePath();
        if (!QDir().mkpath(folder)) {
            ++r.failed;
            r.errors
                << Tr::tr("%1: Der Zielordner kann nicht angelegt werden.").arg(QFileInfo(target).fileName());
            continue;
        }
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
            const bool isSource = sources.contains(resolvedFilePath(target)) || written.contains(target);
            ConflictChoice choice = item.onConflict;
            if (isSource && choice == ConflictChoice::Replace)
                choice = ConflictChoice::KeepBoth; // the source is never replaced
            if (choice == ConflictChoice::Replace && !trash)
                choice = ConflictChoice::KeepBoth;
            if (choice == ConflictChoice::Skip) {
                ++r.skipped;
                continue;
            }
            if (choice == ConflictChoice::KeepBoth) {
                target = QDir(folder).filePath(filebatch::keepBothName(folder, QFileInfo(target).fileName()));
            } else {
                QString error;
                if (!trash->moveToTrash(target, &error)) {
                    ++r.failed;
                    r.errors << QStringLiteral("%1: %2").arg(QFileInfo(target).fileName(), error);
                    continue;
                }
            }
        }
        QString notice;
        const QString e = exportOne(item.source, target, options, &notice);
        if (!e.isEmpty()) {
            ++r.failed;
            r.errors << QStringLiteral("%1: %2").arg(QFileInfo(target).fileName(), e);
            continue;
        }
        if (!notice.isEmpty() && !r.notices.contains(notice))
            r.notices << notice;
        written.insert(target);
        r.outputs << target;
        ++r.written;
    }
    if (progress && !r.cancelled)
        progress(total, total, QString());
    return r;
}

} // namespace lf::exporting
