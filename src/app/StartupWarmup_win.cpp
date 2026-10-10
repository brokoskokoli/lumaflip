// SPDX-License-Identifier: GPL-3.0-or-later
#include "StartupWarmup.h"

#include "core/StartupTrace.h"

#include <thread>

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

namespace lf::startup {

void warmUpGpu() {
    // Enumerating the adapters loads the D3D/DXGI user mode driver DLLs; QRhi creates its
    // device later on the GUI thread and finds them loaded.
    std::thread([] {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            factory->EnumAdapters1(0, &adapter);
        }
        trace::mark("warm-up: DXGI ready");
    }).detach();
}

} // namespace lf::startup
