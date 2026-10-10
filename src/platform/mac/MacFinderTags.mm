// SPDX-License-Identifier: GPL-3.0-or-later
// Finder tags via NSURLTagNamesKey (F-INFO-06). Every call uses a fresh NSURL, so no
// cached resource values are shared between threads or calls.

#include "core/Platform.h"

#include <QCoreApplication>
#include <QFileInfo>

#import <Foundation/Foundation.h>

namespace lf {

namespace {

QString describe(NSError* err) {
    return err ? QString::fromNSString(err.localizedDescription) : QStringLiteral("?");
}

class MacTagService final : public TagService {
public:
    bool isSupported() const override { return true; }

    std::optional<QStringList> tags(const QString& path, QString* error) override {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
            NSArray* names = nil;
            NSError* err = nil;
            if (![url getResourceValue:&names forKey:NSURLTagNamesKey error:&err]) {
                if (error)
                    *error = QCoreApplication::translate("lf::TagService",
                                                         "Tags von „%1“ konnten nicht gelesen werden: %2")
                                 .arg(QFileInfo(path).fileName(), describe(err));
                return std::nullopt;
            }
            QStringList out;
            for (id n in names)
                if ([n isKindOfClass:[NSString class]])
                    out << QString::fromNSString(static_cast<NSString*>(n));
            return out;
        }
    }

    bool setTags(const QString& path, const QStringList& tags, QString* error) override {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
            NSMutableArray* names = [NSMutableArray arrayWithCapacity:NSUInteger(tags.size())];
            for (const QString& t : tags)
                [names addObject:t.toNSString()];
            NSError* err = nil;
            if (![url setResourceValue:names forKey:NSURLTagNamesKey error:&err]) {
                if (error)
                    *error = QCoreApplication::translate("lf::TagService",
                                                         "Tags von „%1“ konnten nicht gesetzt werden: %2")
                                 .arg(QFileInfo(path).fileName(), describe(err));
                return false;
            }
            return true;
        }
    }
};

} // namespace

std::unique_ptr<TagService> createTagService() {
    return std::make_unique<MacTagService>();
}

} // namespace lf
