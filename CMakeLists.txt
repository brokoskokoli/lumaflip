# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

# Must be set before project() to take effect (SPEC: macOS 15+).
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)  # an older cache may hold an empty value
  set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
project(Lumaflip VERSION 1.2.2 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()
if(WIN32)
  enable_language(RC) # version resource and icon of the Windows executables
endif()

set(LF_APP_NAME "Lumaflip")
set(LF_BUNDLE_ID "io.github.brokoskokoli.lumaflip")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(LF_WERROR "Treat warnings in own sources as errors" ON)
option(LF_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(LF_BUILD_TESTS "Build tests" ON)
option(LF_BUILD_FUZZERS "Build the fuzz targets in tests/fuzz (libFuzzer or standalone driver)" OFF)
# Release builds: fail instead of silently dropping lossless JPEG rotation (F-EDIT-02) when
# libjpeg-turbo's static TurboJPEG library (libturbojpeg.a) is missing (src/CMakeLists.txt).
option(LF_REQUIRE_TURBOJPEG "Require the static libjpeg-turbo TurboJPEG library" OFF)
# The app (widgets, QRhi canvas, bundle) and its integration tests/benchmarks: macOS, Linux
# and Windows (M4, SPEC 9.3/13). Other systems build the platform-neutral core only for now.
if(APPLE OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(LF_BUILD_UI "Build the app, its UI library, integration tests and benchmarks" ON)
else()
  option(LF_BUILD_UI "Build the app, its UI library, integration tests and benchmarks" OFF)
endif()
# Testing only: build src/platform/generic (and src/app/generic) instead of the macOS
# platform layer on macOS, to compile and unit-test the portable fallbacks locally.
option(LF_FORCE_GENERIC_PLATFORM "Use the generic platform layer on macOS (testing only)" OFF)
if(NOT APPLE OR LF_FORCE_GENERIC_PLATFORM)
  set(LF_GENERIC_PLATFORM ON)
else()
  set(LF_GENERIC_PLATFORM OFF)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LF_LINUX ON)
else()
  set(LF_LINUX OFF)
endif()
# Windows: the generic layer plus the native parts in src/platform/windows (which replace
# their generic counterparts, src/CMakeLists.txt). LF_GENERIC_PLATFORM stays on there: the
# code and tests use it for "not the macOS platform layer".
if(WIN32 AND NOT LF_FORCE_GENERIC_PLATFORM)
  set(LF_WINDOWS_PLATFORM ON)
else()
  set(LF_WINDOWS_PLATFORM OFF)
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

# ---- WinSparkle: automatic updates of the Windows build (F-MAC-08, docs/WINDOWS.md) ----
# AUTO: on if tools/fetch_winsparkle.py has put WinSparkle.dll into third_party/WinSparkle.
# Nothing is linked: the app loads WinSparkle.dll from its own folder at run time, the build
# only copies it next to Lumaflip.exe. The EdDSA public key is the one of the macOS build
# (LF_SPARKLE_PUBLIC_KEY, compiled in); without it the updater stays disabled, as on macOS.
# The feed is a separate appcast, so Sparkle on macOS never sees Windows items.
set(LF_ENABLE_WINSPARKLE "AUTO" CACHE STRING "WinSparkle updater (Windows): AUTO, ON or OFF")
set_property(CACHE LF_ENABLE_WINSPARKLE PROPERTY STRINGS AUTO ON OFF)
set(LF_WINSPARKLE_DIR "${PROJECT_SOURCE_DIR}/third_party/WinSparkle" CACHE PATH
    "Directory with x64/WinSparkle.dll (tools/fetch_winsparkle.py)")
set(LF_WINSPARKLE_FEED_URL "https://brokoskokoli.github.io/lumaflip/appcast-windows.xml" CACHE STRING
    "WinSparkle appcast URL")
set(LF_WINSPARKLE OFF)
if(WIN32 AND LF_BUILD_UI)
  set(LF_WINSPARKLE_DLL "${LF_WINSPARKLE_DIR}/x64/WinSparkle.dll")
  if(LF_ENABLE_WINSPARKLE STREQUAL "AUTO")
    if(EXISTS "${LF_WINSPARKLE_DLL}")
      set(LF_WINSPARKLE ON)
    else()
      message(STATUS "WinSparkle not found in ${LF_WINSPARKLE_DIR} (run tools/fetch_winsparkle.py): "
                     "building without automatic updates")
    endif()
  elseif(LF_ENABLE_WINSPARKLE)
    if(NOT EXISTS "${LF_WINSPARKLE_DLL}")
      message(FATAL_ERROR "LF_ENABLE_WINSPARKLE=ON, but ${LF_WINSPARKLE_DLL} is missing - run tools/fetch_winsparkle.py")
    endif()
    set(LF_WINSPARKLE ON)
  endif()
endif()
if(LF_WINSPARKLE)
  string(LENGTH "${LF_SPARKLE_PUBLIC_KEY}" _lf_key_length)
  if(LF_SPARKLE_PUBLIC_KEY STREQUAL "")
    message(STATUS "WinSparkle: on, but LF_SPARKLE_PUBLIC_KEY is empty - updater disabled in this build")
  elseif(NOT _lf_key_length EQUAL 44 OR NOT LF_SPARKLE_PUBLIC_KEY MATCHES "^[A-Za-z0-9+/]+=$")
    message(FATAL_ERROR "LF_SPARKLE_PUBLIC_KEY is not a base64 EdDSA public key (44 characters)")
  elseif(NOT LF_WINSPARKLE_FEED_URL MATCHES "^https://")
    message(FATAL_ERROR "LF_WINSPARKLE_FEED_URL must be an https URL")
  else()
    message(STATUS "WinSparkle: on, feed ${LF_WINSPARKLE_FEED_URL}")
  endif()
endif()

set(LF_QT_COMPONENTS Core Gui Concurrent Svg)
if(LF_BUILD_UI)
  list(APPEND LF_QT_COMPONENTS GuiPrivate Widgets ShaderTools Test)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS ${LF_QT_COMPONENTS})
# Freedesktop integration (src/platform/generic/XdgShell.cpp): "show in file manager" via
# org.freedesktop.FileManager1. Optional: without QtDBus the folder is opened instead.
find_package(Qt6 QUIET COMPONENTS DBus)
qt_standard_project_setup()
# Translations (SPEC 4.16, translations/README.md): lupdate/lrelease. Required for the app
# (Homebrew's qt has them); optional for the core-only build (Linux CI), where the unit
# tests that read the .qm files skip without them.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(LF_BUILD_UI AND NOT Qt6LinguistTools_FOUND)
  message(FATAL_ERROR "Qt LinguistTools (lupdate/lrelease) not found - they are part of Homebrew's qt")
endif()

# Common warning flags for our own targets only.
add_library(lf_warnings INTERFACE)
if(MSVC)
  # MSVC (and clang-cl): /W4 is the counterpart of -Wall -Wextra. Warnings that clang only
  # reports with flags the project does not use are switched off, so both compilers
  # enforce the same rules: C4244/C4267/C4305/C4310 (narrowing, -Wconversion), C4127
  # (constant condition), C4456-C4459 (shadowing, -Wshadow), C4324 (padding for alignas),
  # C4702 (unreachable code, -Wunreachable-code: platform #ifdef branches return early).
  target_compile_options(lf_warnings INTERFACE /W4 /wd4244 /wd4267 /wd4305 /wd4310 /wd4127 /wd4456
                                               /wd4457 /wd4458 /wd4459 /wd4324 /wd4702)
  if(LF_WERROR)
    target_compile_options(lf_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(lf_warnings INTERFACE -Wall -Wextra -Wpedantic
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-gnu-zero-variadic-macro-arguments>)
  if(LF_WERROR)
    target_compile_options(lf_warnings INTERFACE -Werror)
  endif()
endif()
if(WIN32)
  # Windows 10 API level, Unicode Win32 API, no min/max macros; sources are UTF-8 (German
  # UI texts). Applies to every target (also the tests).
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006 UNICODE _UNICODE
                          NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus /permissive- /EHsc /bigobj)
  endif()
endif()
if(LF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)

# Screenshot tool for the README and the website (tools/screenshots, macOS only).
option(LF_BUILD_SCREENSHOTS "Build the screenshot tool lf_screenshots (macOS)" ON)
if(LF_BUILD_SCREENSHOTS AND LF_BUILD_UI AND APPLE AND NOT LF_GENERIC_PLATFORM)
  add_subdirectory(tools/screenshots)
endif()

if(LF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Fuzz targets (SPEC 8.5), see tests/fuzz/CMakeLists.txt.
if(LF_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
