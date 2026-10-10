// SPDX-License-Identifier: GPL-3.0-or-later
#include "StartupTrace.h"

#include <QMutex>
#include <QSet>
#include <QtGlobal>

#include <cstdio>

#ifdef Q_OS_DARWIN
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <QElapsedTimer>
#endif

namespace lf::trace {

bool enabled() {
    static const bool on = qEnvironmentVariableIntValue("LF_TRACE_STARTUP") == 1;
    return on;
}

double msSinceProcessStart() {
#ifdef Q_OS_DARWIN
    kinfo_proc info{};
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    timeval now{};
    gettimeofday(&now, nullptr);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return -1;
    const timeval start = info.kp_proc.p_starttime;
    return double(now.tv_sec - start.tv_sec) * 1000.0 + double(now.tv_usec - start.tv_usec) / 1000.0;
#else
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return double(timer.nsecsElapsed()) / 1e6;
#endif
}

void mark(const char* label) {
    if (!enabled())
        return;
    std::fprintf(stderr, "LF_TRACE %7.1f %s\n", msSinceProcessStart(), label);
}

void once(const char* label) {
    if (!enabled())
        return;
    static QMutex mutex;
    static QSet<const void*> seen;
    {
        QMutexLocker lock(&mutex);
        if (seen.contains(label))
            return;
        seen.insert(label);
    }
    mark(label);
}

} // namespace lf::trace
