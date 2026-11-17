#include "veyraVfgApi.h"

#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/Log.h"
#include "veyra/pipeline/VfgBackend.h"
#ifdef CHIAKI_VEYRA_SR
#include "veyraSr.h"
#include "veyra/ngx/VideoSrBackend.h"
#endif

#include <memory>
#include <utility>

namespace {
using veyra::gfx::ComPtr;

struct SharedHandle {
    HANDLE value = nullptr;
    ~SharedHandle() { if (value) CloseHandle(value); }
};

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *image,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

class Processor {
    veyra::gfx::D3D12DeviceContext device;
    veyra::gfx::CommandSlotRing commands;
    veyra::pipeline::VfgBackend vfg;
    ComPtr<ID3D12Resource> input, output, midpoint;
    ComPtr<ID3D12Fence> input_released, processing_done;
    SharedHandle input_handle, output_handle, released_handle, done_handle, flow_handle, midpoint_handle;
#ifdef CHIAKI_VEYRA_SR
    std::unique_ptr<VeyraSr> sr;
    veyra::ngx::NgxCoreHost video_core;
    veyra::ngx::VideoSrBackend video_sr;
    unsigned video_quality = 0;
#endif
    unsigned parity = 0;
    bool has_history = false;

public:
    ~Processor()
    {
        if (commands.initialized())
            commands.drainQueue();
#ifdef CHIAKI_VEYRA_SR
        sr.reset();
        video_sr.release();
#endif
        vfg.shutdown();
    }

    bool initialize(const VeyraVfgConfig &config, VeyraVfgShared &shared)
    {
        if (!config.adapter_luid || !config.width || !config.height || !config.runtime_directory)
            return false;
        veyra::Status status = veyra::Status::Ok;
        veyra::gfx::DeviceContextDesc device_config{};
        device_config.requiredLuid = config.adapter_luid;
        device_config.requiredVendorId = 0x10de;
        if (!device.initialize(device_config, status) ||
            !commands.initialize(device.device(), device.directQueue(), device.fence(),
                                 device.fenceEvent(), device.commandSlotCount(), status))
            return false;

        const bool upscale = config.output_width > config.width && config.output_height > config.height;
        if (config.video_sr_quality && (!upscale || config.hdr10 || config.video_sr_quality > 4))
            return false;
        const auto format = upscale && !config.video_sr_quality ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : config.hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr auto render_target = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!createImage(input, config.width, config.height, format, render_target) ||
            (upscale && !createImage(output, config.output_width, config.output_height, format,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
            FAILED(device.device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&input_released))) ||
            FAILED(device.device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&processing_done))) ||
            !share(input.Get(), input_handle) || (upscale && !share(output.Get(), output_handle)) ||
            !share(input_released.Get(), released_handle) || !share(processing_done.Get(), done_handle))
            return false;
        if (upscale) {
#ifdef CHIAKI_VEYRA_SR
            if (config.video_sr_quality) {
                if (!initializeVideoSr(config, status))
                    return false;
            } else {
                sr = std::make_unique<VeyraSr>();
                ComPtr<ID3D12Resource> flow_input;
                if (!createImage(flow_input, config.width, config.height, DXGI_FORMAT_B8G8R8A8_UNORM, render_target) ||
                    !share(flow_input.Get(), flow_handle) || !sr->initialize(config, device, commands, std::move(flow_input)))
                    return false;
            }
#else
            return false;
#endif
        }
        if (!upscale || config.video_sr_vfg_runtime) {
            const auto width = upscale ? config.output_width : config.width;
            const auto height = upscale ? config.output_height : config.height;
            const auto *runtime = upscale ? config.video_sr_vfg_runtime : config.runtime_directory;
            const auto quality = upscale ? config.video_sr_vfg_quality : config.quality;
            if (!createImage(midpoint, width, height, format, D3D12_RESOURCE_FLAG_NONE) ||
                !share(midpoint.Get(), midpoint_handle) ||
                !vfg.initialize(device, runtime, width, height, format, 2, quality, false))
                return false;
        }
        shared = {input_handle.value, output_handle.value, released_handle.value, done_handle.value,
                  flow_handle.value, midpoint_handle.value};
        return true;
    }

    VeyraVfgResult submit(uint64_t value, bool reset)
    {
        auto *queue = device.directQueue();
        if (FAILED(queue->Wait(input_released.Get(), value)))
            return VeyraVfgResult::Failed;
        VeyraVfgResult result;
#ifdef CHIAKI_VEYRA_SR
        if (video_quality && !midpoint) {
            veyra::Status status = veyra::Status::Ok;
            uint32_t slot;
            auto *list = commands.acquireNext(slot, status);
            if (!list)
                return VeyraVfgResult::Failed;
            if (!recordVideoSr(list)) {
                commands.discardRecording();
                return VeyraVfgResult::Failed;
            }
            if (!commands.submitAndSignal(slot))
                return VeyraVfgResult::Failed;
            result = VeyraVfgResult::OutputReady;
        } else if (sr) {
            if (!sr->process(commands, input.Get(), output.Get(), value, reset))
                return VeyraVfgResult::Failed;
            result = VeyraVfgResult::OutputReady;
        } else
#endif
        {
            result = generateMidpoint(reset);
        }
        if (result == VeyraVfgResult::Failed || FAILED(queue->Signal(processing_done.Get(), value)))
            return VeyraVfgResult::Failed;
        return result;
    }

private:
    VeyraVfgResult generateMidpoint(bool reset)
    {
        veyra::Status status = veyra::Status::Ok;
        uint32_t slot;
        auto *list = commands.acquireNext(slot, status);
        if (!list)
            return VeyraVfgResult::Failed;
        auto *real = input.Get();
#ifdef CHIAKI_VEYRA_SR
        if (video_quality) {
            // Keeping SR and capture in the same list removes a queue submission.
            if (!recordVideoSr(list)) {
                commands.discardRecording();
                return VeyraVfgResult::Failed;
            }
            real = output.Get();
        }
#endif
        const bool interpolate = has_history && !reset;
        transition(list, real, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const bool captured = vfg.capture(list, real, parity, interpolate);
        transition(list, real, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!captured) {
            commands.discardRecording();
            return VeyraVfgResult::Failed;
        }
        if (!commands.submitAndSignal(slot))
            return VeyraVfgResult::Failed;

        if (interpolate) {
            if (!vfg.generate(parity, 1, 2, false))
                return VeyraVfgResult::Failed;
            list = commands.acquireNext(slot, status);
            if (!list)
                return VeyraVfgResult::Failed;
            auto *generated_frame = midpoint.Get();
            transition(list, generated_frame, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!vfg.copyOutput(list, generated_frame, parity)) {
                commands.discardRecording();
                return VeyraVfgResult::Failed;
            }
            transition(list, generated_frame, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (!commands.submitAndSignal(slot))
                return VeyraVfgResult::Failed;
        }
        has_history = true;
        parity ^= 1;
        return interpolate ? VeyraVfgResult::OutputReady : VeyraVfgResult::HistoryOnly;
    }

#ifdef CHIAKI_VEYRA_SR
    bool initializeVideoSr(const VeyraVfgConfig &config, veyra::Status &status)
    {
        if (!config.project_id || !config.engine_version ||
            !video_core.initialize(device.device(), config.runtime_directory,
                                   config.project_id, config.engine_version, status))
            return false;
        uint32_t slot;
        auto *commands_list = commands.acquireNext(slot, status);
        if (!commands_list)
            return false;
        if (!video_sr.create(video_core, commands_list, config.width, config.height,
                             config.output_width, config.output_height)) {
            commands.discardRecording();
            return false;
        }
        video_quality = config.video_sr_quality;
        return commands.submitAndSignal(slot) && commands.waitIdle();
    }

    bool recordVideoSr(ID3D12GraphicsCommandList *list)
    {
        if (!video_sr.evaluate(list, input.Get(), output.Get(), video_quality))
            return false;
        D3D12_RESOURCE_BARRIER output_ready{};
        output_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        output_ready.UAV.pResource = output.Get();
        list->ResourceBarrier(1, &output_ready);
        return true;
    }
#endif
    bool createImage(ComPtr<ID3D12Resource> &resource, uint32_t width, uint32_t height,
                     DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_RESOURCE_DESC image{};
        image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        image.Width = width;
        image.Height = height;
        image.DepthOrArraySize = 1;
        image.MipLevels = 1;
        image.Format = format;
        image.SampleDesc.Count = 1;
        image.Flags = flags;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        return SUCCEEDED(device.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &image,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    }

    bool share(ID3D12DeviceChild *resource, SharedHandle &handle)
    {
        return SUCCEEDED(device.device()->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr,
                                                               &handle.value));
    }
};

void *create(const VeyraVfgConfig *config, VeyraVfgShared *shared)
{
    if (!config || !shared)
        return nullptr;
    try {
        static const bool log_opened = [] {
            wchar_t path[4096]{};
            const auto size = GetEnvironmentVariableW(L"CHIAKI_ENHANCE_LOG", path, 4096);
            if (size && size < 4096)
                veyra::Logger::instance().openFile(path);
            return true;
        }();
        (void)log_opened;
        auto processor = std::make_unique<Processor>();
        return processor->initialize(*config, *shared) ? processor.release() : nullptr;
    } catch (...) {
        return nullptr;
    }
}

VeyraVfgResult submit(void *instance, uint64_t value, bool reset)
{
    try {
        return static_cast<Processor *>(instance)->submit(value, reset);
    } catch (...) {
        return VeyraVfgResult::Failed;
    }
}

void destroy(void *instance)
{
    delete static_cast<Processor *>(instance);
}
}

extern "C" __declspec(dllexport) const VeyraVfgApi *chiaki_veyra_vfg_get_api(uint32_t version)
{
    static const VeyraVfgApi api{create, submit, destroy};
    return version == VeyraVfgAbi ? &api : nullptr;
}
