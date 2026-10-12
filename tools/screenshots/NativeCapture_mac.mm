// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeCapture.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace lf::shots {

QImage captureNativeWindow(QWidget* window) {
    if (!window)
        return {};
    long number = 0;
    @autoreleasepool {
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->window()->winId());
        NSWindow* w = view.window;
        if (!w)
            return {};
        number = long(w.windowNumber);
    }
    QTemporaryDir dir;
    if (!dir.isValid())
        return {};
    const QString file = dir.filePath(QStringLiteral("window.png"));
    QProcess capture;
    capture.start(
        QStringLiteral("/usr/sbin/screencapture"),
        {QStringLiteral("-x"), QStringLiteral("-o"), QStringLiteral("-l"), QString::number(number), file});
    if (!capture.waitForFinished(30000) || capture.exitStatus() != QProcess::NormalExit ||
        capture.exitCode() != 0)
        return {};
    QImage image(file);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace lf::shots
