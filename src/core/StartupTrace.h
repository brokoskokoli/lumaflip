// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Startup profiling (P-01). With the environment variable LF_TRACE_STARTUP=1 every
// trace point prints "LF_TRACE <ms since process start> <label>" to stderr; otherwise a
// trace point costs one static bool check. Thread-safe.

namespace lf::trace {

bool enabled();
// Milliseconds since the kernel started this process (includes dyld and static init).
double msSinceProcessStart();
void mark(const char* label);
// Like mark(), but only the first call with this label (pointer) prints.
void once(const char* label);

} // namespace lf::trace
