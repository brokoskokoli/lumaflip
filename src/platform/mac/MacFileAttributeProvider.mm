// SPDX-License-Identifier: GPL-3.0-or-later
// Fills "Date Added", creation date and the localized kind of FileEntry objects.
// One getattrlist(2) call per file (no Foundation round trips); kinds are cached per
// file name extension, so filling 10 000 entries stays in the low milliseconds.
// Image properties for sorting (capture date, pixel count, rating) come from ImageIO's
// property dictionary, which reads only header and metadata, never the pixels.

#include "core/Platform.h"

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QTimeZone>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sys/attr.h>
#include <unistd.h>

namespace lf {
namespace {

QDateTime fromTimespec(const struct timespec& ts) {
    return QDateTime::fromMSecsSinceEpoch(qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000, QTimeZone::UTC);
}

class MacFileAttributeProvider final : public FileAttributeProvider {
  public:
    void fill(FileEntry& entry) override {
        fillTimes(entry);
        if (entry.kind.isEmpty())
            entry.kind = kindFor(entry.name);
    }

    void fillImageProperties(FileEntry& entry) override {
        entry.imagePropertiesRead = true;
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:entry.path.toNSString() isDirectory:NO];
            NSDictionary* srcOpts = @{(id)kCGImageSourceShouldCache : @NO};
            CGImageSourceRef src =
                CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)srcOpts);
            if (!src)
                return;
            if (CGImageSourceGetCount(src) > 0) {
                NSDictionary* props = CFBridgingRelease(
                    CGImageSourceCopyPropertiesAtIndex(src, 0, (__bridge CFDictionaryRef)srcOpts));
                readProperties(props, entry);
                if (entry.rating < 0) {
                    CGImageMetadataRef meta = CGImageSourceCopyMetadataAtIndex(src, 0, nullptr);
                    if (meta) {
                        NSString* r = CFBridgingRelease(
                            CGImageMetadataCopyStringValueWithPath(meta, nullptr, CFSTR("xmp:Rating")));
                        if (r)
                            entry.rating = ratingFrom(r.doubleValue);
                        CFRelease(meta);
                    }
                }
            }
            CFRelease(src);
        }
    }

  private:
    static int ratingFrom(double v) {
        // XMP: -1 = rejected, 0 = unrated, 1..5 stars.
        if (!(v >= 0))
            return -1;
        return int(std::min(5.0, std::floor(v)));
    }

    static void readProperties(NSDictionary* props, FileEntry& entry) {
        if (![props isKindOfClass:[NSDictionary class]])
            return;
        NSNumber* w = props[(id)kCGImagePropertyPixelWidth];
        NSNumber* h = props[(id)kCGImagePropertyPixelHeight];
        if ([w isKindOfClass:[NSNumber class]] && [h isKindOfClass:[NSNumber class]] && w.longLongValue > 0 &&
            h.longLongValue > 0)
            entry.pixelCount = w.longLongValue * h.longLongValue;
        NSDictionary* exif = props[(id)kCGImagePropertyExifDictionary];
        if ([exif isKindOfClass:[NSDictionary class]]) {
            NSString* dto = exif[(id)kCGImagePropertyExifDateTimeOriginal];
            if (![dto isKindOfClass:[NSString class]])
                dto = exif[(id)kCGImagePropertyExifDateTimeDigitized];
            if ([dto isKindOfClass:[NSString class]]) {
                NSString* offset = exif[@"OffsetTimeOriginal"];
                const QDateTime dt =
                    exifDate(QString::fromNSString(dto),
                             [offset isKindOfClass:[NSString class]] ? QString::fromNSString(offset) : QString());
                if (dt.isValid())
                    entry.captured = dt;
            }
        }
        NSDictionary* iptc = props[(id)kCGImagePropertyIPTCDictionary];
        if ([iptc isKindOfClass:[NSDictionary class]]) {
            NSNumber* stars = iptc[(id)kCGImagePropertyIPTCStarRating];
            if ([stars isKindOfClass:[NSNumber class]])
                entry.rating = ratingFrom(stars.doubleValue);
        }
    }

    // EXIF "YYYY:MM:DD HH:MM:SS" (local time of the camera) plus optional "+HH:MM" offset.
    // Without an offset the time is taken as local time, like Finder and Photos do.
    static QDateTime exifDate(const QString& text, const QString& offset) {
        const QDate date = QDate::fromString(text.left(10), QStringLiteral("yyyy:MM:dd"));
        const QTime time = QTime::fromString(text.mid(11, 8), QStringLiteral("HH:mm:ss"));
        if (!date.isValid() || !time.isValid())
            return {};
        const QString off = offset.trimmed();
        if (off.size() == 6 && (off[0] == u'+' || off[0] == u'-') && off[3] == u':') {
            bool okH = false, okM = false;
            const int hh = off.mid(1, 2).toInt(&okH);
            const int mm = off.mid(4, 2).toInt(&okM);
            if (okH && okM) {
                const int secs = (hh * 3600 + mm * 60) * (off[0] == u'-' ? -1 : 1);
                return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(secs));
            }
        }
        return QDateTime(date, time, QTimeZone::LocalTime);
    }

    QMutex mutex_;
    QHash<QString, QString> kinds_; // lower-case suffix -> localized description

    static void fillTimes(FileEntry& entry) {
        struct attrlist req = {};
        req.bitmapcount = ATTR_BIT_MAP_COUNT;
        req.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_CRTIME | ATTR_CMN_ADDEDTIME;
        alignas(8) char buf[128] = {};
        const QByteArray path = QFile::encodeName(entry.path);
        if (getattrlist(path.constData(), &req, buf, sizeof buf, 0) != 0)
            return;
        // Layout: u_int32_t length, attribute_set_t returned, then the attributes in bit
        // order; only those flagged in `returned` are present.
        u_int32_t length = 0;
        attribute_set_t returned = {};
        std::memcpy(&length, buf, sizeof length);
        std::memcpy(&returned, buf + sizeof length, sizeof returned);
        const char* p = buf + sizeof length + sizeof returned;
        const char* end = buf + std::min<size_t>(length, sizeof buf);
        struct timespec ts;
        if (returned.commonattr & ATTR_CMN_CRTIME) {
            if (p + sizeof ts > end)
                return;
            std::memcpy(&ts, p, sizeof ts);
            entry.created = fromTimespec(ts);
            p += sizeof ts;
        }
        if (returned.commonattr & ATTR_CMN_ADDEDTIME) {
            if (p + sizeof ts > end)
                return;
            std::memcpy(&ts, p, sizeof ts);
            entry.added = fromTimespec(ts);
        }
    }

    QString kindFor(const QString& name) {
        const qsizetype dot = name.lastIndexOf(u'.');
        const QString suffix = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
        {
            QMutexLocker lock(&mutex_);
            const auto it = kinds_.constFind(suffix);
            if (it != kinds_.cend())
                return *it;
        }
        QString kind;
        @autoreleasepool {
            UTType* type = suffix.isEmpty() ? nil : [UTType typeWithFilenameExtension:suffix.toNSString()];
            if (type && type.localizedDescription)
                kind = QString::fromNSString(type.localizedDescription);
        }
        QMutexLocker lock(&mutex_);
        kinds_.insert(suffix, kind);
        return kind;
    }
};

} // namespace

std::unique_ptr<FileAttributeProvider> createFileAttributeProvider() {
    return std::make_unique<MacFileAttributeProvider>();
}

} // namespace lf
