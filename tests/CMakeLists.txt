# SPDX-License-Identifier: GPL-3.0-or-later
find_package(GTest REQUIRED)
include(GoogleTest)

# Unit tests: every tests/unit/*.cpp goes into one GoogleTest binary.
file(GLOB LF_UNIT_TESTS CONFIGURE_DEPENDS unit/*.cpp unit/*.mm)
add_executable(lf_unit_tests ${LF_UNIT_TESTS})
target_link_libraries(lf_unit_tests PRIVATE lf_core GTest::gtest_main lf_warnings)
target_compile_definitions(lf_unit_tests PRIVATE LF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                                                  LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
target_compile_options(lf_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
gtest_discover_tests(lf_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)

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
endforeach()

# Benchmarks (P-xx)
file(GLOB LF_BENCH_SOURCES CONFIGURE_DEPENDS bench/*.cpp)
if(LF_BENCH_SOURCES)
  add_executable(lf_bench ${LF_BENCH_SOURCES})
  target_link_libraries(lf_bench PRIVATE lf_ui lf_warnings)
  target_compile_definitions(lf_bench PRIVATE LF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
endif()
