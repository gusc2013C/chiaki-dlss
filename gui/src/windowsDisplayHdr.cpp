#include "windowsDisplayHdr.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

WindowsDisplayHdr queryWindowsDisplayHdr(HWND window)
{
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return {};

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};

    for (UINT adapter_index = 0; ; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(adapter_index, &adapter)))
            break;
        for (UINT output_index = 0; ; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(output_index, &output)))
                break;
            DXGI_OUTPUT_DESC base{};
            if (FAILED(output->GetDesc(&base)) || base.Monitor != monitor)
                continue;

            ComPtr<IDXGIOutput6> advanced_output;
            DXGI_OUTPUT_DESC1 display{};
            if (FAILED(output.As(&advanced_output)) || FAILED(advanced_output->GetDesc1(&display)))
                return {};
            if (display.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                !std::isfinite(display.MaxLuminance) ||
                display.MaxLuminance < 80.0f || display.MaxLuminance > 10000.0f)
                return {};

            const float black = std::isfinite(display.MinLuminance) &&
                                display.MinLuminance >= 0.0f && display.MinLuminance < display.MaxLuminance
                ? display.MinLuminance : 0.0f;
            return {display.MaxLuminance, black};
        }
    }
    return {};
}
