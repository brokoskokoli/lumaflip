// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fuzz targets (SPEC 8.5): every tests/fuzz/fuzz_*.cpp defines the libFuzzer entry point
// and a few built-in seeds. Built with clang's libFuzzer when available
// (-fsanitize=fuzzer), otherwise with the standalone mutation driver (FuzzDriver.cpp).
// See tests/fuzz/CMakeLists.txt.

#include <QByteArray>
#include <QList>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace lf::fuzz {

// Built-in seeds of the target (the standalone driver mutates them; `-write_seeds=DIR`
// exports them as a corpus for libFuzzer).
QList<QByteArray> seeds();

inline QByteArray bytes(const uint8_t* data, size_t size) {
    return QByteArray(reinterpret_cast<const char*>(data), qsizetype(size));
}

// A failed invariant: abort so the driver/libFuzzer keeps the input.
[[noreturn]] void fail(const char* what);

} // namespace lf::fuzz
