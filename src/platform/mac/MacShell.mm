// SPDX-License-Identifier: GPL-3.0-or-later
// macOS shell integration: reveal in Finder (F-FILE-02), open with (F-FILE-05),
// default viewer registration (F-MAC-04), default app query (F-MAC-09).

#include "MacDefaultApp.h"
#include "core/DefaultAppOffer.h"
#include "core/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace lf {

namespace {

constexpr int64_t kDefaultAppTimeoutSec = 10; // blocking variant only
constexpr int kDefaultAppPendingMs = 3000;    // "still running" note (async)

class MacShell final : public Shell {
public:
    void revealInFileManager(const QString& path) override {
        @autoreleasepool {
            [[NSWorkspace sharedWorkspace]
                activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path.toNSString()] ]];
        }
    }

    void openWith(const QString& path, const QString& appPath) override {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
            NSWorkspace* ws = [NSWorkspace sharedWorkspace];
            NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
            if (appPath.isEmpty()) {
                NSURL* app = [ws URLForApplicationToOpenURL:url];
                if (!app)
                    return;
                [ws openURLs:@[ url ] withApplicationAtURL:app configuration:config completionHandler:nil];
                return;
            }
            [ws openURLs:@[ url ]
                withApplicationAtURL:[NSURL fileURLWithPath:appPath.toNSString()]
                       configuration:config
                   completionHandler:nil];
        }
    }

    // Keep in sync with CFBundleDocumentTypes (rank Default) in packaging/macos/Info.plist.in.
    QStringList imageDocumentTypes() const override {
        return {"public.jpeg",
                "public.png",
                "com.compuserve.gif",
                "public.tiff",
                "com.microsoft.bmp",
                "org.webmproject.webp",
                "public.heic",
                "public.heif",
                "public.avif",
                "public.jpeg-xl",
                "public.camera-raw-image",
                "com.adobe.raw-image",
                "com.adobe.photoshop-image",
                "com.ilm.openexr-image",
                "public.radiance",
                "public.jpeg-2000",
                "com.truevision.tga-image",
                "com.microsoft.ico",
                "com.apple.icns",
                "public.svg-image"};
    }

    // Non-blocking (F-MAC-04): the per-type completion handlers are collected thread-safely
    // (MacDefaultApp); the final result is delivered whenever the system answers. After a
    // while without an answer the user gets a "still running" note instead of a failure:
    // macOS may be waiting for a confirmation dialog.
    void makeDefaultViewerAsync(const QStringList& types, QObject* context, DefaultViewerDone done,
                                DefaultViewerPending pending) override {
        @autoreleasepool {
            NSURL* bundle = [[NSBundle mainBundle] bundleURL];
            if (![bundle.pathExtension isEqualToString:@"app"]) {
                const QString error = QCoreApplication::translate(
                    "lf::Shell", "Lumaflip läuft nicht als App-Paket und kann nicht als Standard-App "
                                 "festgelegt werden.");
                QPointer<QObject> guard(context);
                QMetaObject::invokeMethod(
                    context,
                    [guard, done, error] {
                        if (guard && done)
                            done(false, error);
                    },
                    Qt::QueuedConnection);
                return;
            }
            auto known = [](const QString& t) { return [UTType typeWithIdentifier:t.toNSString()] != nil; };
            mac::SetDefaultApp setter = [bundle](const QString& t,
                                                 // By value: the async completion block keeps it.
                                                 // NOLINTNEXTLINE(performance-unnecessary-value-param)
                                                 std::function<void(const QString&)> completion) {
                UTType* type = [UTType typeWithIdentifier:t.toNSString()];
                if (!type) { // not reached: `known` filters unknown types first
                    completion(t);
                    return;
                }
                [[NSWorkspace sharedWorkspace]
                    setDefaultApplicationAtURL:bundle
                             toOpenContentType:type
                             completionHandler:^(NSError* err) {
                               completion(err ? QString::fromNSString(err.localizedDescription) : QString());
                             }];
            };
            mac::registerDefaultApp(types, known, setter, kDefaultAppPendingMs, context, done, pending);
        }
    }

    // F-MAC-09: the content type of the file (only types the app declares in Info.plist,
    // i.e. conforming to imageDocumentTypes()) and whether LaunchServices opens it with this
    // app. Called on a worker thread: LaunchServices may take a while.
    DefaultViewerStatus defaultViewerStatus(const QString& filePath) override {
        @autoreleasepool {
            DefaultViewerStatus status;
            NSString* own = [[NSBundle mainBundle] bundleIdentifier];
            if (own.length == 0)
                return status; // no bundle: nothing to compare with
            NSURL* url = [NSURL fileURLWithPath:filePath.toNSString()];
            UTType* type = nil;
            [url getResourceValue:&type forKey:NSURLContentTypeKey error:nil];
            if (!type) {
                const QString suffix = QFileInfo(filePath).suffix();
                if (!suffix.isEmpty())
                    type = [UTType typeWithFilenameExtension:suffix.toNSString()];
            }
            if (!type)
                return status;
            bool ours = false;
            for (const QString& t : imageDocumentTypes()) {
                UTType* declared = [UTType typeWithIdentifier:t.toNSString()];
                if (declared && [type conformsToType:declared]) {
                    ours = true;
                    break;
                }
            }
            if (!ours)
                return status;
            status.type = QString::fromNSString(type.identifier);
            status.typeName =
                type.localizedDescription ? QString::fromNSString(type.localizedDescription) : status.type;
            NSURL* app = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenContentType:type];
            if (!app) { // no app at all opens the type
                status.state = DefaultViewerStatus::State::NotDefault;
                return status;
            }
            NSString* appId = [[NSBundle bundleWithURL:app] bundleIdentifier];
            if (appId.length == 0)
                return status; // cannot tell
            status.state = [appId isEqualToString:own] ? DefaultViewerStatus::State::IsDefault
                                                       : DefaultViewerStatus::State::NotDefault;
            return status;
        }
    }

    // F-MAC-09: an app bundle in /Applications or ~/Applications on a writable volume (not
    // the build tree, not a mounted disk image, not App Translocation).
    bool isInstalledApp() override {
        @autoreleasepool {
            NSURL* bundle = [[[NSBundle mainBundle] bundleURL] URLByResolvingSymlinksInPath];
            if (![bundle.pathExtension isEqualToString:@"app"])
                return false;
            NSNumber* readOnly = nil;
            if (![bundle getResourceValue:&readOnly forKey:NSURLVolumeIsReadOnlyKey error:nil] || !readOnly ||
                readOnly.boolValue)
                return false;
            return defaultapp::isInsideAny(
                QString::fromNSString(bundle.path),
                {QStringLiteral("/Applications"), QDir::homePath() + QStringLiteral("/Applications")});
        }
    }

    bool makeDefaultViewer(const QStringList& types, QString* error) override {
        @autoreleasepool {
            NSURL* bundle = [[NSBundle mainBundle] bundleURL];
            if (![bundle.pathExtension isEqualToString:@"app"]) {
                if (error)
                    *error = QCoreApplication::translate(
                        "lf::Shell", "Lumaflip läuft nicht als App-Paket und kann nicht als Standard-App "
                                     "festgelegt werden.");
                return false;
            }
            NSMutableArray<NSString*>* errors = [NSMutableArray array];
            dispatch_group_t group = dispatch_group_create();
            for (const QString& t : types) {
                UTType* type = [UTType typeWithIdentifier:t.toNSString()];
                if (!type) {
                    @synchronized(errors) {
                        [errors addObject:[NSString stringWithFormat:@"%@: %@", t.toNSString(),
                                                                     QCoreApplication::translate(
                                                                         "lf::Shell", "unbekannter Typ")
                                                                         .toNSString()]];
                    }
                    continue;
                }
                NSString* ident = t.toNSString();
                dispatch_group_enter(group);
                [[NSWorkspace sharedWorkspace]
                    setDefaultApplicationAtURL:bundle
                             toOpenContentType:type
                             completionHandler:^(NSError* err) {
                               if (err) {
                                   @synchronized(errors) {
                                       [errors
                                           addObject:[NSString stringWithFormat:@"%@: %@", ident,
                                                                                err.localizedDescription]];
                                   }
                               }
                               dispatch_group_leave(group);
                             }];
            }
            const long timedOut = dispatch_group_wait(
                group, dispatch_time(DISPATCH_TIME_NOW, kDefaultAppTimeoutSec * int64_t(NSEC_PER_SEC)));
            QStringList messages;
            @synchronized(errors) {
                for (NSString* e in errors)
                    messages << QString::fromNSString(e);
            }
            if (timedOut)
                messages << QCoreApplication::translate(
                    "lf::Shell", "Zeitüberschreitung bei der Rückmeldung des Systems.");
            if (!messages.isEmpty()) {
                if (error)
                    *error = messages.join(QLatin1Char('\n'));
                return false;
            }
            return true;
        }
    }
};

} // namespace

std::unique_ptr<Shell> createShell() {
    return std::make_unique<MacShell>();
}

} // namespace lf
