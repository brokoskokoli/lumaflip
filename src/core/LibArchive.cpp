// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibArchive.h"

#include <QDateTime>
#include <QFile>
#include <QTimeZone>

#include <algorithm>
#include <cstdint>
#include <sys/types.h>

#if LF_HAVE_LIBARCHIVE && defined(Q_OS_WIN)
// Windows (vcpkg): the real headers - the declarations below use POSIX types, and file
// names go through the wide-character API (the narrow one expects the ANSI code page).
#include <archive.h>
#include <archive_entry.h>
using LaArchive = struct archive;
using LaEntry = struct archive_entry;
using ssize_t = la_ssize_t;
#define LF_ARCHIVE_OPEN_READ(a, path, block)                                                                 \
    archive_read_open_filename_w(a, reinterpret_cast<const wchar_t*>((path).utf16()), block)
#define LF_ARCHIVE_OPEN_WRITE(a, path)                                                                       \
    archive_write_open_filename_w(a, reinterpret_cast<const wchar_t*>((path).utf16()))
#elif LF_HAVE_LIBARCHIVE
// The stable C API of libarchive 3 (archive.h / archive_entry.h). macOS ships the library
// (/usr/lib/libarchive.2.dylib, SDK: usr/lib/libarchive.tbd) but no header, so the few
// functions used here are declared with their documented signatures.
extern "C" {
struct archive;
struct archive_entry;
struct archive* archive_read_new(void);
int archive_read_support_filter_all(struct archive*);
int archive_read_support_format_zip(struct archive*);
int archive_read_support_format_rar(struct archive*);
int archive_read_support_format_rar5(struct archive*);
int archive_read_support_format_7zip(struct archive*);
int archive_read_open_filename(struct archive*, const char* filename, size_t block_size);
int archive_read_next_header(struct archive*, struct archive_entry**);
ssize_t archive_read_data(struct archive*, void*, size_t);
int archive_read_data_skip(struct archive*);
int archive_read_free(struct archive*);
const char* archive_error_string(struct archive*);
const char* archive_entry_pathname(struct archive_entry*);
const char* archive_entry_pathname_utf8(struct archive_entry*);
int64_t archive_entry_size(struct archive_entry*);
int archive_entry_size_is_set(struct archive_entry*);
mode_t archive_entry_filetype(struct archive_entry*);
time_t archive_entry_mtime(struct archive_entry*);
int archive_entry_is_encrypted(struct archive_entry*);
int archive_format(struct archive*);
struct archive* archive_write_new(void);
int archive_write_set_format_by_name(struct archive*, const char* name);
int archive_write_open_filename(struct archive*, const char* file);
struct archive_entry* archive_entry_new(void);
void archive_entry_set_pathname_utf8(struct archive_entry*, const char*);
void archive_entry_set_size(struct archive_entry*, int64_t);
void archive_entry_set_filetype(struct archive_entry*, unsigned int);
void archive_entry_set_perm(struct archive_entry*, mode_t);
void archive_entry_set_mtime(struct archive_entry*, time_t, long);
int archive_write_header(struct archive*, struct archive_entry*);
ssize_t archive_write_data(struct archive*, const void*, size_t);
void archive_entry_free(struct archive_entry*);
int archive_write_close(struct archive*);
int archive_write_free(struct archive*);
}
// lf::archive (Archive.h) hides the C struct names inside namespace lf.
using LaArchive = struct archive;
using LaEntry = struct archive_entry;
#define LF_ARCHIVE_OPEN_READ(a, path, block)                                                                 \
    archive_read_open_filename(a, QFile::encodeName(path).constData(), block)
#define LF_ARCHIVE_OPEN_WRITE(a, path) archive_write_open_filename(a, QFile::encodeName(path).constData())
#endif

namespace lf::libarchive {

#if LF_HAVE_LIBARCHIVE

namespace {

constexpr int kOk = 0;
constexpr int kEof = 1;
constexpr int kWarn = -20;
constexpr unsigned kRegularFile = 0100000;
constexpr size_t kBlockSize = 64 * 1024;
constexpr int kFormatBaseMask = 0xff0000;
constexpr int kFormat7Zip = 0xe0000; // ARCHIVE_FORMAT_7ZIP

struct Reader {
    LaArchive* a = nullptr;
    Reader() { a = archive_read_new(); }
    ~Reader() {
        if (a)
            archive_read_free(a);
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool open(const QString& path) {
        if (!a)
            return false;
        archive_read_support_filter_all(a);
        archive_read_support_format_zip(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);
        archive_read_support_format_7zip(a);
        return LF_ARCHIVE_OPEN_READ(a, path, kBlockSize) == kOk;
    }
};

QString entryName(LaEntry* e) {
    if (const char* u = archive_entry_pathname_utf8(e))
        return QString::fromUtf8(u);
    if (const char* l = archive_entry_pathname(e))
        return QString::fromLocal8Bit(l);
    return {};
}

bool acceptName(const QString& name) {
    if (name.isEmpty() || name.startsWith(u'/'))
        return false;
    for (const QStringView part : QStringView(name).split(u'/'))
        if (part == u"..")
            return false;
    return true;
}

// libarchive could not go on: a password protected archive (encrypted data or headers) gets
// its own message, anything else is damaged.
// Only the error text is asked: archive_read_has_encrypted_entries() is not safe after a
// failed open on every libarchive version (crashed in the Linux CI).
QString failure(LaArchive* a) {
    const QString text = QString::fromUtf8(archive_error_string(a) ? archive_error_string(a) : "");
    if (text.contains(QLatin1String("ncrypt"), Qt::CaseInsensitive) ||
        text.contains(QLatin1String("assphrase"), Qt::CaseInsensitive))
        return archive::messageEncryptedArchive();
    return archive::messageDamagedArchive();
}

// 7z archives are (nearly always) solid: skipping a member decompresses it anyway, and the
// skip of the macOS system libarchive loses its place after large members (the next member
// then fails to read). Their data is read instead.
bool readsToSkip(LaArchive* a) {
    return (archive_format(a) & kFormatBaseMask) == kFormat7Zip;
}

// Passes over the data of an entry that is not wanted, within the scan budget (kMaxScanBytes,
// Archive.h). `listing`: only the headers are needed (7z skips without decompressing then).
// Read here, counting every byte that is produced: 7z data (readsToSkip()) and entries without
// a stated size. Else skipped by libarchive (the next header): ZIP and non-solid RAR seek;
// a solid RAR decompresses, but never more than the stated size (the RAR decoders stop at
// the unpacked size) - so the stated size is what it costs. False: the budget is used up.
bool passOver(LaArchive* a, LaEntry* e, qint64* scanned, qint64 scanLimit, QByteArray& scratch,
              bool listing = false) {
    if (archive_entry_size_is_set(e) && (listing || !readsToSkip(a))) {
        *scanned += std::max<qint64>(0, qint64(archive_entry_size(e)));
        return *scanned <= scanLimit;
    }
    for (;;) {
        const ssize_t n = archive_read_data(a, scratch.data(), size_t(scratch.size()));
        if (n <= 0)
            return true; // end (or damaged data: the next header says so)
        *scanned += n;
        if (*scanned > scanLimit)
            return false;
    }
}

// The data of the current entry, at most maxBytes (whatever size the header states).
std::optional<QByteArray> readData(LaArchive* a, LaEntry* e, qint64 maxBytes, QByteArray& scratch,
                                   QString* error) {
    const qint64 declared = archive_entry_size_is_set(e) ? qint64(archive_entry_size(e)) : -1;
    if (declared > maxBytes) {
        *error = archive::messageMemberTooLarge();
        return std::nullopt;
    }
    QByteArray out;
    out.reserve(qsizetype(declared > 0 ? declared : qint64(kBlockSize)));
    for (;;) {
        const ssize_t n = archive_read_data(a, scratch.data(), size_t(scratch.size()));
        if (n == 0)
            break;
        if (n < 0) {
            *error = failure(a);
            return std::nullopt;
        }
        if (qint64(out.size()) + n > maxBytes) {
            *error = archive::messageMemberTooLarge();
            return std::nullopt;
        }
        out.append(scratch.constData(), qsizetype(n));
    }
    return out;
}

} // namespace

bool available() {
    return true;
}

archive::Listing list(const QString& path, qint64 scanLimit) {
    archive::Listing out;
    Reader r;
    if (!r.open(path)) {
        out.error = !QFile::exists(path) ? archive::messageDamagedArchive()
                    : r.a && failure(r.a) == archive::messageEncryptedArchive()
                        ? archive::messageEncryptedArchive()
                        : archive::messageUnsupportedArchive();
        return out;
    }
    QByteArray scratch(qsizetype(kBlockSize), Qt::Uninitialized);
    LaEntry* e = nullptr;
    int rc = kOk;
    int encrypted = 0;
    qint64 scanned = 0;
    bool budgetUsed = false;
    while ((rc = archive_read_next_header(r.a, &e)) == kOk || rc == kWarn) {
        if (out.members.size() >= archive::kMaxMembers)
            break;
        if (archive_entry_filetype(e) != kRegularFile)
            continue;
        const QString name = entryName(e).normalized(QString::NormalizationForm_C);
        if (!acceptName(name))
            continue;
        if (archive_entry_is_encrypted(e)) {
            ++encrypted;
            continue;
        }
        archive::Member m;
        m.name = name;
        m.size = archive_entry_size_is_set(e) ? qint64(archive_entry_size(e)) : -1;
        if (const time_t t = archive_entry_mtime(e); t > 0)
            m.modified = QDateTime::fromSecsSinceEpoch(qint64(t), QTimeZone::UTC);
        out.members.append(m);
        // The data is not needed for the listing; skipping it (solid RAR archives decompress
        // it) is bounded like read(): members past the budget could not be read anyway.
        if (!passOver(r.a, e, &scanned, scanLimit, scratch, true)) {
            budgetUsed = true;
            break;
        }
    }
    if (out.members.isEmpty() && (encrypted > 0 || (rc != kEof && !budgetUsed))) {
        out.error = encrypted > 0 ? archive::messageEncryptedArchive() : failure(r.a);
        return out;
    }
    out.ok = true; // a damaged tail (or one past the scan budget) still lists the members before it
    return out;
}

std::optional<QVector<archive::Extracted>> readRun(const QString& path, const QString& name, qint64 maxBytes,
                                                   qint64 maxRunBytes, const RunFilter& follow,
                                                   QString* error, qint64 scanLimit) {
    auto fail = [&](const QString& message) -> std::optional<QVector<archive::Extracted>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    Reader r;
    if (!r.open(path))
        return fail(r.a ? failure(r.a) : archive::messageDamagedArchive());
    QByteArray scratch(qsizetype(kBlockSize), Qt::Uninitialized);
    const QString want = name.normalized(QString::NormalizationForm_C);
    QVector<archive::Extracted> run;
    qint64 runBytes = 0;
    qint64 scanned = 0;
    LaEntry* e = nullptr;
    int rc = kOk;
    int seen = 0;
    while ((rc = archive_read_next_header(r.a, &e)) == kOk || rc == kWarn) {
        if (++seen > archive::kMaxMembers)
            break;
        if (archive_entry_filetype(e) != kRegularFile)
            continue;
        const QString entry = entryName(e).normalized(QString::NormalizationForm_C);
        if (run.isEmpty()) {
            if (entry != want) {
                if (!passOver(r.a, e, &scanned, scanLimit, scratch))
                    return fail(archive::messageArchiveTooLarge());
                continue;
            }
            if (archive_entry_is_encrypted(e))
                return fail(archive::messageEncryptedArchive());
            QString readError;
            auto data = readData(r.a, e, maxBytes, scratch, &readError);
            if (!data)
                return fail(readError);
            scanned += data->size();
            runBytes += data->size();
            run.append(archive::Extracted{entry, std::move(*data)});
            if (!follow || runBytes >= maxRunBytes || scanned > scanLimit)
                break;
            continue;
        }
        // F-OPEN-10: the members after the wanted one come along while they are decompressed
        // anyway (solid archives): browsing on does not start from the beginning again.
        const qint64 declared = archive_entry_size_is_set(e) ? qint64(archive_entry_size(e)) : -1;
        if (archive_entry_is_encrypted(e) || !acceptName(entry) || !follow(entry)) {
            if (!passOver(r.a, e, &scanned, scanLimit, scratch))
                break;
            continue;
        }
        if (declared < 0 || runBytes + declared > maxRunBytes || scanned + declared > scanLimit)
            break;
        QString readError;
        auto data = readData(r.a, e, std::min(maxBytes, maxRunBytes - runBytes), scratch, &readError);
        if (!data)
            break; // the wanted member is there; the rest is only a bonus
        scanned += data->size();
        runBytes += data->size();
        run.append(archive::Extracted{entry, std::move(*data)});
        if (runBytes >= maxRunBytes)
            break;
    }
    if (run.isEmpty())
        return fail(rc == kEof || rc == kOk || rc == kWarn ? archive::messageDamagedArchive() : failure(r.a));
    return run;
}

std::optional<QByteArray> read(const QString& path, const QString& name, qint64 maxBytes, QString* error,
                               qint64 scanLimit) {
    auto run = readRun(path, name, maxBytes, 0, {}, error, scanLimit);
    if (!run)
        return std::nullopt;
    return std::move(run->first().data);
}

bool writeArchive(const QString& path, const QString& format, const QList<QPair<QString, QByteArray>>& files,
                  QString* error) {
    LaArchive* a = archive_write_new();
    auto fail = [&](const char* what) {
        if (error)
            *error = QString::fromUtf8(what) + QStringLiteral(": ") +
                     QString::fromUtf8(archive_error_string(a) ? archive_error_string(a) : "");
        archive_write_free(a);
        return false;
    };
    if (archive_write_set_format_by_name(a, format.toUtf8().constData()) != kOk)
        return fail("format");
    if (LF_ARCHIVE_OPEN_WRITE(a, path) != kOk)
        return fail("open");
    for (const auto& [name, data] : files) {
        LaEntry* e = archive_entry_new();
        archive_entry_set_pathname_utf8(e, name.toUtf8().constData());
        archive_entry_set_size(e, data.size());
        archive_entry_set_filetype(e, kRegularFile);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_mtime(e, 1700000000, 0);
        const int rc = archive_write_header(a, e);
        archive_entry_free(e);
        if (rc != kOk)
            return fail("header");
        if (!data.isEmpty() && archive_write_data(a, data.constData(), size_t(data.size())) != data.size())
            return fail("data");
    }
    if (archive_write_close(a) != kOk)
        return fail("close");
    archive_write_free(a);
    return true;
}

#else // !LF_HAVE_LIBARCHIVE

bool available() {
    return false;
}

archive::Listing list(const QString&, qint64) {
    archive::Listing out;
    out.error = archive::messageUnsupportedArchive();
    return out;
}

std::optional<QByteArray> read(const QString&, const QString&, qint64, QString* error, qint64) {
    if (error)
        *error = archive::messageUnsupportedArchive();
    return std::nullopt;
}

std::optional<QVector<archive::Extracted>> readRun(const QString&, const QString&, qint64, qint64,
                                                   const RunFilter&, QString* error, qint64) {
    if (error)
        *error = archive::messageUnsupportedArchive();
    return std::nullopt;
}

bool writeArchive(const QString&, const QString&, const QList<QPair<QString, QByteArray>>&, QString* error) {
    if (error)
        *error = QStringLiteral("libarchive is not available");
    return false;
}

#endif

} // namespace lf::libarchive
