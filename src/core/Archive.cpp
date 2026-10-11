// SPDX-License-Identifier: GPL-3.0-or-later
#include "Archive.h"

#include "DecoderHelper.h"
#include "LibArchive.h"
#include "PdfDocument.h"
#include "Platform.h"
#include "VirtualPath.h"
#include "ZipReader.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTimeZone>

#include <algorithm>

namespace lf::archive {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Archive)
};

constexpr int kListTimeoutMs = 20000;
constexpr int kReadTimeoutMs = 30000;

bool acceptName(const QString& name) {
    if (name.isEmpty() || name.startsWith(u'/') || name.endsWith(u'/'))
        return false;
    for (const QStringView part : QStringView(name).split(u'/'))
        if (part == u"..")
            return false;
    return true;
}

Listing listZip(const QString& path) {
    Listing out;
    ZipReader zip(path);
    if (!zip.isValid()) {
        // ZIP64 and other variants the own reader rejects: libarchive may know them.
        if (libarchive::available()) {
            Listing la = libarchive::list(path);
            if (la.ok)
                return la;
        }
        out.error = QFileInfo::exists(path) ? messageDamagedArchive() : zip.errorString();
        return out;
    }
    if (zip.names().isEmpty() && zip.encryptedCount() > 0) {
        out.error = messageEncryptedArchive();
        return out;
    }
    const QStringList names = zip.names();
    for (const QString& n : names) {
        if (out.members.size() >= kMaxMembers)
            break;
        if (!acceptName(n))
            continue;
        const auto m = zip.member(n);
        if (!m)
            continue;
        out.members.append(
            Member{n.normalized(QString::NormalizationForm_C), qint64(m->uncompressedSize), {}});
    }
    out.ok = true;
    return out;
}

std::optional<QByteArray> readZip(const QString& path, const QString& member, QString* error) {
    ZipReader zip(path);
    if (!zip.isValid()) {
        if (libarchive::available())
            return libarchive::read(path, member, kMaxMemberBytes, error);
        if (error)
            *error = messageDamagedArchive();
        return std::nullopt;
    }
    // Names are compared in NFC (the collection normalizes paths).
    QString stored;
    const QString want = member.normalized(QString::NormalizationForm_C);
    if (zip.member(member))
        stored = member;
    else
        for (const QString& n : zip.names())
            if (n.normalized(QString::NormalizationForm_C) == want) {
                stored = n;
                break;
            }
    const auto m = stored.isEmpty() ? std::nullopt : zip.member(stored);
    if (!m) {
        if (error)
            *error = zip.isEncrypted(member) ? messageEncryptedArchive()
                                             : Tr::tr("„%1“ fehlt im Archiv.").arg(member);
        return std::nullopt;
    }
    if (qint64(m->uncompressedSize) > kMaxMemberBytes) {
        if (error)
            *error = messageMemberTooLarge();
        return std::nullopt;
    }
    // ZipReader never produces more than the stated size (and at most ~1032:1 of the
    // compressed bytes): a zip bomb cannot make it allocate more than kMaxMemberBytes.
    auto data = zip.read(stored, kMaxMemberBytes);
    if (!data && error)
        *error = messageDamagedArchive();
    return data;
}

Listing listPdf(const QString& path) {
    Listing out;
    QString error;
    const int pages = pdf::pageCount(path, &error);
    if (pages < 0) {
        out.error = error;
        return out;
    }
    for (int i = 0; i < pages && i < kMaxMembers; ++i)
        out.members.append(Member{vpath::pdfPageMember(i), -1, {}});
    out.ok = true;
    return out;
}

} // namespace

QString messageUnsupportedArchive() {
    return Tr::tr("Dieses Archivformat wird nicht unterstützt.");
}
QString messageMemberTooLarge() {
    return Tr::tr("Das Bild im Archiv ist zu groß.");
}
QString messageDamagedArchive() {
    return Tr::tr("Das Archiv ist beschädigt oder unvollständig.");
}
QString messageNoImages() {
    return Tr::tr("Das Archiv enthält keine Bilder.");
}
QString messageEncryptedArchive() {
    return Tr::tr("Das Archiv ist passwortgeschützt – verschlüsselte Archive werden nicht unterstützt.");
}
QString messageArchiveTooLarge() {
    return Tr::tr("Das Archiv ist zu groß, um dieses Bild daraus zu lesen.");
}

bool libArchiveAvailable() {
    return libarchive::available();
}

Listing listLocal(const QString& containerPath) {
    switch (vpath::containerKind(containerPath)) {
    case vpath::ContainerKind::Zip:
        return listZip(containerPath);
    case vpath::ContainerKind::LibArchive: {
        Listing l = libarchive::list(containerPath);
        if (!l.ok && l.error.isEmpty())
            l.error = messageUnsupportedArchive();
        return l;
    }
    case vpath::ContainerKind::Pdf:
        return listPdf(containerPath);
    case vpath::ContainerKind::None:
        break;
    }
    Listing out;
    out.error = messageUnsupportedArchive();
    return out;
}

std::optional<QByteArray> readLocal(const QString& containerPath, const QString& member, QString* error) {
    switch (vpath::containerKind(containerPath)) {
    case vpath::ContainerKind::Zip:
        return readZip(containerPath, member, error);
    case vpath::ContainerKind::LibArchive:
        return libarchive::read(containerPath, member, kMaxMemberBytes, error);
    case vpath::ContainerKind::Pdf:
    case vpath::ContainerKind::None:
        break;
    }
    if (error)
        *error = messageUnsupportedArchive();
    return std::nullopt;
}

std::optional<QVector<Extracted>> readRunLocal(const QString& containerPath, const QString& member,
                                               QString* error) {
    if (vpath::containerKind(containerPath) == vpath::ContainerKind::LibArchive)
        return libarchive::readRun(containerPath, member, kMaxMemberBytes, kMaxRunBytes,
                                   &vpath::isImageMemberName, error);
    auto data = readLocal(containerPath, member, error);
    if (!data)
        return std::nullopt;
    return QVector<Extracted>{Extracted{member.normalized(QString::NormalizationForm_C), std::move(*data)}};
}

Listing list(const QString& containerPath) {
    if (vpath::containerKind(containerPath) == vpath::ContainerKind::None) {
        Listing out;
        out.error = messageUnsupportedArchive();
        return out;
    }
    if (const auto pool = DecoderHelperPool::shared()) {
        ipc::Request req;
        req.op = ipc::Op::ArchiveList;
        req.path = containerPath;
        Listing out;
        DecoderHelperPool::Failure failure = DecoderHelperPool::Failure::None;
        if (auto r = pool->call(req, kListTimeoutMs, &failure, &out.error)) {
            out.ok = r->ok;
            if (!r->ok)
                out.error = r->error;
            // The helper's answer is checked like any other input.
            for (const Member& m : std::as_const(r->members))
                if (out.members.size() < kMaxMembers && acceptName(m.name))
                    out.members.append(m);
        }
        return out;
    }
    return listLocal(containerPath);
}

std::optional<QByteArray> read(const QString& containerPath, const QString& member, QString* error) {
    if (const auto pool = DecoderHelperPool::shared()) {
        ipc::Request req;
        req.op = ipc::Op::ArchiveRead;
        req.path = containerPath;
        req.member = member;
        DecoderHelperPool::Failure failure = DecoderHelperPool::Failure::None;
        QString callError;
        auto r = pool->call(req, kReadTimeoutMs, &failure, &callError);
        if (!r) {
            if (error)
                *error = callError;
            return std::nullopt;
        }
        if (!r->ok) {
            if (error)
                *error = r->error;
            return std::nullopt;
        }
        if (r->data.size() > kMaxMemberBytes) {
            if (error)
                *error = messageMemberTooLarge();
            return std::nullopt;
        }
        return r->data;
    }
    return readLocal(containerPath, member, error);
}

std::optional<QVector<Extracted>> readRun(const QString& containerPath, const QString& member,
                                          QString* error) {
    const auto pool = DecoderHelperPool::shared();
    if (!pool)
        return readRunLocal(containerPath, member, error);
    ipc::Request req;
    req.op = ipc::Op::ArchiveReadRun;
    req.path = containerPath;
    req.member = member;
    DecoderHelperPool::Failure failure = DecoderHelperPool::Failure::None;
    QString callError;
    auto r = pool->call(req, kReadTimeoutMs, &failure, &callError);
    if (!r || !r->ok) {
        if (error)
            *error = r ? r->error : callError;
        return std::nullopt;
    }
    // The helper's answer is checked like any other input: member i is the next
    // members[i].size bytes of data; the first one is the wanted member.
    QVector<Extracted> out;
    qint64 at = 0;
    for (const Member& m : std::as_const(r->members)) {
        if (m.size < 0 || m.size > kMaxMemberBytes || m.size > r->data.size() - at || !acceptName(m.name))
            break;
        out.append(Extracted{m.name, r->data.mid(at, m.size)});
        at += m.size;
    }
    if (out.isEmpty() || out.first().name != member.normalized(QString::NormalizationForm_C)) {
        if (error)
            *error = messageDamagedArchive();
        return std::nullopt;
    }
    return out;
}

void sortMemberEntries(QVector<FileEntry>& entries, const NameCollator& collator, bool ascending) {
    // Folder by folder inside the archive (like a Finder list view), Finder name order.
    auto parts = [](const FileEntry& e) {
        const auto p = vpath::split(e.path);
        return p ? p->member : e.name;
    };
    QVector<QString> keys;
    keys.reserve(entries.size());
    for (const FileEntry& e : std::as_const(entries))
        keys.append(parts(e));
    QVector<int> order(entries.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    auto compare = [&](const QString& a, const QString& b) {
        const QStringList pa = a.split(u'/');
        const QStringList pb = b.split(u'/');
        const qsizetype n = std::min(pa.size(), pb.size());
        for (qsizetype i = 0; i < n; ++i) {
            const bool lastA = i == pa.size() - 1, lastB = i == pb.size() - 1;
            if (lastA != lastB)
                return lastA ? -1 : 1; // files of a folder before its subfolders
            if (const int c = collator.compare(pa[i], pb[i]); c != 0)
                return c;
        }
        return int(pa.size() - pb.size());
    };
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        const int c = compare(keys[x], keys[y]);
        return ascending ? c < 0 : c > 0;
    });
    QVector<FileEntry> sorted;
    sorted.reserve(entries.size());
    for (int i : std::as_const(order))
        sorted.append(std::move(entries[i]));
    entries = std::move(sorted);
}

QVector<FileEntry> imageEntries(const QString& containerPath, const Listing& listing,
                                const NameCollator& collator) {
    QVector<FileEntry> out;
    const bool isPdf = vpath::containerKind(containerPath) == vpath::ContainerKind::Pdf;
    const QFileInfo container(containerPath);
    const QDateTime containerDate = container.lastModified(QTimeZone::UTC);
    for (const Member& m : listing.members) {
        if (!(isPdf ? vpath::pdfPageIndex(m.name) >= 0 : vpath::isImageMemberName(m.name)))
            continue;
        FileEntry e;
        e.path = vpath::join(containerPath, m.name);
        e.name = vpath::memberFileName(e.path);
        e.size = m.size;
        e.modified = m.modified.isValid() ? m.modified : containerDate;
        e.created = e.modified;
        e.kind = isPdf ? QStringLiteral("PDF") : QString();
        out.append(std::move(e));
    }
    if (!isPdf)
        sortMemberEntries(out, collator); // PDF pages stay in page order
    return out;
}

} // namespace lf::archive
