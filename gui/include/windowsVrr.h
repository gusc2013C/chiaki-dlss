#pragma once

#include <windows.h>

struct WindowsVrrState {
    bool available = false;
    bool possible = false;
    bool requested = false;
    bool enabled = false;
    bool active = false;
};

WindowsVrrState queryWindowsVrr(HWND window);
