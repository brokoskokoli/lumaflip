// SPDX-License-Identifier: GPL-3.0-or-later
#include "Bench.h"

#include <QApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    // --list: names of all registered benchmarks (tools/spec_coverage.py --build-dir).
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& b : lf::bench::registry())
            std::printf("%s\n", b.name.c_str());
        return 0;
    }
    QApplication app(argc, argv);
    const char* tolEnv = std::getenv("LF_BENCH_TOLERANCE");
    const double tolerance = tolEnv ? std::atof(tolEnv) : 1.2;
    // LF_BENCH_ONLY=name1,name2: run only these (tools/check.sh re-runs failed benchmarks).
    std::set<std::string> only;
    if (const char* onlyEnv = std::getenv("LF_BENCH_ONLY")) {
        std::stringstream ss(onlyEnv);
        for (std::string n; std::getline(ss, n, ',');)
            if (!n.empty())
                only.insert(n);
    }
    int failed = 0;
    for (const auto& b : lf::bench::registry()) {
        if (!only.empty() && only.count(b.name) == 0)
            continue;
        const double ms = b.fn();
        if (ms < 0) {
            std::printf("SKIP  %-45s\n", b.name.c_str());
            continue;
        }
        const bool ok = ms <= b.limitMs * tolerance;
        std::printf("%s  %-45s %9.2f ms  (limit %.1f ms)\n", ok ? "OK  " : "FAIL", b.name.c_str(), ms,
                    b.limitMs);
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}
