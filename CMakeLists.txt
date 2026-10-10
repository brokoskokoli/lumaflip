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

# ---- Sparkle 2: automatic updates (F-MAC-08, SPEC 10.1) ----
# AUTO: on if tools/fetch_sparkle.sh has put the framework into third_party/Sparkle,
# otherwise off (the app builds and runs without it: updater disabled, menu item hidden).
set(LF_ENABLE_SPARKLE "AUTO" CACHE STRING "Sparkle updater (F-MAC-08): AUTO, ON or OFF")
set_property(CACHE LF_ENABLE_SPARKLE PROPERTY STRINGS AUTO ON OFF)
set(LF_SPARKLE_DIR "${PROJECT_SOURCE_DIR}/third_party/Sparkle" CACHE PATH
    "Directory with Sparkle.framework (tools/fetch_sparkle.sh)")
# EdDSA public key (base64, from Sparkle's generate_keys; docs/RELEASING.md). Empty: Sparkle
# is linked, but the bundle gets no feed/key and the updater stays disabled at runtime.
set(LF_SPARKLE_PUBLIC_KEY "" CACHE STRING "Sparkle EdDSA public key (SUPublicEDKey)")
set(LF_SPARKLE_FEED_URL "https://brokoskokoli.github.io/lumaflip/appcast.xml" CACHE STRING
    "Sparkle appcast URL (SUFeedURL)")
set(LF_SPARKLE OFF)
if(APPLE AND NOT LF_GENERIC_PLATFORM AND LF_BUILD_UI)
  set(_lf_sparkle_fw "${LF_SPARKLE_DIR}/Sparkle.framework")
  if(LF_ENABLE_SPARKLE STREQUAL "AUTO")
    if(EXISTS "${_lf_sparkle_fw}/Versions/B/Sparkle")
      set(LF_SPARKLE ON)
    else()
      message(STATUS "Sparkle not found in ${LF_SPARKLE_DIR} (run tools/fetch_sparkle.sh): "
                     "building without automatic updates")
    endif()
  elseif(LF_ENABLE_SPARKLE)
    if(NOT EXISTS "${_lf_sparkle_fw}/Versions/B/Sparkle")
      message(FATAL_ERROR "LF_ENABLE_SPARKLE=ON, but ${_lf_sparkle_fw} is missing - run tools/fetch_sparkle.sh")
    endif()
    set(LF_SPARKLE ON)
  endif()
endif()
if(LF_SPARKLE)
  string(LENGTH "${LF_SPARKLE_PUBLIC_KEY}" _lf_key_length)
  if(LF_SPARKLE_PUBLIC_KEY STREQUAL "")
    message(STATUS "Sparkle: on, but LF_SPARKLE_PUBLIC_KEY is empty - updater disabled in this build")
  elseif(NOT _lf_key_length EQUAL 44 OR NOT LF_SPARKLE_PUBLIC_KEY MATCHES "^[A-Za-z0-9+/]+=$")
    message(FATAL_ERROR "LF_SPARKLE_PUBLIC_KEY is not a base64 EdDSA public key (44 characters)")
  elseif(NOT LF_SPARKLE_FEED_URL MATCHES "^https://")
    message(FATAL_ERROR "LF_SPARKLE_FEED_URL must be an https URL")
  else()
    message(STATUS "Sparkle: on, feed ${LF_SPARKLE_FEED_URL}")
  endif()
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
