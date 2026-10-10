// SPDX-License-Identifier: GPL-3.0-or-later
// Rating storage (F-CULL-02), see RatingStore.h.

#include "RatingStore.h"

#include "ImageFormats.h"
#include "LosslessTransform.h"
#include "Settings.h"
#include "Xmp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMutexLocker>
#include <QPointer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <mutex>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#endif

namespace lf {

namespace {

// Tr::tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::RatingStore)
};

constexpr qint64 kMaxSidecarBytes = 16 * 1024 * 1024;

QString hiddenSibling(const QString& path) {
    const QFileInfo fi(path);
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    return fi.dir().filePath(
        QStringLiteral(".%1.lumaflip-%2.%3").arg(fi.completeBaseName(), id, fi.suffix()));
}

bool writeNew(const QString& path, const QByteArray& data, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write(data) != data.size() || !f.flush()) {
        if (error)
            *error = Tr::tr("„%1“ konnte nicht geschrieben werden (Ordner schreibgeschützt?).")
                         .arg(QFileInfo(path).fileName());
        f.close();
        QFile::remove(path);
        return false;
    }
#ifdef Q_OS_UNIX
    ::fsync(f.handle());
#elif defined(Q_OS_WIN)
    FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(f.handle())));
#endif
    f.close();
    return true;
}

QString joinErrors(const QStringList& errors) {
    return errors.join(QLatin1Char(' '));
}

} // namespace

// ---- pure ----

std::optional<Rating> ratingio::resolve(const RatingOptions& o, const std::optional<Rating>& sidecar,
                                        const std::optional<Rating>& embedded,
                                        const std::optional<Rating>& tags) {
    if (o.sidecar && sidecar)
        return sidecar;
    if (o.embedded && embedded)
        return embedded;
    if (o.finderTags && tags)
        return tags;
    return std::nullopt;
}

// ---- sidecars ----

QString ratingio::existingSidecar(const QString& imagePath, const sidecars::Folder* folder) {
    const QStringList found = sidecars::readable(imagePath, folder);
    return found.isEmpty() ? QString() : found.first();
}

QString ratingio::sidecarForWriting(const QString& imagePath) {
    // RAW+JPEG pair: the Adobe sidecar ("IMG_1.xmp") belongs to the RAW file; the JPEG
    // reads and writes its own ("IMG_1.JPG.xmp") so the two ratings stay apart.
    return sidecars::forWriting(imagePath);
}

std::optional<Rating> ratingio::readSidecar(const QString& imagePath, const sidecars::Folder* folder) {
    const QString path = existingSidecar(imagePath, folder);
    if (path.isEmpty())
        return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxSidecarBytes)
        return std::nullopt;
    return xmp::readRating(xmp::decode(f.readAll()));
}

QString ratingio::writeSidecar(const QString& imagePath, const Rating& r) {
    const QString path = sidecarForWriting(imagePath);
    const QString real = resolvedFilePath(path);
    const bool exists = QFileInfo(real).isFile();
    QString text;
    xmp::Encoding encoding = xmp::Encoding::Utf8;
    if (exists) {
        QFile f(real);
        if (!f.open(QIODevice::ReadOnly))
            return Tr::tr("„%1“ kann nicht gelesen werden.").arg(QFileInfo(path).fileName());
        if (f.size() > kMaxSidecarBytes)
            return Tr::tr("„%1“ ist zu groß.").arg(QFileInfo(path).fileName());
        const QByteArray bytes = f.readAll();
        text = xmp::decode(bytes, &encoding);
        if (text.trimmed().isEmpty())
            text.clear();
    } else if (r.isEmpty()) {
        return {}; // nothing stored, nothing to store
    }
    const std::optional<QString> out = xmp::writeRating(text, r);
    if (!out)
        return Tr::tr("Die XMP-Datei „%1“ ist beschädigt oder unbekannt aufgebaut und wird nicht verändert.")
            .arg(QFileInfo(path).fileName());
    if (exists && *out == text)
        return {};
    QString error;
    const QString tmp = hiddenSibling(real);
    if (!writeNew(tmp, xmp::encode(*out, encoding), &error))
        return error;
    if (exists) {
        // Same safety as saving an image: flushed, attributes/hard links kept, atomic.
        if (!platform::replaceFile(tmp, real, false, &error)) {
            QFile::remove(tmp);
            return error.isEmpty()
                       ? Tr::tr("„%1“ konnte nicht ersetzt werden.").arg(QFileInfo(path).fileName())
                       : error;
        }
        return {};
    }
    QString e = fileops::moveNoReplace(tmp, path);
    if (!e.isEmpty()) {
        QFile::remove(tmp);
        return e;
    }
    return {};
}

// ---- embedded ----

bool ratingio::canEmbed(const QString& path, QString* reason) {
    auto refuse = [reason](const QString& why) {
        if (reason)
            *reason = why;
        return false;
    };
    const QString target = resolvedFilePath(path);
    const QFileInfo fi(target);
    if (!fi.isFile())
        return refuse(Tr::tr("Die Datei existiert nicht mehr."));
    const QString type = platform::imageTypeIdentifier(target);
    const bool dng = type == QLatin1String("com.adobe.raw-image") ||
                     fi.suffix().compare(QLatin1String("dng"), Qt::CaseInsensitive) == 0;
    if (!dng && (platform::isCameraRawType(type) || isRawSuffix(fi.suffix().toLower()) ||
                 isRawSuffix(QFileInfo(path).suffix().toLower())))
        return refuse(Tr::tr("RAW-Dateien werden nie verändert."));
    if (type.isEmpty() || !platform::canEmbedRatingType(type))
        return refuse(Tr::tr("In dieses Format kann die Bewertung nicht sicher geschrieben werden."));
    if (platform::imageCount(target) > 1)
        return refuse(Tr::tr("In animierte oder mehrseitige Dateien wird nicht geschrieben."));
    if (!fi.isWritable() || !QFileInfo(fi.absolutePath()).isWritable())
        return refuse(Tr::tr("Die Datei ist schreibgeschützt."));
    return true;
}

std::optional<Rating> ratingio::readEmbedded(const QString& path) {
    return platform::readEmbeddedRating(path);
}

QString ratingio::writeEmbedded(const QString& path, const Rating& r, bool keepModificationDate,
                                bool* refused) {
    if (refused)
        *refused = false;
    auto refuse = [refused](const QString& why) {
        if (refused)
            *refused = true;
        return why;
    };
    QString reason;
    if (!canEmbed(path, &reason))
        return refuse(reason);
    const QString target = resolvedFilePath(path);
    const FileStamp stamp = fileStamp(target);
    if (!stamp.isValid())
        return Tr::tr("„%1“ kann nicht gelesen werden.").arg(QFileInfo(path).fileName());
    const Rating before = platform::readEmbeddedRating(target).value_or(Rating());
    if (before == r)
        return {};
    const QString tmp = hiddenSibling(target);
    QString error;
    if (!platform::writeEmbeddedRating(target, tmp, r, &error)) {
        QFile::remove(tmp);
        return refuse(error.isEmpty() ? Tr::tr("Die Bewertung ließ sich nicht ins Bild schreiben.") : error);
    }
    // Never silently re-encoded: the compressed JPEG data (and everything behind it) must be
    // unchanged; the rating must read back.
    if (platform::imageTypeIdentifier(target) == QLatin1String("public.jpeg")) {
        QFile a(target), b(tmp);
        const bool same = a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && [&] {
            const QByteArray x = a.readAll(), y = b.readAll();
            const QByteArray sx = jpeg::scanData(x);
            return !sx.isEmpty() && sx == jpeg::scanData(y);
        }();
        if (!same) {
            QFile::remove(tmp);
            return refuse(Tr::tr("Die Bewertung ließ sich nicht ohne Neukodieren ins Bild schreiben."));
        }
    }
    if (platform::readEmbeddedRating(tmp).value_or(Rating()) != r) {
        QFile::remove(tmp);
        return refuse(Tr::tr("Die Bewertung ließ sich nicht ins Bild schreiben."));
    }
    if (fileStamp(target) != stamp) {
        QFile::remove(tmp);
        return Tr::tr("„%1“ wurde inzwischen verändert – Bewertung nicht gespeichert.")
            .arg(QFileInfo(path).fileName());
    }
    if (!platform::replaceFile(tmp, target, keepModificationDate, &error)) {
        QFile::remove(tmp);
        return error.isEmpty() ? Tr::tr("Die Datei konnte nicht ersetzt werden.") : error;
    }
    if (!keepModificationDate) {
        // The metadata copy carries the original's dates over: the file changed now.
        QFile f(target);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    }
    return {};
}

// ---- Finder tags ----

std::optional<Rating> ratingio::readTags(const QString& path, const RatingTagNames& names) {
    QString error;
    const auto raw = platform::readRawTags(path, &error);
    if (!raw)
        return std::nullopt;
    bool found = false;
    const Rating r = rating::fromTags(*raw, names, &found);
    return found ? std::optional<Rating>(r) : std::nullopt;
}

QString ratingio::writeTags(const QString& path, const Rating& r, const RatingTagNames& names) {
    if (!platform::tagsSupported())
        return Tr::tr("Finder-Tags werden auf diesem System nicht unterstützt.");
    QString error;
    const auto raw = platform::readRawTags(path, &error);
    if (!raw)
        return error;
    const QStringList next = rating::applyToTags(*raw, r, names);
    if (next == *raw)
        return {};
    if (!platform::writeRawTags(path, next, &error))
        return error;
    return {};
}

// ---- all stores ----

Rating ratingio::read(const QString& path, const RatingOptions& o, const sidecars::Folder* folder) {
    // Lazy: the first enabled store with a value wins, later ones are not read.
    if (o.sidecar)
        if (auto r = readSidecar(path, folder))
            return *r;
    if (o.embedded) {
        if (auto r = readEmbedded(path))
            return *r;
        // Formats that cannot carry it keep the rating in the sidecar (fallback).
        if (!o.sidecar)
            if (auto r = readSidecar(path, folder))
                return *r;
    }
    if (o.finderTags)
        if (auto r = readTags(path, o.tags))
            return *r;
    return Rating();
}

RatingWriteResult ratingio::write(const QString& path, const Rating& r, const RatingOptions& o) {
    RatingWriteResult res;
    if (!o.sidecar && !o.embedded && !o.finderTags) {
        res.error = Tr::tr("Kein Speicherort für Bewertungen gewählt (Einstellungen → Bewertung).");
        return res;
    }
    QStringList errors;
    bool sidecar = o.sidecar;
    if (o.embedded) {
        bool refused = false;
        const QString e = writeEmbedded(path, r, o.keepModificationDate, &refused);
        if (refused) {
            sidecar = true;
            res.notice = Tr::tr("„%1“: %2 Die Bewertung steht deshalb in der XMP-Zusatzdatei.")
                             .arg(QFileInfo(path).fileName(), e);
        } else if (!e.isEmpty()) {
            errors << e;
        }
    }
    if (sidecar)
        if (const QString e = writeSidecar(path, r); !e.isEmpty())
            errors << e;
    if (o.finderTags)
        if (const QString e = writeTags(path, r, o.tags); !e.isEmpty())
            errors << e;
    res.error = joinErrors(errors);
    return res;
}

QHash<QString, Rating> ratingio::readMany(const QStringList& paths, const RatingOptions& o,
                                          const std::function<bool()>& cancelled) {
    QHash<QString, Rating> out;
    QHash<QString, sidecars::Folder> listings;
    for (const QString& p : paths) {
        if (cancelled && cancelled())
            break;
        const QString folder = QFileInfo(p).absolutePath();
        auto it = listings.find(folder);
        if (it == listings.end())
            it = listings.insert(folder, sidecars::Folder::list(folder));
        out.insert(p, read(p, o, &*it));
    }
    return out;
}

// ---- RatingStore ----

RatingStore& RatingStore::instance() {
    static RatingStore* store = new RatingStore(QCoreApplication::instance());
    return *store;
}

RatingStore::RatingStore(QObject* parent) : QObject(parent) {
    qRegisterMetaType<lf::Rating>("lf::Rating");
}

RatingStore::~RatingStore() = default;

RatingOptions RatingStore::options() const {
    QMutexLocker g(&m_mutex);
    return m_options;
}

void RatingStore::setOptions(const RatingOptions& o) {
    QMutexLocker g(&m_mutex);
    if (m_options == o)
        return;
    m_options = o;
    m_cache.clear();
    ++m_optionsGeneration;
}

void RatingStore::followSettings() {
    auto load = [this] {
        Settings& s = Settings::instance();
        RatingOptions o;
        o.sidecar = s.value(key::RatingStoreSidecar).toBool();
        o.embedded = s.value(key::RatingStoreEmbedded).toBool();
        o.finderTags = s.value(key::RatingStoreFinderTags).toBool();
        o.keepModificationDate = s.value(key::RatingKeepModDate).toBool();
        o.tags.starSymbol = s.value(key::RatingTagStarSymbol).toString();
        o.tags.pick = s.value(key::RatingTagPick).toString();
        o.tags.reject = s.value(key::RatingTagReject).toString();
        o.tags.starColor = s.value(key::RatingTagStarColor).toInt();
        o.tags.pickColor = s.value(key::RatingTagPickColor).toInt();
        o.tags.rejectColor = s.value(key::RatingTagRejectColor).toInt();
        setOptions(o);
    };
    load();
    connect(&Settings::instance(), &Settings::changed, this, [load](const QString& k) {
        if (k.startsWith(QLatin1String("rating/")))
            load();
    });
}

std::optional<Rating> RatingStore::cached(const QString& path) const {
    QMutexLocker g(&m_mutex);
    const auto it = m_cache.constFind(path);
    return it == m_cache.cend() ? std::nullopt : std::optional<Rating>(*it);
}

void RatingStore::store(const QString& path, const Rating& r) {
    QMutexLocker g(&m_mutex);
    m_cache.insert(path, r);
}

void RatingStore::forget(const QString& path) {
    QMutexLocker g(&m_mutex);
    m_cache.remove(path);
}

Rating RatingStore::readNow(const QString& path) {
    const RatingOptions o = options();
    const Rating r = ratingio::read(path, o);
    store(path, r);
    return r;
}

std::function<Rating(const QString&)> RatingStore::reader() {
    // Fresh from the disk (another program may have changed it) with the options of the
    // moment; the cache follows. The store must outlive the readers (instance(): the app).
    RatingStore* self = this;
    return [self](const QString& path) {
        const Rating r = ratingio::read(path, self->options());
        self->store(path, r);
        return r;
    };
}

void RatingStore::request(const QString& path) {
    if (path.isEmpty())
        return;
    const RatingOptions o = options();
    auto* w = new QFutureWatcher<Rating>(this);
    connect(w, &QFutureWatcher<Rating>::finished, this, [this, w, path, o] {
        const Rating r = w->result();
        w->deleteLater();
        if (!(options() == o))
            return; // stale (options changed meanwhile)
        store(path, r);
        emit ratingChanged(path, r);
    });
    w->setFuture(QtConcurrent::run([path, o] { return ratingio::read(path, o); }));
}

void RatingStore::setRating(FileOps& ops, const QString& path, const Rating& r, WriteDone done) {
    changeRating(
        ops, path, [r](const Rating&) { return r; },
        [done = std::move(done)](bool ok, const QString& error, const QString& notice, const Rating&) {
            if (done)
                done(ok, error, notice);
        });
}

void RatingStore::changeRating(FileOps& ops, const QString& path, Change change, ChangeDone done) {
    changeRatings(ops, QStringList{path}, std::move(change), std::move(done));
}

void RatingStore::changeRatings(FileOps& ops, const QStringList& paths, Change change, ChangeDone done) {
    if (paths.isEmpty())
        return;
    struct State {
        std::mutex mutex;
        bool planned = false;
        QVector<Rating> previous, next;
        QString notice;
    };
    auto state = std::make_shared<State>();
    const RatingOptions o = options();
    QPointer<RatingStore> self(this);
    // Writes `values` to the files; on an error the files written so far get `undo` back
    // (all or nothing, best effort).
    auto writeAll = [paths, o](const QVector<Rating>& values, const QVector<Rating>& undo, QString* notice) {
        for (qsizetype i = 0; i < paths.size(); ++i) {
            const RatingWriteResult res = ratingio::write(paths[i], values[i], o);
            if (notice && notice->isEmpty())
                *notice = res.notice;
            if (!res.ok()) {
                for (qsizetype k = i; k >= 0; --k)
                    ratingio::write(paths[k], undo[k], o);
                return res.error;
            }
        }
        return QString();
    };
    FileOps::FileWork apply = [state, paths, change = std::move(change), o, writeAll] {
        std::lock_guard g(state->mutex);
        if (!state->planned) {
            for (const QString& p : paths) {
                const Rating before = ratingio::read(p, o);
                state->previous.append(before);
                state->next.append(change ? change(before) : before);
            }
            state->planned = true;
        }
        state->notice.clear();
        return writeAll(state->next, state->previous, &state->notice);
    };
    FileOps::FileWork revert = [state, writeAll] {
        std::lock_guard g(state->mutex);
        return writeAll(state->previous, state->next, nullptr);
    };
    FileOps::StepNotifier notify = [self, state, paths](bool forward) {
        if (!self)
            return;
        QVector<Rating> values;
        {
            std::lock_guard g(state->mutex);
            values = forward ? state->next : state->previous;
        }
        for (qsizetype i = 0; i < paths.size() && i < values.size(); ++i) {
            self->store(paths[i], values[i]);
            emit self->ratingChanged(paths[i], values[i]);
        }
    };
    ops.runAsync(paths.size() > 1 ? Tr::tr("Bewertungen") : Tr::tr("Bewertung"), apply, revert, notify,
                 [self, state, paths, done = std::move(done)](bool ok, const QString& error) {
                     QString notice;
                     Rating now;
                     {
                         std::lock_guard g(state->mutex);
                         notice = state->notice;
                         const QVector<Rating>& v = ok ? state->next : state->previous;
                         now = v.isEmpty() ? Rating() : v.first();
                     }
                     if (!ok && self)
                         for (const QString& p : paths)
                             self->request(p); // show what is on disk now
                     if (done)
                         done(ok, error, notice, now);
                 });
}

} // namespace lf
