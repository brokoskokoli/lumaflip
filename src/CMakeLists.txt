# SPDX-License-Identifier: GPL-3.0-or-later

# ---- lf_core: platform-neutral core + platform adapters (static library) ----
file(GLOB LF_CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp core/*.h)
if(APPLE)
  file(GLOB LF_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/mac/*.mm platform/mac/*.cpp platform/mac/*.h)
else()
  file(GLOB LF_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/generic/*.cpp platform/generic/*.h)
endif()
# Portable implementations that the mac build may reuse (e.g. DsStore parser) live in core/.

add_library(lf_core STATIC ${LF_CORE_SOURCES} ${LF_PLATFORM_SOURCES})
target_include_directories(lf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lf_core
  PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Svg
  PRIVATE lf_warnings)
if(APPLE)
  target_link_libraries(lf_core PUBLIC
    "-framework Foundation" "-framework AppKit" "-framework ImageIO"
    "-framework CoreGraphics" "-framework CoreServices" "-framework UniformTypeIdentifiers"
    "-framework QuartzCore" "-framework Metal")
  target_compile_options(lf_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

# ---- lf_ui: widgets (static library so integration tests can link it) ----
file(GLOB LF_UI_SOURCES CONFIGURE_DEPENDS app/*.cpp app/*.h app/*.mm)
list(FILTER LF_UI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_library(lf_ui STATIC ${LF_UI_SOURCES})
target_link_libraries(lf_ui PUBLIC lf_core Qt6::Widgets Qt6::GuiPrivate PRIVATE lf_warnings)
if(APPLE)
  target_compile_options(lf_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()
file(GLOB LF_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/*.vert ${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/*.frag)
if(LF_SHADERS)
  qt_add_shaders(lf_ui "lf_shaders" PREFIX "/shaders" BASE app/shaders FILES ${LF_SHADERS})
endif()

# ---- the app bundle ----
qt_add_executable(Lumaflip MACOSX_BUNDLE app/main.cpp)
target_link_libraries(Lumaflip PRIVATE lf_ui lf_warnings)
set_target_properties(Lumaflip PROPERTIES
  OUTPUT_NAME "${LF_APP_NAME}"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${LF_BUNDLE_ID}"
  MACOSX_BUNDLE_BUNDLE_NAME "${LF_APP_NAME}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
