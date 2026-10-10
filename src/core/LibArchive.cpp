// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibArchive.h"

#include <QDateTime>
#include <QFile>
#include <QTimeZone>

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

} // namespace

bool available() {
    return true;
}

archive::Listing list(const QString& path) {
    archive::Listing out;
    Reader r;
    if (!r.open(path)) {
        out.error =
            QFile::exists(path) ? archive::messageUnsupportedArchive() : archive::messageDamagedArchive();
        return out;
    }
    LaEntry* e = nullptr;
    int rc = kOk;
    while ((rc = archive_read_next_header(r.a, &e)) == kOk || rc == kWarn) {
        if (out.members.size() >= archive::kMaxMembers)
            break;
        if (archive_entry_filetype(e) != kRegularFile || archive_entry_is_encrypted(e))
            continue;
        const QString name = entryName(e).normalized(QString::NormalizationForm_C);
        if (!acceptName(name))
            continue;
        archive::Member m;
        m.name = name;
        m.size = archive_entry_size_is_set(e) ? qint64(archive_entry_size(e)) : -1;
        if (const time_t t = archive_entry_mtime(e); t > 0)
            m.modified = QDateTime::fromSecsSinceEpoch(qint64(t), QTimeZone::UTC);
        out.members.append(m);
        // The data is not needed for the listing; libarchive skips it (seeks where it can).
    }
    if (rc != kEof && out.members.isEmpty()) {
        out.error = archive::messageDamagedArchive();
        return out;
    }
    out.ok = true; // a damaged tail still lists the members before it
    return out;
}

std::optional<QByteArray> read(const QString& path, const QString& name, qint64 maxBytes, QString* error) {
    auto fail = [&](const QString& message) -> std::optional<QByteArray> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    Reader r;
    if (!r.open(path))
        return fail(archive::messageDamagedArchive());
    const QString want = name.normalized(QString::NormalizationForm_C);
    LaEntry* e = nullptr;
    int rc = kOk;
    int seen = 0;
    while ((rc = archive_read_next_header(r.a, &e)) == kOk || rc == kWarn) {
        if (++seen > archive::kMaxMembers)
            break;
        if (archive_entry_filetype(e) != kRegularFile)
            continue;
        if (entryName(e).normalized(QString::NormalizationForm_C) != want)
            continue;
        if (archive_entry_is_encrypted(e))
            return fail(archive::messageUnsupportedArchive());
        // Whatever size the header states: never more than maxBytes are produced.
        QByteArray out;
        const qint64 declared = archive_entry_size_is_set(e) ? qint64(archive_entry_size(e)) : -1;
        if (declared > maxBytes)
            return fail(archive::messageMemberTooLarge());
        out.reserve(int(declared > 0 ? declared : kBlockSize));
        char buf[kBlockSize];
        for (;;) {
            const ssize_t n = archive_read_data(r.a, buf, sizeof buf);
            if (n == 0)
                break;
            if (n < 0)
                return fail(archive::messageDamagedArchive());
            if (qint64(out.size()) + n > maxBytes)
                return fail(archive::messageMemberTooLarge());
            out.append(buf, qsizetype(n));
        }
        return out;
    }
    return fail(archive::messageDamagedArchive());
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

archive::Listing list(const QString&) {
    archive::Listing out;
    out.error = archive::messageUnsupportedArchive();
    return out;
}

std::optional<QByteArray> read(const QString&, const QString&, qint64, QString* error) {
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
