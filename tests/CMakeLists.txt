# SPDX-License-Identifier: GPL-3.0-or-later
find_package(GTest REQUIRED)
include(GoogleTest)

# Unit tests: every tests/unit/*.cpp goes into one GoogleTest binary.
# With the generic platform (Linux, LF_FORCE_GENERIC_PLATFORM) the Objective-C++
# tests and every test that includes a macOS platform header are left out.
if(LF_GENERIC_PLATFORM)
  file(GLOB LF_UNIT_TESTS CONFIGURE_DEPENDS unit/*.cpp)
  foreach(src ${LF_UNIT_TESTS})
    file(STRINGS ${src} mac_includes REGEX "#include [\"<]platform/mac/")
    # Linux-only tests (inotify, statx, seccomp …) are left out of other generic builds.
    set(linux_includes "")
    if(NOT LF_LINUX)
      file(STRINGS ${src} linux_includes REGEX "#include [\"<]platform/linux/")
    endif()
    if(mac_includes OR linux_includes)
      list(REMOVE_ITEM LF_UNIT_TESTS ${src})
    endif()
  endforeach()
else()
  file(GLOB LF_UNIT_TESTS CONFIGURE_DEPENDS unit/*.cpp unit/*.mm)
  # Tests of the portable/Linux platform layers need their sources.
  foreach(src ${LF_UNIT_TESTS})
    file(STRINGS ${src} other_includes REGEX "#include [\"<]platform/(generic|linux)/")
    if(other_includes)
      list(REMOVE_ITEM LF_UNIT_TESTS ${src})
    endif()
  endforeach()
endif()
# Tests of the Windows platform layer (test_windows_*.cpp, include platform/windows) only there.
if(NOT LF_WINDOWS_PLATFORM)
  list(FILTER LF_UNIT_TESTS EXCLUDE REGEX ".*/test_windows_[^/]*\\.cpp$")
else()
  # The freedesktop (XDG) helpers are not part of the Windows build.
  foreach(src ${LF_UNIT_TESTS})
    file(STRINGS ${src} xdg_includes REGEX "#include [\"<]platform/generic/Xdg")
    if(xdg_includes)
      list(REMOVE_ITEM LF_UNIT_TESTS ${src})
    endif()
  endforeach()
endif()
add_executable(lf_unit_tests ${LF_UNIT_TESTS})
target_link_libraries(lf_unit_tests PRIVATE lf_core GTest::gtest_main lf_warnings)
target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                  LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
if(APPLE)
  target_compile_options(lf_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()
# Translation tests (F-I18N-02/05/06): the built .qm files and the .ts sources.
list(JOIN LF_UI_LANGUAGES "," _lf_ui_languages)
target_compile_definitions(lf_unit_tests PRIVATE LF_QM_DIR="${LF_QM_DIR}"
                                                  LF_TS_DIR="${LF_TRANSLATIONS_SOURCE_DIR}"
                                                  LF_SRC_DIR="${PROJECT_SOURCE_DIR}"
                                                  LF_UI_LANGUAGES="${_lf_ui_languages}")
if(TARGET lf_lrelease)
  add_dependencies(lf_unit_tests lf_lrelease)
  target_compile_definitions(lf_unit_tests PRIVATE LF_HAVE_TRANSLATIONS=1)
endif()
# Decoder helper process tests (SPEC 8.5) start the real helper binary.
if(LF_DECODER_HELPER_TARGET)
  add_dependencies(lf_unit_tests ${LF_DECODER_HELPER_TARGET})
  target_compile_definitions(lf_unit_tests PRIVATE
    LF_DECODER_HELPER_BINARY="$<TARGET_FILE:${LF_DECODER_HELPER_TARGET}>")
endif()
# Library decoder tests (test_library_decoders.cpp) encode their files with the same libraries.
foreach(_lib HEIF:LIBHEIF JXL:LIBJXL EXR:OPENEXR)
  string(REPLACE ":" ";" _pair "${_lib}")
  list(GET _pair 0 _opt)
  list(GET _pair 1 _define)
  if(TARGET PkgConfig::LF_PC_${_opt})
    target_link_libraries(lf_unit_tests PRIVATE PkgConfig::LF_PC_${_opt})
    target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_HAVE_${_define}=1)
  else()
    target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_HAVE_${_define}=0)
  endif()
endforeach()
# Lossless crop tests (test_m4_crop.cpp) encode JPEG variants (4:4:4, 4:2:2, progressive,
# restart markers) with the TurboJPEG library the core uses.
get_target_property(_lf_core_defs lf_core COMPILE_DEFINITIONS)
if("LF_HAVE_TURBOJPEG=1" IN_LIST _lf_core_defs) # found and new enough (tj3 API, src/CMakeLists.txt)
  target_include_directories(lf_unit_tests SYSTEM PRIVATE ${LF_TURBOJPEG_INCLUDE})
  target_link_libraries(lf_unit_tests PRIVATE ${LF_TURBOJPEG_LIB})
  target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_HAVE_TURBOJPEG=1)
endif()
gtest_discover_tests(lf_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)

if(NOT LF_BUILD_UI)
  return() # integration tests and benchmarks need lf_ui
endif()

# Test support for integration tests (integration/support): real native input events.
if(APPLE AND NOT LF_GENERIC_PLATFORM)
  add_library(lf_test_support STATIC integration/support/NativeEvents.h integration/support/NativeEvents_mac.mm)
  target_compile_options(lf_test_support PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(lf_test_support PUBLIC Qt6::Widgets "-framework AppKit" "-framework Carbon"
                                        PRIVATE lf_warnings)
else()
  # No native event injection (the tests that need it skip, nativeEventsAvailable()).
  add_library(lf_test_support STATIC integration/support/NativeEvents.h integration/support/NativeEvents_generic.cpp)
  target_link_libraries(lf_test_support PUBLIC Qt6::Widgets PRIVATE lf_warnings)
endif()
target_include_directories(lf_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)

# Integration tests: each tests/integration/*.cpp is a Qt Test executable linking the UI.
file(GLOB LF_INTEGRATION_TESTS CONFIGURE_DEPENDS integration/tst_*.cpp)
foreach(src ${LF_INTEGRATION_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lf_ui Qt6::Test lf_warnings)
  if(TARGET lf_test_support)
    target_link_libraries(${name} PRIVATE lf_test_support)
  endif()
  target_compile_definitions(${name} PRIVATE LF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                             LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
  add_test(NAME ${name} COMMAND ${name})
  # GUI tests use thread pools and real windows: let ctest count each as 4 CPUs so a
  # parallel run (-j NCPU) starts only a few at a time (avoids load spikes and flaky timing).
  set_tests_properties(${name} PROPERTIES LABELS integration PROCESSORS 4)
  # F-COL-03: the extended range (EDR) surface only where a test asks for it (tst_hdr): the
  # others read the canvas framebuffer, which an EDR surface would cover.
  set(_lf_env "LF_NO_EDR=1")
  if(name STREQUAL "tst_hdr")
    set(_lf_env "LF_NO_EDR=0")
  endif()
  if(LF_SANITIZE)
    # ASan/UBSan make the GUI thread several times slower (P-05 blocking check).
    list(APPEND _lf_env "LF_UI_GAP_LIMIT_MS=600")
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_lf_env}")
endforeach()

# Benchmarks (P-xx): macOS only for now (they compare against the ImageIO backend).
if(LF_GENERIC_PLATFORM)
  return()
endif()
file(GLOB LF_BENCH_SOURCES CONFIGURE_DEPENDS bench/*.cpp)
if(LF_BENCH_SOURCES)
  add_executable(lf_bench ${LF_BENCH_SOURCES})
  target_link_libraries(lf_bench PRIVATE lf_ui lf_warnings)
  target_compile_definitions(lf_bench PRIVATE LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
  if(TARGET Lumaflip)
    # P_01_ProcessLaunchToFirstFrame24MP spawns the real app.
    target_compile_definitions(lf_bench PRIVATE LF_APP_BINARY="$<TARGET_FILE:Lumaflip>")
    add_dependencies(lf_bench Lumaflip)
  endif()
endif()
