#pragma once

#include <windows.h>

struct WindowsDisplayHdr
{
    // Zero peak means Windows HDR is off or the display range was unavailable.
    float peak_nits = 0.0f;
    float black_nits = 0.0f;
};

WindowsDisplayHdr queryWindowsDisplayHdr(HWND window);
