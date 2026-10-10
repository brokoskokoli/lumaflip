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
    if(mac_includes)
      list(REMOVE_ITEM LF_UNIT_TESTS ${src})
    endif()
  endforeach()
else()
  file(GLOB LF_UNIT_TESTS CONFIGURE_DEPENDS unit/*.cpp unit/*.mm)
endif()
add_executable(lf_unit_tests ${LF_UNIT_TESTS})
target_link_libraries(lf_unit_tests PRIVATE lf_core GTest::gtest_main lf_warnings)
target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                  LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
if(APPLE)
  target_compile_options(lf_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()
gtest_discover_tests(lf_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)

if(NOT LF_BUILD_UI)
  return() # integration tests and benchmarks need lf_ui
endif()

# Integration tests: each tests/integration/*.cpp is a Qt Test executable linking the UI.
file(GLOB LF_INTEGRATION_TESTS CONFIGURE_DEPENDS integration/tst_*.cpp)
foreach(src ${LF_INTEGRATION_TESTS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lf_ui Qt6::Test lf_warnings)
  target_compile_definitions(${name} PRIVATE LF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                             LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS integration)
  if(LF_SANITIZE)
    # ASan/UBSan make the GUI thread several times slower (P-05 blocking check).
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "LF_UI_GAP_LIMIT_MS=600")
  endif()
endforeach()

# Benchmarks (P-xx)
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
