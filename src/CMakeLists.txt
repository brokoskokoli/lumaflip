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
# zlib: raw deflate of ZIP members (OpenRaster/Krita archives, core/ZipReader.cpp).
find_package(ZLIB REQUIRED)
target_link_libraries(lf_core
  PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Svg
  PRIVATE lf_warnings ZLIB::ZLIB)
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
# SVG via resvg (SPEC 4.4 "Vektor", M3; core/ResvgDecoder.h): Homebrew's libresvg, linked
# dynamically (Apache-2.0/MIT, SPEC 11; tools/macos_deploy.py bundles the dylib and its
# licence texts from the keg). AUTO: on if found; QtSvg stays the fallback either way.
set(LF_ENABLE_RESVG "AUTO" CACHE STRING "SVG via resvg: AUTO, ON or OFF")
set_property(CACHE LF_ENABLE_RESVG PROPERTY STRINGS AUTO ON OFF)
set(LF_RESVG OFF)
if(NOT LF_ENABLE_RESVG STREQUAL "OFF")
  find_path(LF_RESVG_INCLUDE resvg.h
    HINTS /opt/homebrew/opt/resvg/include /usr/local/opt/resvg/include PATH_SUFFIXES resvg)
  find_library(LF_RESVG_LIB NAMES resvg HINTS /opt/homebrew/opt/resvg/lib /usr/local/opt/resvg/lib)
  if(LF_RESVG_INCLUDE AND LF_RESVG_LIB)
    set(LF_RESVG ON)
  elseif(LF_ENABLE_RESVG STREQUAL "ON")
    message(FATAL_ERROR "LF_ENABLE_RESVG=ON, but resvg was not found (brew install resvg)")
  endif()
endif()
if(LF_RESVG)
  message(STATUS "resvg: ${LF_RESVG_LIB}")
  target_include_directories(lf_core SYSTEM PRIVATE ${LF_RESVG_INCLUDE})
  target_link_libraries(lf_core PRIVATE ${LF_RESVG_LIB})
  target_compile_definitions(lf_core PRIVATE LF_HAVE_RESVG=1)
  if(LF_RESVG_LIB MATCHES "\\.a$")
    file(REAL_PATH "${LF_RESVG_LIB}" _lf_resvg_real)
    list(APPEND LF_STATIC_COMPONENTS "{\"id\": \"resvg\", \"library\": \"${_lf_resvg_real}\"}")
  endif()
else()
  message(STATUS "resvg not found or disabled: SVG via QtSvg only (SVG Tiny 1.2)")
  target_compile_definitions(lf_core PRIVATE LF_HAVE_RESVG=0)
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

# ---- lumaflip-decoder: decoder helper process (SPEC 8.5, core/DecoderHelper.h) ----
# Runs the library decoders (Qt image plugins, QtSvg) outside the app; bundled next to the
# app binary (Contents/MacOS) and signed by tools/package.sh.
if(APPLE AND NOT LF_GENERIC_PLATFORM)
  add_executable(lumaflip-decoder helper/main.cpp)
  target_link_libraries(lumaflip-decoder PRIVATE lf_core lf_warnings)
  set(LF_DECODER_HELPER_TARGET lumaflip-decoder CACHE INTERNAL "decoder helper target")
else()
  set(LF_DECODER_HELPER_TARGET "" CACHE INTERNAL "decoder helper target")
endif()

# ---- translations (SPEC 4.16, translations/README.md) ----
# German is the source language; lumaflip_de.ts holds only its plural forms. Keep the list
# in sync with src/core/Language.cpp and tools/macos_deploy.py (UI_LANGUAGES).
set(LF_UI_LANGUAGES de en fr es it pt_BR pt_PT nl pl sv da nb fi cs ja zh_CN zh_TW ko
    CACHE INTERNAL "UI languages (F-I18N-02)")
set(LF_TRANSLATIONS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/translations" CACHE INTERNAL "")
set(LF_QM_DIR "${PROJECT_BINARY_DIR}/translations" CACHE INTERNAL "Built lumaflip_<lang>.qm files")
set(LF_QM_FILES "")
if(Qt6LinguistTools_FOUND)
  set(_lf_ts_files "")
  foreach(_lang IN LISTS LF_UI_LANGUAGES)
    if(NOT _lang STREQUAL "de")
      list(APPEND _lf_ts_files "${LF_TRANSLATIONS_SOURCE_DIR}/lumaflip_${_lang}.ts")
    endif()
  endforeach()
  # Every source under src/ (also the other platform's): one catalogue for all builds.
  file(GLOB_RECURSE LF_I18N_SOURCES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h ${CMAKE_CURRENT_SOURCE_DIR}/*.mm)
  # Updates the .ts files from the sources (never part of a normal build):
  #   cmake --build build/check --target lf_lupdate
  qt_add_lupdate(TS_FILES ${_lf_ts_files}
    PLURALS_TS_FILE "${LF_TRANSLATIONS_SOURCE_DIR}/lumaflip_de.ts"
    SOURCES ${LF_I18N_SOURCES}
    LUPDATE_TARGET lf_lupdate
    NO_GLOBAL_TARGET
    OPTIONS -source-language de -locations none -no-obsolete -silent)
  # The .qm files (part of every build): bundle (below) and tests (LF_QM_DIR).
  qt_add_lrelease(TS_FILES ${_lf_ts_files} "${LF_TRANSLATIONS_SOURCE_DIR}/lumaflip_de.ts"
    LRELEASE_TARGET lf_lrelease
    NO_GLOBAL_TARGET
    QM_OUTPUT_DIRECTORY "${LF_QM_DIR}"
    QM_FILES_OUTPUT_VARIABLE LF_QM_FILES
    OPTIONS -silent)
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
# Unbundled binaries (integration tests) find lumaflip_<lang>.qm in the build tree.
target_compile_definitions(lf_ui PRIVATE LF_TRANSLATIONS_BUILD_DIR="${LF_QM_DIR}")
add_dependencies(lf_ui lf_lrelease)
if(APPLE)
  target_compile_options(lf_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  # F-INFO-04: mini map in the info panel (MKMapSnapshotter, only after a click).
  target_link_libraries(lf_ui PUBLIC "-framework MapKit" "-framework CoreLocation")
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
# Translations into Contents/Resources/translations (Qt's own catalogues are added by
# tools/macos_deploy.py); <lang>.lproj folders with localized Info.plist texts (TCC
# prompts, document type names), so macOS offers the app's languages (per-app language in
# System Settings, CFBundleLocalizations in Info.plist.in) and native panels follow.
set_source_files_properties(${LF_QM_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/translations
                                                      GENERATED TRUE)
file(GLOB LF_LPROJ_STRINGS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/packaging/macos/lproj/*.lproj/InfoPlist.strings")
foreach(_strings IN LISTS LF_LPROJ_STRINGS)
  get_filename_component(_lproj "${_strings}" DIRECTORY)
  get_filename_component(_lproj "${_lproj}" NAME)
  set_source_files_properties(${_strings} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/${_lproj}")
endforeach()
qt_add_executable(Lumaflip MACOSX_BUNDLE app/main.cpp ${LF_APP_ICON} ${LF_STATIC_MANIFEST} ${LF_QM_FILES}
                  ${LF_LPROJ_STRINGS})
add_dependencies(Lumaflip lf_lrelease)
target_link_libraries(Lumaflip PRIVATE lf_ui lf_warnings)
target_compile_definitions(Lumaflip PRIVATE LF_APP_VERSION="${PROJECT_VERSION}")
# The decoder helper lives next to the app binary (DecoderHelperPool::defaultExecutable()).
if(LF_DECODER_HELPER_TARGET)
  add_dependencies(Lumaflip ${LF_DECODER_HELPER_TARGET})
  add_custom_command(TARGET Lumaflip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${LF_DECODER_HELPER_TARGET}>"
            "$<TARGET_BUNDLE_CONTENT_DIR:Lumaflip>/MacOS/lumaflip-decoder"
    COMMENT "Bundling lumaflip-decoder"
    VERBATIM)
endif()
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
