// SPDX-License-Identifier: GPL-3.0-or-later
#include "Bench.h"

#include <QApplication>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const char* tolEnv = std::getenv("LF_BENCH_TOLERANCE");
    const double tolerance = tolEnv ? std::atof(tolEnv) : 1.2;
    int failed = 0;
    for (const auto& b : lf::bench::registry()) {
        const double ms = b.fn();
        if (ms < 0) {
            std::printf("SKIP  %-45s\n", b.name.c_str());
            continue;
        }
        const bool ok = ms <= b.limitMs * tolerance;
        std::printf("%s  %-45s %9.2f ms  (limit %.1f ms)\n", ok ? "OK  " : "FAIL", b.name.c_str(), ms, b.limitMs);
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}
