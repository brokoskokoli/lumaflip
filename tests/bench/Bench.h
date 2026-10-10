// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal benchmark registry for P-xx requirements (SPEC 7, 15.1).
// Usage in any tests/bench/*.cpp (no main()):
//   LF_BENCH(P_05_EnumerateAndSort10k, 500.0) { ... return measuredMs; }
// The function returns the measured value in milliseconds; the run fails if it
// exceeds the limit * tolerance (LF_BENCH_TOLERANCE, default 1.2). Return a
// negative value to mark the benchmark as skipped (e.g. corpus file missing).

#include <functional>
#include <string>
#include <vector>

namespace lf::bench {

struct Entry {
    std::string name;
    double limitMs;
    std::function<double()> fn;
};

inline std::vector<Entry>& registry() {
    static std::vector<Entry> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, double limitMs, std::function<double()> fn) {
        registry().push_back({name, limitMs, std::move(fn)});
    }
};

} // namespace lf::bench

#define LF_BENCH(name, limitMs)                                                                              \
    static double lf_bench_##name();                                                                         \
    static ::lf::bench::Registrar lf_bench_reg_##name(#name, limitMs, &lf_bench_##name);                     \
    static double lf_bench_##name()
