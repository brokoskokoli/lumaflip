// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows display information for color management and HDR (F-COL-01/02/03).
//
// `monitor` is an HMONITOR (void* keeps <windows.h> out of this header).

#include <QString>

namespace lf::win {

// Path of the ICC profile Windows color management assigns to the monitor (the default
// profile of the display, GetICMProfileW); empty if there is none.
QString monitorIccProfilePath(void* monitor);

struct MonitorColorInfo {
    bool valid = false;         // the display path was found
    bool advancedColor = false; // HDR or Auto Color Management: the compositor works in scRGB
    bool hdr = false;           // HDR output (DXGI output in BT.2100 PQ)
    float maxLuminance = 0.0f;  // nits, peak brightness reported by the display (DXGI)
    float sdrWhiteNits = 80.0f; // nits of SDR white ("SDR content brightness" slider)
};

// Advanced color state of the monitor (DisplayConfig + DXGI).
MonitorColorInfo monitorColorInfo(void* monitor);
inline bool monitorAdvancedColor(void* monitor) {
    return monitorColorInfo(monitor).advancedColor;
}
// Extended range headroom of an HDR monitor: peak luminance relative to SDR white (>= 1).
float monitorHeadroom(const MonitorColorInfo& info);

} // namespace lf::win
