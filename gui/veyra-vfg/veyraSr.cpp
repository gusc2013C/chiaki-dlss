#include "veyraSr.h"

#include <array>
#include <cstring>
#include <utility>

namespace {
using namespace veyra::pipeline;
constexpr unsigned grid = 4;
constexpr auto flow_color = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr char motion_shader[] = R"(
Texture2D<int2> raw_flow : register(t0);
Texture2D<uint> raw_cost : register(t1);
Texture2D<float4> previous : register(t2);
Texture2D<float4> current : register(t3);
RWTexture2D<float2> motion : register(u0);
RWTexture2D<float> depth : register(u1);
cbuffer Params : register(b0) {
    uint width, height, grid, reset;
    float scale_x, scale_y;
};
float luma(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }
[numthreads(16, 16, 1)]
void main(uint3 pixel : SV_DispatchThreadID) {
    if (pixel.x >= width || pixel.y >= height) return;
    float2 displacement = 0;
    if (!reset) {
        uint2 cell = pixel.xy / grid;
        float2 estimate = float2(raw_flow[cell]) / 32.0;
        int2 reference = int2(round(float2(pixel.xy) + estimate));
        if (raw_cost[cell] < 192 && all(reference >= 0) && all(reference < int2(width, height))) {
            float error = abs(luma(current[pixel.xy].rgb) - luma(previous[reference].rgb));
            displacement = estimate * (1.0 - smoothstep(0.03, 0.15, error));
        }
    }
    motion[pixel.xy] = displacement * float2(scale_x, scale_y);
    depth[pixel.xy] = 0.0;
}
)";
}

VeyraSr::~VeyraSr()
{
    veyra::Status status = veyra::Status::Ok;
    flow.unregisterAll(status);
    previous.Reset();
    current.Reset();
    vectors.Reset();
    cost.Reset();
    flow.shutdown();
    sr.release();
    if (parameters)
        core.destroyParameters(parameters);
}

bool VeyraSr::initialize(const VeyraVfgConfig &config, veyra::gfx::D3D12DeviceContext &device,
                         veyra::gfx::CommandSlotRing &commands, ComPtr<ID3D12Resource> flow_input)
{
    if (!flow_input || !config.project_id || !config.engine_version)
        return false;
    auto *d3d = device.device();
    current = std::move(flow_input);
    width = config.width;
    height = config.height;
    scale_x = float(config.output_width) / float(width);
    scale_y = float(config.output_height) / float(height);
    const unsigned grid_width = (width + grid - 1) / grid;
    const unsigned grid_height = (height + grid - 1) / grid;
    previous = makeTexture(d3d, width, height, flow_color, false);
    vectors = makeTexture(d3d, grid_width, grid_height, DXGI_FORMAT_R16G16_SINT, true);
    cost = makeTexture(d3d, grid_width, grid_height, DXGI_FORMAT_R8_UINT, true);
    motion = makeTexture(d3d, width, height, DXGI_FORMAT_R32G32_FLOAT, true);
    depth = makeTexture(d3d, width, height, DXGI_FORMAT_R32_FLOAT, true);
    if (!previous || !vectors || !cost || !motion || !depth ||
        FAILED(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flow_input_ready))) ||
        FAILED(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flow_done))))
        return false;
    veyra::Status status = veyra::Status::Ok;
    veyra::ngx::NvOfSession::Desc flow_config{};
    flow_config.width = width;
    flow_config.height = height;
    flow_config.inFence = flow_input_ready.Get();
    flow_config.outFence = flow_done.Get();
    flow_config.gridSize = grid;
    if (!flow.initialize(d3d, previous.Get(), current.Get(), vectors.Get(), cost.Get(), flow_config, status) ||
        !core.initialize(d3d, config.runtime_directory, config.project_id, config.engine_version, status))
        return false;
    parameters = core.allocateParameters(status);
    if (!parameters)
        return false;

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> shader_errors;
    if (FAILED(D3DCompile(motion_shader, sizeof(motion_shader) - 1, nullptr, nullptr, nullptr,
                          "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &shader_errors))) {
        if (shader_errors)
            veyra::log::error("sr-motion", static_cast<const char *>(shader_errors->GetBufferPointer()));
        return false;
    }
    const auto *bytes = static_cast<const uint8_t *>(bytecode->GetBufferPointer());
    if (!densify.create(d3d, {bytes, bytes + bytecode->GetBufferSize()}, 6, 4, 2, 6))
        return false;
    DescriptorStager descriptors;
    if (!descriptors.initialize(d3d, 1))
        return false;
    const std::array<ID3D12Resource *, 4> inputs{vectors.Get(), cost.Get(), previous.Get(), current.Get()};
    for (unsigned slot = 0; slot < inputs.size(); ++slot)
        descriptors.stageSrv(inputs[slot], nullptr, densify.heap.Get(), slot);
    makeUav(d3d, motion.Get(), DXGI_FORMAT_R32G32_FLOAT, cpuHandleOf(densify, 4));
    makeUav(d3d, depth.Get(), DXGI_FORMAT_R32_FLOAT, cpuHandleOf(densify, 5));

    uint32_t slot;
    auto *list = commands.acquireNext(slot, status);
    if (!list)
        return false;
    const veyra::ngx::DlssSrBackend::CreateDesc feature{
        width, height, config.output_width, config.output_height,
        NVSDK_NGX_PerfQuality_Value_MaxQuality, false, config.quality};
    if (!sr.create(core, list, parameters, feature, status)) {
        commands.discardRecording();
        return false;
    }
    return commands.submitAndSignal(slot) && commands.waitIdle();
}

bool VeyraSr::process(veyra::gfx::CommandSlotRing &commands, ID3D12Resource *color,
                      ID3D12Resource *output, uint64_t value, bool reset)
{
    veyra::Status status = veyra::Status::Ok;
    reset = reset || !history;
    if (!reset) {
        auto *queue = commands.queue();
        const auto flow_value = flow.nextOutValue();
        if (FAILED(queue->Signal(flow_input_ready.Get(), value)) || !flow.execute(value, status) ||
            FAILED(queue->Wait(flow_done.Get(), flow_value)))
            return false;
    }
    uint32_t slot;
    auto *list = commands.acquireNext(slot, status);
    if (!list)
        return false;
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    for (auto *texture : {vectors.Get(), cost.Get(), previous.Get(), current.Get()})
        states.transition(list, texture, read);
    states.transition(list, motion.Get(), write);
    states.transition(list, depth.Get(), write);
    const struct {
        uint32_t width, height, grid, reset;
        float scale_x, scale_y;
    } constants{width, height, grid, uint32_t(reset), scale_x, scale_y};
    std::array<float, 6> words{};
    std::memcpy(words.data(), &constants, sizeof(constants));
    densify.bind(list, words.data(), gpuHandleOf(densify, 0).ptr, gpuHandleOf(densify, 4).ptr);
    list->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    states.transition(list, motion.Get(), read);
    states.transition(list, depth.Get(), read);
    states.transition(list, color, read);
    states.transition(list, output, write);
    const veyra::ngx::DlssSrBackend::EvalDesc evaluate{color, output, depth.Get(), motion.Get(), reset, 0, 0, 0};
    if (!sr.evaluate(list, parameters, evaluate, status)) {
        commands.discardRecording();
        return false;
    }
    states.transition(list, current.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    states.transition(list, previous.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(previous.Get(), current.Get());
    for (auto *texture : {vectors.Get(), cost.Get(), previous.Get(), current.Get(), color, output})
        states.transition(list, texture, common);
    history = commands.submitAndSignal(slot);
    return history;
}
