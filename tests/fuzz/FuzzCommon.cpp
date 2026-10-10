// SPDX-License-Identifier: GPL-3.0-or-later
#include "FuzzTarget.h"

#include <cstdio>
#include <cstdlib>

[[noreturn]] void lf::fuzz::fail(const char* what) {
    std::fprintf(stderr, "lf fuzz: invariant failed: %s\n", what);
    std::abort();
}
