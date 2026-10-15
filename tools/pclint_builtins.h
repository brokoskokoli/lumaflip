// SPDX-License-Identifier: GPL-3.0-or-later
// Pre-included by tools/pclint.sh: compiler builtins that the libc++ of current clang uses
// and PC-lint Plus 2025 does not know yet. Only for the analysis, never compiled.
#pragma once

template <class T>
constexpr int __builtin_clzg(T x, int ifZero = 0) {
    if (!x)
        return ifZero;
    int n = 0;
    for (T m = T(T(1) << (sizeof(T) * 8 - 1)); !(x & m); m = T(m >> 1))
        ++n;
    return n;
}

template <class T>
constexpr int __builtin_ctzg(T x, int ifZero = 0) {
    if (!x)
        return ifZero;
    int n = 0;
    for (; !(x & T(1)); x = T(x >> 1))
        ++n;
    return n;
}

template <class T>
constexpr int __builtin_popcountg(T x) {
    int n = 0;
    for (; x; x = T(x & T(x - 1)))
        ++n;
    return n;
}

#define __is_nothrow_convertible(From, To) __is_convertible(From, To)

// PC-lint Plus parses for a target without NEON: keep Qt away from arm_neon.h and __fp16.
#undef __ARM_NEON
#undef __ARM_NEON__
#undef __ARM_NEON_FP
#undef __ARM_FP16_FORMAT_IEEE
#undef __ARM_FP16_ARGS
#undef __ARM_FEATURE_FP16_SCALAR_ARITHMETIC
#undef __ARM_FEATURE_FP16_VECTOR_ARITHMETIC
