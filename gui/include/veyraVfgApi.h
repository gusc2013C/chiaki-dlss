#pragma once

#include <cstdint>
#include <windows.h>

constexpr uint32_t VeyraVfgAbi = 4;

struct VeyraVfgConfig {
    uint64_t adapter_luid;
    uint32_t width;
    uint32_t height;
    uint32_t quality;
    uint32_t hdr10;
    const wchar_t *runtime_directory;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t video_sr_quality;
    const char *project_id;
    const char *engine_version;
    const wchar_t *video_sr_vfg_runtime;
    uint32_t video_sr_vfg_quality;
};

struct VeyraVfgShared {
    // Borrowed until destroy(). Importing into Vulkan does not consume these handles.
    HANDLE input;
    HANDLE output;
    HANDLE input_released;
    HANDLE processing_done;
    HANDLE flow_input;
    HANDLE midpoint;
};

enum class VeyraVfgResult : int {
    Failed = -1,
    HistoryOnly = 0,
    OutputReady = 1,
};

struct VeyraVfgApi {
    void *(*create)(const VeyraVfgConfig *, VeyraVfgShared *);
    // D3D12 waits for input_released and signals processing_done at value on
    // every non-fatal return. A reset captures one real frame as new history.
    // With combined spatial SR and VFG, HistoryOnly still delivers the real
    // upscaled frame and OutputReady also delivers the generated midpoint.
    VeyraVfgResult (*submit)(void *, uint64_t value, bool reset);
    void (*destroy)(void *);
};

using VeyraVfgGetApi = const VeyraVfgApi *(*)(uint32_t);
