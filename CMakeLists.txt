# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(Lumaflip VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Concurrent Svg ShaderTools Test)
qt_standard_project_setup()

# Common warning flags for our own targets only.
add_library(lf_warnings INTERFACE)
target_compile_options(lf_warnings INTERFACE -Wall -Wextra -Wpedantic -Wno-gnu-zero-variadic-macro-arguments)
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
