#pragma once

#include "veyraVfgApi.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/ngx/DlssSrBackend.h"
#include "veyra/ngx/NgxCoreHost.h"
#include "veyra/ngx/NvOfSession.h"
#include "veyra/pipeline/GpuPassUtils.h"

class VeyraSr {
public:
    ~VeyraSr();
    bool initialize(const VeyraVfgConfig &, veyra::gfx::D3D12DeviceContext &,
                    veyra::gfx::CommandSlotRing &, veyra::gfx::ComPtr<ID3D12Resource> flow_input);
    bool process(veyra::gfx::CommandSlotRing &, ID3D12Resource *color, ID3D12Resource *output,
                 uint64_t value, bool reset);

private:
    veyra::ngx::NgxCoreHost core;
    veyra::ngx::DlssSrBackend sr;
    veyra::ngx::NvOfSession flow;
    veyra::pipeline::ComputePass densify;
    veyra::pipeline::StateTracker states;
    veyra::gfx::ComPtr<ID3D12Resource> previous, current, vectors, cost, motion, depth;
    veyra::gfx::ComPtr<ID3D12Fence> flow_input_ready, flow_done;
    NVSDK_NGX_Parameter *parameters = nullptr;
    unsigned width = 0, height = 0;
    float scale_x = 1.0f, scale_y = 1.0f;
    bool history = false;
};
