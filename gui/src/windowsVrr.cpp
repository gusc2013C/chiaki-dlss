#include "windowsVrr.h"

#include <cstdint>

namespace {
struct VrrInfo {
    uint32_t version;
    uint32_t flags;
    uint32_t reserved[4];
};

struct NvApi {
    using Query = void *(*)(uint32_t);
    using DisplayId = int (*)(const char *, uint32_t *);
    using GetVrr = int (*)(uint32_t, VrrInfo *);
    HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    DisplayId display_id = nullptr;
    GetVrr get_vrr = nullptr;

    NvApi()
    {
        if (!module)
            return;
        auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
        if (!query)
            return;
        auto initialize = reinterpret_cast<int (*)()>(query(0x0150e828));
        if (!initialize || initialize() != 0)
            return;
        display_id = reinterpret_cast<DisplayId>(query(0xae457190));
        get_vrr = reinterpret_cast<GetVrr>(query(0xdf8fda57));
    }
};
}

WindowsVrrState queryWindowsVrr(HWND window)
{
    static const NvApi nvapi;
    if (!nvapi.display_id || !nvapi.get_vrr || !window)
        return {};
    const auto display = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!display || !GetMonitorInfoA(display, &monitor))
        return {};
    uint32_t display_id;
    if (nvapi.display_id(monitor.szDevice, &display_id) != 0)
        return {};
    VrrInfo info{uint32_t(sizeof(VrrInfo)) | (1u << 16), 0, {}};
    if (nvapi.get_vrr(display_id, &info) != 0)
        return {};
    return {true, bool(info.flags & 2), bool(info.flags & 4), bool(info.flags & 1),
            bool(info.flags & 16)};
}
