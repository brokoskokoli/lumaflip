# SPDX-License-Identifier: GPL-3.0-or-later

# ---- lf_core: platform-neutral core + platform adapters (static library) ----
file(GLOB LF_CORE_SOURCES CONFIGURE_DEPENDS core/*.cpp core/*.h)
if(NOT LF_GENERIC_PLATFORM)
  file(GLOB LF_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/mac/*.mm platform/mac/*.cpp platform/mac/*.h)
  # The Sparkle backend belongs to the UI library (below), only when Sparkle is enabled.
  list(FILTER LF_PLATFORM_SOURCES EXCLUDE REGEX ".*/SparkleUpdater\\.(mm|h)$")
else()
  file(GLOB LF_PLATFORM_SOURCES CONFIGURE_DEPENDS platform/generic/*.cpp platform/generic/*.h)
endif()
# Portable implementations that the mac build may reuse (e.g. DsStore parser) live in core/.

add_library(lf_core STATIC ${LF_CORE_SOURCES} ${LF_PLATFORM_SOURCES})
target_include_directories(lf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lf_core
  PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Svg
  PRIVATE lf_warnings)
# F-EDIT-02: lossless JPEG rotation (DCT transform) with libjpeg-turbo's TurboJPEG API,
# linked statically (BSD-style license; Homebrew's jpeg-turbo is a Qt dependency anyway).
# Without it the rotation is saved via the EXIF orientation only.
find_path(LF_TURBOJPEG_INCLUDE turbojpeg.h
  HINTS /opt/homebrew/opt/jpeg-turbo/include /usr/local/opt/jpeg-turbo/include)
find_library(LF_TURBOJPEG_LIB NAMES libturbojpeg.a turbojpeg
  HINTS /opt/homebrew/opt/jpeg-turbo/lib /usr/local/opt/jpeg-turbo/lib)
# Third-party code linked statically into the app (no dylib for tools/macos_deploy.py to
# find) is listed in Contents/Resources/static-components.json; the deploy script requires
# licence texts for every entry (STATIC_COMPONENTS there, texts in packaging/licenses/).
set(LF_STATIC_COMPONENTS "")
if(LF_TURBOJPEG_INCLUDE AND LF_TURBOJPEG_LIB)
  message(STATUS "TurboJPEG: ${LF_TURBOJPEG_LIB}")
  target_include_directories(lf_core SYSTEM PRIVATE ${LF_TURBOJPEG_INCLUDE})
  target_link_libraries(lf_core PRIVATE ${LF_TURBOJPEG_LIB})
  target_compile_definitions(lf_core PRIVATE LF_HAVE_TURBOJPEG=1)
  if(LF_TURBOJPEG_LIB MATCHES "\\.a$")
    file(REAL_PATH "${LF_TURBOJPEG_LIB}" _lf_turbojpeg_real)
    list(APPEND LF_STATIC_COMPONENTS "{\"id\": \"jpeg-turbo\", \"library\": \"${_lf_turbojpeg_real}\"}")
  endif()
else()
  message(STATUS "TurboJPEG not found: lossless JPEG rotation (F-EDIT-02, DCT) disabled")
endif()
if(LF_REQUIRE_TURBOJPEG AND NOT (LF_TURBOJPEG_INCLUDE AND LF_TURBOJPEG_LIB MATCHES "\\.a$"))
  message(FATAL_ERROR "LF_REQUIRE_TURBOJPEG: the static libturbojpeg.a and turbojpeg.h were not found "
                      "(brew install jpeg-turbo); found: '${LF_TURBOJPEG_LIB}'")
endif()
list(JOIN LF_STATIC_COMPONENTS ",\n  " _lf_static_json)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/static-components.json"
     CONTENT "[\n  ${_lf_static_json}\n]\n" @ONLY)
if(LF_GENERIC_PLATFORM)
  target_compile_definitions(lf_core PUBLIC LF_GENERIC_PLATFORM=1)
elseif(APPLE)
  target_link_libraries(lf_core PUBLIC
    "-framework Foundation" "-framework AppKit" "-framework ImageIO"
    "-framework CoreGraphics" "-framework CoreServices" "-framework UniformTypeIdentifiers"
    "-framework QuartzCore" "-framework Metal")
  target_compile_options(lf_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

if(NOT LF_BUILD_UI)
  return() # core only (Linux CI, LF_FORCE_GENERIC_PLATFORM)
endif()

# ---- lf_ui: widgets (static library so integration tests can link it) ----
file(GLOB LF_UI_SOURCES CONFIGURE_DEPENDS app/*.cpp app/*.h app/*.mm)
list(FILTER LF_UI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_library(lf_ui STATIC ${LF_UI_SOURCES})
target_link_libraries(lf_ui PUBLIC lf_core Qt6::Widgets Qt6::GuiPrivate PRIVATE lf_warnings)
target_compile_definitions(lf_ui PRIVATE LF_APP_VERSION="${PROJECT_VERSION}") # AboutDialog
if(APPLE)
  target_compile_options(lf_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()
# F-MAC-08: Sparkle 2 (see LF_ENABLE_SPARKLE in the top-level CMakeLists.txt).
if(LF_SPARKLE)
  target_sources(lf_ui PRIVATE platform/mac/SparkleUpdater.mm platform/mac/SparkleUpdater.h)
  target_compile_definitions(lf_ui PUBLIC LF_HAVE_SPARKLE=1)
  # -iframework: Sparkle's headers are third-party code (no -Werror on their warnings).
  target_compile_options(lf_ui PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:SHELL:-iframework ${LF_SPARKLE_DIR}>")
  target_link_libraries(lf_ui PUBLIC "${LF_SPARKLE_DIR}/Sparkle.framework")
else()
  target_compile_definitions(lf_ui PUBLIC LF_HAVE_SPARKLE=0)
endif()
file(GLOB LF_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/*.vert ${CMAKE_CURRENT_SOURCE_DIR}/app/shaders/*.frag)
if(LF_SHADERS)
  qt_add_shaders(lf_ui "lf_shaders" PREFIX "/shaders" BASE app/shaders FILES ${LF_SHADERS})
endif()

# ---- the app bundle ----
# Development language German (UI texts are German): Qt's finalizer writes
# QT_I18N_SOURCE_LANGUAGE (default "en") into CFBundleDevelopmentRegion of the bundle.
set(QT_I18N_SOURCE_LANGUAGE de)
# App icon (source: packaging/macos/icon/lumaflip.svg, rendered by tools/make_icon.sh).
set(LF_APP_ICON "${PROJECT_SOURCE_DIR}/packaging/macos/icon/Lumaflip.icns")
set_source_files_properties(${LF_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set(LF_STATIC_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/static-components.json")
set_source_files_properties(${LF_STATIC_MANIFEST} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
qt_add_executable(Lumaflip MACOSX_BUNDLE app/main.cpp ${LF_APP_ICON} ${LF_STATIC_MANIFEST})
target_link_libraries(Lumaflip PRIVATE lf_ui lf_warnings)
target_compile_definitions(Lumaflip PRIVATE LF_APP_VERSION="${PROJECT_VERSION}")
set_target_properties(Lumaflip PROPERTIES
  OUTPUT_NAME "${LF_APP_NAME}"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${LF_BUNDLE_ID}"
  MACOSX_BUNDLE_BUNDLE_NAME "${LF_APP_NAME}"
  MACOSX_BUNDLE_ICON_FILE "Lumaflip.icns"
  MACOSX_BUNDLE_COPYRIGHT "© 2026 brokoskokoli und Mitwirkende. Lizenz: GPL-3.0-or-later."
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

# Sparkle (F-MAC-08): Info.plist entries (feed, public key, signed feed, verification
# before extraction) only with a public key - never a feed without a key. The framework
# is embedded into Contents/Frameworks like in the release bundle (tools/package.sh).
set(LF_SPARKLE_PLIST_ENTRIES "")
if(LF_SPARKLE)
  if(NOT LF_SPARKLE_PUBLIC_KEY STREQUAL "")
    string(CONCAT LF_SPARKLE_PLIST_ENTRIES
      "<key>SUFeedURL</key><string>${LF_SPARKLE_FEED_URL}</string>\n"
      "  <key>SUPublicEDKey</key><string>${LF_SPARKLE_PUBLIC_KEY}</string>\n"
      "  <key>SURequireSignedFeed</key><true/>\n"
      "  <key>SUVerifyUpdateBeforeExtraction</key><true/>")
  endif()
  target_link_options(Lumaflip PRIVATE "LINKER:-rpath,@executable_path/../Frameworks")
  add_custom_command(TARGET Lumaflip POST_BUILD
    COMMAND ditto "${LF_SPARKLE_DIR}/Sparkle.framework"
            "$<TARGET_BUNDLE_CONTENT_DIR:Lumaflip>/Frameworks/Sparkle.framework"
    COMMENT "Embedding Sparkle.framework"
    VERBATIM)
endif()
