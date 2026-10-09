// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for unit tests that need a Qt event loop (queued signals, timers).
// The application object is created lazily once per process and never destroyed.

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QGuiApplication>
#include <QThread>
#include <functional>

namespace lf::test {

inline void ensureQtApp() {
    if (QCoreApplication::instance())
        return;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char name[] = "lf_unit_tests";
    static char* argv[] = {name, nullptr};
    new QGuiApplication(argc, argv);
}

// Processes events until `done` returns true or `timeoutMs` elapses.
inline bool waitUntil(const std::function<bool()>& done, int timeoutMs = 2000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

// Processes events for `ms` milliseconds.
inline void spin(int ms) {
    waitUntil([] { return false; }, ms);
}

} // namespace lf::test
