#pragma once

#include <windows.h>
#include <libplacebo/colorspace.h>

struct WindowsDisplayHdr
{
    // Zero peak means Windows HDR is off or the display range was unavailable.
    float peak_nits = 0.0f;
    float black_nits = 0.0f;
    pl_raw_primaries primaries{};
};

WindowsDisplayHdr queryWindowsDisplayHdr(HWND window);
