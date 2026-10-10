// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/mac/SparkleUpdater.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

namespace lf {

namespace {

QString toQ(NSString* s) {
    return s ? QString::fromNSString(s) : QString();
}

NSBundle* bundleFor(const QString& bundlePath) {
    return bundlePath.isEmpty() ? NSBundle.mainBundle : [NSBundle bundleWithPath:bundlePath.toNSString()];
}

} // namespace

SparkleBundleInfo readSparkleBundleInfo(const QString& bundlePath) {
    SparkleBundleInfo info;
    @autoreleasepool {
        NSBundle* bundle = bundleFor(bundlePath);
        if (!bundle)
            return info;
        info.bundleId = toQ(bundle.bundleIdentifier);
        info.isBundle = !info.bundleId.isEmpty() && [bundle.bundlePath.pathExtension isEqualToString:@"app"];
        id feed = [bundle objectForInfoDictionaryKey:@"SUFeedURL"];
        id key = [bundle objectForInfoDictionaryKey:@"SUPublicEDKey"];
        if ([feed isKindOfClass:NSString.class])
            info.feedUrl = toQ(feed);
        if ([key isKindOfClass:NSString.class])
            info.publicKey = toQ(key);
    }
    return info;
}

struct SparkleUpdater::Private {
    SPUStandardUpdaterController* controller = nil; // main bundle
    SPUStandardUserDriver* userDriver = nil;        // other bundles
    SPUUpdater* updater = nil;
    bool started = false;
};

SparkleUpdater::SparkleUpdater(const QString& bundlePath) : d(std::make_unique<Private>()) {
    @autoreleasepool {
        if (bundlePath.isEmpty()) {
            d->controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO
                                                                          updaterDelegate:nil
                                                                       userDriverDelegate:nil];
            d->updater = d->controller.updater;
        } else {
            NSBundle* bundle = bundleFor(bundlePath);
            if (bundle) {
                d->userDriver = [[SPUStandardUserDriver alloc] initWithHostBundle:bundle delegate:nil];
                d->updater = [[SPUUpdater alloc] initWithHostBundle:bundle
                                                  applicationBundle:bundle
                                                         userDriver:d->userDriver
                                                           delegate:nil];
            }
        }
    }
}

SparkleUpdater::~SparkleUpdater() = default;

bool SparkleUpdater::start(QString* error) {
    if (d->started)
        return true;
    if (!d->updater) {
        if (error)
            *error = QStringLiteral("no updater for this bundle");
        return false;
    }
    NSError* err = nil;
    d->started = [d->updater startUpdater:&err];
    if (!d->started && error)
        *error = toQ(err.localizedDescription);
    return d->started;
}

bool SparkleUpdater::isStarted() const {
    return d->started;
}

bool SparkleUpdater::automaticallyChecks() const {
    return d->updater && d->updater.automaticallyChecksForUpdates;
}

void SparkleUpdater::setAutomaticallyChecks(bool on) {
    if (d->updater)
        d->updater.automaticallyChecksForUpdates = on;
}

bool SparkleUpdater::automaticallyDownloads() const {
    return d->updater && d->updater.automaticallyDownloadsUpdates;
}

void SparkleUpdater::setAutomaticallyDownloads(bool on) {
    if (d->updater)
        d->updater.automaticallyDownloadsUpdates = on;
}

bool SparkleUpdater::allowsAutomaticDownloads() const {
    return d->updater && d->updater.allowsAutomaticUpdates;
}

bool SparkleUpdater::canCheckForUpdates() const {
    return d->started && d->updater.canCheckForUpdates;
}

void SparkleUpdater::checkForUpdates() {
    if (!d->started)
        return;
    if (d->controller)
        [d->controller checkForUpdates:nil];
    else
        [d->updater checkForUpdates];
}

} // namespace lf
