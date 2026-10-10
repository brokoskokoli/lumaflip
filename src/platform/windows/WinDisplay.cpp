// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/WinDisplay.h"

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace lf::win {

namespace {

bool monitorDevice(HMONITOR monitor, std::wstring* device) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!monitor || !GetMonitorInfoW(monitor, &mi))
        return false;
    *device = mi.szDevice;
    return true;
}

// The DisplayConfig path of the GDI device `device` ("\\.\DISPLAY1").
bool displayPath(const std::wstring& device, DISPLAYCONFIG_PATH_INFO* out) {
    UINT32 pathCount = 0, modeCount = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return false;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                           modes.data(), nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER)
            continue; // the configuration changed in between
        if (rc != ERROR_SUCCESS)
            return false;
        for (UINT32 i = 0; i < pathCount; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof source;
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                device == source.viewGdiDeviceName) {
                *out = paths[i];
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace

QString monitorIccProfilePath(void* monitor) {
    std::wstring device;
    if (!monitorDevice(static_cast<HMONITOR>(monitor), &device))
        return {};
    HDC dc = CreateDCW(L"DISPLAY", device.c_str(), nullptr, nullptr);
    if (!dc)
        return {};
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = GetICMProfileW(dc, &size, path);
    DeleteDC(dc);
    return ok ? QString::fromWCharArray(path) : QString();
}

MonitorColorInfo monitorColorInfo(void* monitorHandle) {
    MonitorColorInfo info;
    const auto monitor = static_cast<HMONITOR>(monitorHandle);
    std::wstring device;
    if (!monitorDevice(monitor, &device))
        return info;
    DISPLAYCONFIG_PATH_INFO path{};
    if (displayPath(device, &path)) {
        info.valid = true;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof color;
        color.header.adapterId = path.targetInfo.adapterId;
        color.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS)
            info.advancedColor = color.advancedColorEnabled != 0;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        // SDRWhiteLevel: multiplier of 80 nits, times 1000.
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            info.sdrWhiteNits = float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    // DXGI: the output's color space and peak luminance.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return info;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor)
                continue;
            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6))) {
                DXGI_OUTPUT_DESC1 d1{};
                if (SUCCEEDED(output6->GetDesc1(&d1))) {
                    info.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    info.maxLuminance = d1.MaxLuminance;
                    if (info.hdr)
                        info.advancedColor = true;
                }
            }
            return info;
        }
    }
    return info;
}

float monitorHeadroom(const MonitorColorInfo& info) {
    if (!info.hdr || info.maxLuminance <= 0.0f || info.sdrWhiteNits <= 0.0f)
        return 1.0f;
    return std::max(1.0f, info.maxLuminance / info.sdrWhiteNits);
}

} // namespace lf::win
