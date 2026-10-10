// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal, bounds-checked ZIP reader for container image formats (OpenRaster .ora, Krita
// .kra; SPEC 4.4 "Rest"). Reads the central directory and single members (stored or
// deflated) with random access, so a large archive is never read as a whole. No ZIP64,
// no encryption, no multi-disk archives: those are reported as errors. Every size and
// offset from the file is checked against the file size before it is used (fuzz-safe).
// Not thread-safe; one reader per thread.

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace lf {

class ZipReader {
public:
    struct Member {
        QString name;
        quint16 method = 0; // 0 stored, 8 deflate
        quint32 compressedSize = 0;
        quint32 uncompressedSize = 0;
        quint32 localHeaderOffset = 0;
    };

    // Opens `path` and reads the central directory. isValid() tells whether it worked.
    explicit ZipReader(const QString& path);
    // Reads from memory (tests, fuzzing).
    explicit ZipReader(QByteArray data);

    bool isValid() const { return m_valid; }
    QString errorString() const { return m_error; }
    QStringList names() const;
    std::optional<Member> member(const QString& name) const;
    // Contents of a member, at most `maxBytes` of the uncompressed data (0 = the whole
    // member, up to kMaxMemberBytes). nullopt + errorString() on damaged data.
    std::optional<QByteArray> read(const QString& name, qint64 maxBytes = 0);

    static constexpr qint64 kMaxMemberBytes = qint64(1) << 30; // 1 GiB
    static constexpr int kMaxMembers = 100000;

private:
    bool readAt(qint64 offset, qint64 size, QByteArray* out);
    bool parseDirectory();
    bool fail(const QString& error);

    QFile m_file;
    QByteArray m_data; // memory variant
    bool m_inMemory = false;
    qint64 m_size = 0;
    bool m_valid = false;
    QString m_error;
    QHash<QString, Member> m_members;
    QStringList m_order;
};

} // namespace lf
