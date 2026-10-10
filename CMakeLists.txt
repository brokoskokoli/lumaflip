# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(Lumaflip VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(LF_APP_NAME "Lumaflip")
set(LF_BUNDLE_ID "io.github.brokoskokoli.lumaflip")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
set(CMAKE_AUTOMOC ON)

option(LF_WERROR "Treat warnings in own sources as errors" ON)
option(LF_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(LF_BUILD_TESTS "Build tests" ON)
# The app (widgets, QRhi canvas, bundle) and its integration tests/benchmarks.
# Off on Linux for now: there only the platform-neutral core is built and
# unit-tested (SPEC 9.3).
if(APPLE)
  option(LF_BUILD_UI "Build the app, its UI library, integration tests and benchmarks" ON)
else()
  option(LF_BUILD_UI "Build the app, its UI library, integration tests and benchmarks" OFF)
endif()
# Testing only: build src/platform/generic instead of src/platform/mac on macOS, to
# compile and unit-test the portable fallbacks locally. Implies LF_BUILD_UI=OFF.
option(LF_FORCE_GENERIC_PLATFORM "Use the generic platform layer on macOS (testing only)" OFF)
if(NOT APPLE OR LF_FORCE_GENERIC_PLATFORM)
  set(LF_GENERIC_PLATFORM ON)
else()
  set(LF_GENERIC_PLATFORM OFF)
endif()
if(LF_FORCE_GENERIC_PLATFORM AND LF_BUILD_UI)
  message(STATUS "LF_FORCE_GENERIC_PLATFORM: not building the UI (LF_BUILD_UI=OFF)")
  set(LF_BUILD_UI OFF CACHE BOOL "" FORCE)
endif()

set(LF_QT_COMPONENTS Core Gui Concurrent Svg)
if(LF_BUILD_UI)
  list(APPEND LF_QT_COMPONENTS GuiPrivate Widgets ShaderTools Test)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS ${LF_QT_COMPONENTS})
qt_standard_project_setup()

# Common warning flags for our own targets only.
add_library(lf_warnings INTERFACE)
target_compile_options(lf_warnings INTERFACE -Wall -Wextra -Wpedantic
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-gnu-zero-variadic-macro-arguments>)
if(LF_WERROR)
  target_compile_options(lf_warnings INTERFACE -Werror)
endif()
if(LF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)

if(LF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
