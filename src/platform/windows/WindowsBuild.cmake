# SPDX-License-Identifier: GPL-3.0-or-later
# Windows build parts (included from src/CMakeLists.txt when WIN32): the app icon, the
# version resources of Lumaflip.exe and lumaflip-decoder.exe and the application manifest.

# ---- app icon: rendered from the SVG at build time (no binary icon in the repository) ----
add_executable(lf_make_ico "${CMAKE_CURRENT_LIST_DIR}/tools/MakeIco.cpp")
target_link_libraries(lf_make_ico PRIVATE Qt6::Gui Qt6::Svg)
set(LF_WINDOWS_ICON "${PROJECT_BINARY_DIR}/packaging/Lumaflip.ico" CACHE INTERNAL "app icon (.ico)")
set(_lf_icon_svg "${PROJECT_SOURCE_DIR}/packaging/macos/icon/lumaflip.svg")
# The tool runs from the build tree: Qt's DLLs come from Qt's bin directory.
add_custom_command(OUTPUT "${LF_WINDOWS_ICON}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/packaging"
  COMMAND ${CMAKE_COMMAND} "-DLF_QT_BIN=$<TARGET_FILE_DIR:Qt6::Core>" "-DLF_TOOL=$<TARGET_FILE:lf_make_ico>"
          "-DLF_ARGS=${_lf_icon_svg}|${LF_WINDOWS_ICON}" -P "${CMAKE_CURRENT_LIST_DIR}/RunWithQt.cmake"
  DEPENDS lf_make_ico "${_lf_icon_svg}"
  COMMENT "Rendering Lumaflip.ico"
  VERBATIM)
add_custom_target(lf_windows_icon DEPENDS "${LF_WINDOWS_ICON}")

# ---- version resources ----
set(LF_WINDOWS_MANIFEST "${PROJECT_SOURCE_DIR}/packaging/windows/Lumaflip.manifest" CACHE INTERNAL "")
function(lf_windows_rc name description icon out)
  set(LF_RC_NAME "${name}")
  set(LF_RC_DESCRIPTION "${description}")
  if(icon)
    file(TO_CMAKE_PATH "${icon}" _icon)
    set(LF_RC_ICON "IDI_ICON1 ICON \"${_icon}\"")
  else()
    set(LF_RC_ICON "")
  endif()
  configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/version.rc.in" "${out}" @ONLY)
endfunction()
lf_windows_rc(Lumaflip "Lumaflip image viewer" "${LF_WINDOWS_ICON}"
              "${CMAKE_CURRENT_BINARY_DIR}/Lumaflip.rc")
lf_windows_rc(lumaflip-decoder "Lumaflip decoder helper" ""
              "${CMAKE_CURRENT_BINARY_DIR}/lumaflip-decoder.rc")
# The .rc file of the app reads the generated icon.
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/Lumaflip.rc" PROPERTIES
  OBJECT_DEPENDS "${LF_WINDOWS_ICON}")
