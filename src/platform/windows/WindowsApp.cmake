# SPDX-License-Identifier: GPL-3.0-or-later
# Lumaflip.exe (included from src/CMakeLists.txt on Windows, instead of the macOS bundle).
# Layout of the build directory = layout of the installation (tools/windows_deploy.py adds
# Qt via windeployqt and the licence texts):
#   Lumaflip.exe, lumaflip-decoder.exe, translations/lumaflip_<lang>.qm, licenses/

qt_add_executable(Lumaflip WIN32 app/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/Lumaflip.rc"
                  "${LF_WINDOWS_MANIFEST}")
add_dependencies(Lumaflip lf_lrelease lf_windows_icon)
target_link_libraries(Lumaflip PRIVATE lf_ui lf_warnings)
target_compile_definitions(Lumaflip PRIVATE LF_APP_VERSION="${PROJECT_VERSION}")
set_target_properties(Lumaflip PROPERTIES OUTPUT_NAME "${LF_APP_NAME}")

# The decoder helper and the translations next to the executable.
if(LF_DECODER_HELPER_TARGET)
  add_dependencies(Lumaflip ${LF_DECODER_HELPER_TARGET})
  add_custom_command(TARGET Lumaflip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${LF_DECODER_HELPER_TARGET}>"
            "$<TARGET_FILE_DIR:Lumaflip>/lumaflip-decoder.exe"
    COMMENT "Copying lumaflip-decoder.exe next to Lumaflip.exe"
    VERBATIM)
endif()
if(LF_QM_FILES)
  add_custom_command(TARGET Lumaflip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Lumaflip>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LF_QM_FILES} "$<TARGET_FILE_DIR:Lumaflip>/translations/"
    COMMENT "Copying translations next to Lumaflip.exe"
    VERBATIM)
endif()
