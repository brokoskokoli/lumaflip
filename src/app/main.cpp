// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/Translations.h"
#include "app/ViewerWindow.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Settings.h"
#include "core/SortSpec.h"

#include <QApplication>
#include <QPointer>

#include <cstdio>
#include <cstdlib>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

Q_DECLARE_METATYPE(lf::InputEvent)
Q_DECLARE_METATYPE(lf::SortSpec)

namespace {

// Milliseconds since this process was started by the kernel (includes dyld, Qt init).
double msSinceProcessStart() {
    kinfo_proc info{};
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    timeval now{};
    gettimeofday(&now, nullptr);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return -1;
    const timeval start = info.kp_proc.p_starttime;
    return (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_usec - start.tv_usec) / 1000.0;
}

// Benchmark hook P_01_ProcessLaunchToFirstFrame24MP: with LF_EXIT_AFTER_FIRST_FRAME=1 the
// app prints "LF_FIRST_FRAME_MS=<ms>" once the first frame showing the opened file at
// Screen quality or better was presented, and exits at once (nothing saved or restored).
void installFirstFrameHook(lf::Application& app) {
    auto hook = [](lf::ViewerWindow* w) {
        auto* canvas = w->view()->findChild<lf::ImageCanvas*>();
        if (!canvas)
            return;
        auto armed = std::make_shared<bool>(false);
        QObject::connect(
            w, &lf::ViewerWindow::imageShown, canvas, [armed, canvas](const QString&, lf::Quality q) {
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
            std::printf("LF_FIRST_FRAME_MS=%.1f\n", msSinceProcessStart());
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

} // namespace

int main(int argc, char* argv[]) {
    const bool exitAfterFirstFrame = qgetenv("LF_EXIT_AFTER_FIRST_FRAME") == "1";
    QApplication app(argc, argv);
    lf::Settings::configureApplication(); // SPEC 8.6: settings/app data locations
    QApplication::setApplicationVersion(QStringLiteral(LF_APP_VERSION));
    lf::installQtTranslations(); // before the menu bar exists (application menu texts)
    if (!exitAfterFirstFrame)
        lf::Settings::migrateLegacyLocations();

    qRegisterMetaType<lf::InputEvent>();
    qRegisterMetaType<lf::SortSpec>();
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");

    lf::Application shell;
    if (exitAfterFirstFrame) {
        shell.setSessionEnabled(false);
        installFirstFrameHook(shell);
    }
    shell.start(QApplication::arguments());
    return QApplication::exec();
}
