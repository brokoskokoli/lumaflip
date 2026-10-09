// SPDX-License-Identifier: GPL-3.0-or-later
// macOS shell integration: reveal in Finder (F-FILE-02), open with (F-FILE-05),
// default viewer registration (F-MAC-04).

#include "core/Platform.h"

#include <QCoreApplication>
#include <QStringList>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace lf {

namespace {

constexpr int64_t kDefaultAppTimeoutSec = 10;

QString tr(const char* text) {
    return QCoreApplication::translate("lf::Shell", text);
}

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

    bool makeDefaultViewer(const QStringList& types, QString* error) override {
        @autoreleasepool {
            NSURL* bundle = [[NSBundle mainBundle] bundleURL];
            if (![bundle.pathExtension isEqualToString:@"app"]) {
                if (error)
                    *error = tr("Lumaflip läuft nicht als App-Paket und kann nicht als Standard-App "
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
                                                                     tr("unbekannter Typ").toNSString()]];
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
                messages << tr("Zeitüberschreitung bei der Rückmeldung des Systems.");
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
