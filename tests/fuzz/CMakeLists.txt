# SPDX-License-Identifier: GPL-3.0-or-later
# Fuzz targets for the own parsers (SPEC 8.5): XMP sidecars, the ZIP reader, the "Rest"
# format decoders and the app side of the decoder helper protocol. Only with
# -DLF_BUILD_FUZZERS=ON, best in an own build folder with sanitizers:
#
#   cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLF_BUILD_FUZZERS=ON \
#         -DLF_SANITIZE=ON -DLF_BUILD_UI=OFF -DLF_BUILD_TESTS=OFF -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
#   cmake --build build/fuzz
#   nice -n 10 build/fuzz/tests/fuzz/fuzz_xmp -max_total_time=60
#   nice -n 10 build/fuzz/tests/fuzz/fuzz_simple_decoders -max_total_time=60 tests/corpus/generated
#
# With a clang that ships libFuzzer (Homebrew llvm: -DCMAKE_CXX_COMPILER=$(brew --prefix
# llvm)/bin/clang++) the targets are real libFuzzer binaries (coverage guided). Apple's
# clang has no libFuzzer: the same targets are then linked with the standalone mutation
# driver FuzzDriver.cpp (same command line subset, no coverage feedback). A crash or
# sanitizer report leaves the input in ./crash-<target>-<pid>.

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
  "#include <cstdint>
   #include <cstddef>
   extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }"
  LF_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
if(LF_HAVE_LIBFUZZER)
  message(STATUS "Fuzz targets: libFuzzer")
else()
  message(STATUS "Fuzz targets: standalone mutation driver (compiler without libFuzzer)")
endif()

find_package(ZLIB REQUIRED)
foreach(target fuzz_xmp fuzz_zip fuzz_simple_decoders fuzz_decoder_ipc)
  add_executable(${target} ${target}.cpp FuzzCommon.cpp FuzzTarget.h)
  target_link_libraries(${target} PRIVATE lf_core lf_warnings ZLIB::ZLIB)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(LF_HAVE_LIBFUZZER)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(${target} PRIVATE FuzzDriver.cpp)
  endif()
endforeach()
