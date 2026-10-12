# SPDX-License-Identifier: GPL-3.0-or-later
# lf_screenshots: takes the README/website screenshots and the frames of the demo animation
# from real windows (docs/RELEASING.md "Screenshots", .github/workflows/screenshots.yml).
# macOS only (it draws the macOS window frame and takes MapKit's map from the window server).
# Built with the app, so CI notices when the UI it drives changes; never run by ctest (it
# opens windows). The pure helpers (ShotFrame.cpp) are unit-tested in tests/unit.
add_executable(lf_screenshots main.cpp ShotDriver.cpp ShotDriver.h ShotFrame.cpp ShotFrame.h
                              NativeCapture.h NativeCapture_mac.mm)
target_link_libraries(lf_screenshots PRIVATE lf_ui Qt6::Test lf_warnings "-framework AppKit")
target_compile_options(lf_screenshots PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
