// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/StartupWarmup.h"
#include "app/Translations.h"
#include "app/ViewerWindow.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Settings.h"
#include "core/SortSpec.h"
#include "core/StartupTrace.h"

#include <QApplication>
#include <QPointer>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

Q_DECLARE_METATYPE(lf::InputEvent)
Q_DECLARE_METATYPE(lf::SortSpec)

namespace {

// Benchmark hook P_01_ProcessLaunchToFirstFrame24MP: with LF_EXIT_AFTER_FIRST_FRAME=1 the
// app prints "LF_FIRST_FRAME_MS=<ms>" once the first frame showing the opened file at
// Screen quality or better was presented, and exits at once (nothing saved or restored).
void installFirstFrameHook(lf::Application& app) {
    auto hook = [](lf::ViewerWindow* w) {
        auto* canvas = w->view()->findChild<lf::ImageCanvas*>();
        if (!canvas)
            return;
        auto armed = std::make_shared<bool>(false);
        QObject::connect(w, &lf::ViewerWindow::imageShown, canvas,
                         [armed, canvas](const QString&, lf::Quality q) {
                             // The window's stage bookkeeping, not DecodedImage::isPreview (true for any
                             // downscaled image, also the Screen stage).
                             if (q != lf::Quality::Preview) {
                                 *armed = true;
                                 canvas->update();
                             }
                         });
        QObject::connect(canvas, &QRhiWidget::frameSubmitted, canvas, [armed] {
            if (!*armed)
                return;
            std::printf("LF_FIRST_FRAME_MS=%.1f\n", lf::trace::msSinceProcessStart());
            std::fflush(stdout);
            std::_Exit(0);
        });
    };
    auto hooked = std::make_shared<QList<QPointer<lf::ViewerWindow>>>();
    QObject::connect(&app, &lf::Application::windowsChanged, &app, [&app, hook, hooked] {
        for (lf::ViewerWindow* w : app.windows()) {
            if (hooked->contains(w))
                continue;
            hooked->append(w);
            hook(w);
        }
    });
}

// LF_TRACE_STARTUP: the window system events that lead to the first presented frame
// (the window is exposed only after the event loop has run NSApplication's launch).
class TraceEvents : public QObject {
public:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Expose && o->isWindowType())
            lf::trace::once("window exposed (event loop)");
        else if (e->type() == QEvent::FileOpen)
            lf::trace::once("open request from the system (QFileOpenEvent)");
        return false;
    }
};

} // namespace

int main(int argc, char* argv[]) {
    const bool exitAfterFirstFrame = qgetenv("LF_EXIT_AFTER_FIRST_FRAME") == "1";
    if (exitAfterFirstFrame) // diagnostics: time before main (dyld, static init)
        std::fprintf(stderr, "LF_MAIN_MS=%.1f\n", lf::trace::msSinceProcessStart());
    lf::trace::mark("main");
    // P-01: the GPU driver and the opened image are loaded in parallel to the creation of
    // the application object and the window.
    lf::startup::warmUpGpu();
    {
        QStringList args;
        for (int i = 1; i < argc; ++i)
            args << QString::fromLocal8Bit(argv[i]);
        const QString image = lf::startup::firstImageArgument(args);
        if (!image.isEmpty())
            lf::ImageProvider::prestart(image, lf::startup::expectedDecodeTarget());
    }
    lf::trace::mark("prestart queued");
    QApplication app(argc, argv);
    lf::trace::mark("QApplication created");
    lf::startup::warmUpAfterApplication();
    TraceEvents traceEvents;
    if (lf::trace::enabled())
        app.installEventFilter(&traceEvents);
    lf::Settings::configureApplication(); // SPEC 8.6: settings/app data locations
    QApplication::setApplicationVersion(QStringLiteral(LF_APP_VERSION));
    lf::installQtTranslations(); // UI language (F-I18N-03), before the menu bar exists
    lf::trace::mark("translations installed");
    if (!exitAfterFirstFrame)
        lf::Settings::migrateLegacyLocations();

    qRegisterMetaType<lf::InputEvent>();
    qRegisterMetaType<lf::SortSpec>();
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");

    lf::Application shell;
    lf::trace::mark("Application created");
    if (exitAfterFirstFrame) {
        shell.setSessionEnabled(false);
        installFirstFrameHook(shell);
    }
    if (lf::trace::enabled())
        QTimer::singleShot(0, [] { lf::trace::mark("event loop running"); });
    shell.start(QApplication::arguments());
    lf::trace::mark("start() returned, entering event loop");
    return QApplication::exec();
}
