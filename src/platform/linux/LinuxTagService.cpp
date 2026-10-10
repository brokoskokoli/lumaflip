// SPDX-License-Identifier: GPL-3.0-or-later
// File tags on Linux (F-INFO-06; Platform.h TagService): the extended attribute
// "user.xdg.tags" (freedesktop Common Extended Attributes; comma-separated UTF-8), which KDE
// (Dolphin/Baloo) reads and writes. Replaces src/platform/generic/GenericTagService.cpp in
// Linux builds. File systems without user xattrs (tmpfs before 6.6, some network shares,
// FAT) report an error when writing; reading gives no tags.

#include "core/Platform.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

#include <sys/xattr.h>

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::TagService)
};

constexpr const char* kAttr = "user.xdg.tags";

class LinuxTagService final : public TagService {
public:
    bool isSupported() const override { return true; }

    std::optional<QStringList> tags(const QString& path, QString* error) override {
        const QByteArray p = QFile::encodeName(path);
        QByteArray value(256, Qt::Uninitialized);
        for (int attempt = 0; attempt < 4; ++attempt) {
            const ssize_t n = getxattr(p.constData(), kAttr, value.data(), size_t(value.size()));
            if (n >= 0) {
                value.truncate(qsizetype(n));
                return parse(value);
            }
            if (errno == ERANGE) {
                const ssize_t need = getxattr(p.constData(), kAttr, nullptr, 0);
                if (need < 0)
                    break;
                value.resize(qsizetype(need));
                continue;
            }
            if (errno == ENODATA || errno == ENOTSUP)
                return QStringList(); // no tags / no xattrs on this file system
            break;
        }
        if (error)
            *error =
                QFileInfo::exists(path)
                    ? Tr::tr("Die Tags von „%1“ können nicht gelesen werden.").arg(QFileInfo(path).fileName())
                    : Tr::tr("„%1“ existiert nicht mehr.").arg(QFileInfo(path).fileName());
        return std::nullopt;
    }

    bool setTags(const QString& path, const QStringList& tags, QString* error) override {
        const QByteArray p = QFile::encodeName(path);
        QStringList clean;
        for (QString t : tags) {
            t = t.trimmed();
            t.remove(QLatin1Char(',')); // the separator cannot be part of a tag
            if (!t.isEmpty() && !clean.contains(t))
                clean << t;
        }
        int rc = 0;
        if (clean.isEmpty()) {
            rc = removexattr(p.constData(), kAttr);
            if (rc != 0 && errno == ENODATA)
                rc = 0;
        } else {
            const QByteArray v = clean.join(QLatin1Char(',')).toUtf8();
            rc = setxattr(p.constData(), kAttr, v.constData(), size_t(v.size()), 0);
        }
        if (rc == 0)
            return true;
        if (error) {
            const int e = errno;
            if (!QFileInfo::exists(path))
                *error = Tr::tr("„%1“ existiert nicht mehr.").arg(QFileInfo(path).fileName());
            else if (e == ENOTSUP)
                *error = Tr::tr("Dieses Dateisystem unterstützt keine Tags.");
            else
                *error = Tr::tr("Die Tags von „%1“ können nicht gespeichert werden: %2")
                             .arg(QFileInfo(path).fileName(), QString::fromLocal8Bit(strerror(e)));
        }
        return false;
    }

    static QStringList parse(const QByteArray& value) {
        QStringList out;
        for (const QString& t : QString::fromUtf8(value).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString s = t.trimmed();
            if (!s.isEmpty() && !out.contains(s))
                out << s;
        }
        return out;
    }
};

} // namespace

std::unique_ptr<TagService> createTagService() {
    return std::make_unique<LinuxTagService>();
}

} // namespace lf
