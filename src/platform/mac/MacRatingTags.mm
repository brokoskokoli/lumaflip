// SPDX-License-Identifier: GPL-3.0-or-later
// Finder tags with label colors for ratings (F-CULL-02 c). NSURLTagNamesKey knows only the
// names, so the tags are read and written in the form Finder stores them: the extended
// attribute "com.apple.metadata:_kMDItemUserTags", a binary property list of strings
// "Name" or "Name\n<color index>".

#include "core/RatingStore.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#import <Foundation/Foundation.h>

#include <cerrno>
#include <cstring>
#include <sys/xattr.h>
#include <vector>

namespace lf::platform {

namespace {

constexpr const char* kTagsAttr = "com.apple.metadata:_kMDItemUserTags";

// Tr::tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::RatingStore)
};

QString errnoText() {
    return QString::fromLocal8Bit(strerror(errno));
}

} // namespace

bool tagsSupported() {
    return true;
}

std::optional<QStringList> readRawTags(const QString& path, QString* error) {
    const QByteArray p = QFile::encodeName(path);
    const ssize_t size = getxattr(p.constData(), kTagsAttr, nullptr, 0, 0, 0);
    if (size < 0) {
        if (errno == ENOATTR)
            return QStringList();
        if (error)
            *error = Tr::tr("Tags von „%1“ konnten nicht gelesen werden: %2")
                         .arg(QFileInfo(path).fileName(), errnoText());
        return std::nullopt;
    }
    std::vector<char> buf(static_cast<size_t>(size));
    const ssize_t got = getxattr(p.constData(), kTagsAttr, buf.data(), buf.size(), 0, 0);
    if (got < 0) {
        if (error)
            *error = Tr::tr("Tags von „%1“ konnten nicht gelesen werden: %2")
                         .arg(QFileInfo(path).fileName(), errnoText());
        return std::nullopt;
    }
    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:buf.data() length:NSUInteger(got)];
        id plist = [NSPropertyListSerialization propertyListWithData:data
                                                             options:NSPropertyListImmutable
                                                              format:nil
                                                               error:nil];
        QStringList out;
        if ([plist isKindOfClass:[NSArray class]])
            for (id item in static_cast<NSArray*>(plist))
                if ([item isKindOfClass:[NSString class]])
                    out << QString::fromNSString(static_cast<NSString*>(item));
        return out;
    }
}

bool writeRawTags(const QString& path, const QStringList& rawTags, QString* error) {
    const QByteArray p = QFile::encodeName(path);
    if (rawTags.isEmpty()) {
        if (removexattr(p.constData(), kTagsAttr, 0) == 0 || errno == ENOATTR)
            return true;
    } else {
        @autoreleasepool {
            NSMutableArray* arr = [NSMutableArray arrayWithCapacity:NSUInteger(rawTags.size())];
            for (const QString& t : rawTags)
                [arr addObject:t.toNSString()];
            NSData* data = [NSPropertyListSerialization dataWithPropertyList:arr
                                                                      format:NSPropertyListBinaryFormat_v1_0
                                                                     options:0
                                                                       error:nil];
            if (data && setxattr(p.constData(), kTagsAttr, data.bytes, data.length, 0, 0) == 0)
                return true;
        }
    }
    if (error)
        *error = Tr::tr("Tags von „%1“ konnten nicht gesetzt werden: %2")
                     .arg(QFileInfo(path).fileName(), errnoText());
    return false;
}

} // namespace lf::platform
