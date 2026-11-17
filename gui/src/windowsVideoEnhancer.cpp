#include "windowsVideoEnhancer.h"
#include "veyraVfgApi.h"

#include <vulkan/vulkan_win32.h>

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QtAlgorithms>

#include <cstring>

namespace {
int videoSrQuality()
{
    const int quality = qEnvironmentVariableIntValue("CHIAKI_RTX_VIDEO_SR");
    return quality >= 1 && quality <= 4 ? quality : 0;
}

struct SharedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    pl_tex texture = nullptr;
};

pl_frame asRgbFrame(pl_tex texture, const pl_color_space &color)
{
    pl_frame frame{};
    frame.num_planes = 1;
    frame.planes[0].texture = texture;
    frame.planes[0].components = 3;
    for (int channel = 0; channel < 3; ++channel)
        frame.planes[0].component_mapping[channel] = channel;
    frame.planes[0].component_mapping[3] = -1;
    frame.repr = pl_color_repr_rgb;
    frame.color = color;
    frame.crop = {0, 0, float(texture->params.w), float(texture->params.h)};
    return frame;
}
}

struct WindowsVideoEnhancer::Impl {
    const pl_vulkan vulkan;
    const PFN_vkGetInstanceProcAddr instance_proc;
    const bool upscale;
    const int spatial_quality;
    const bool prefer_combined;
    QLibrary library;
    const VeyraVfgApi *api = nullptr;
    void *processor = nullptr;
    pl_renderer converter = nullptr;
    SharedImage input, output, flow_input, midpoint;
    VkSemaphore released = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
    uint64_t sequence = 0;
    uint64_t processed = 0;
    bool input_written = false;
    bool output_readable = false;
    bool midpoint_readable = false;
    bool failed = false;
    int width = 0, height = 0;
    bool hdr10 = false;

    struct {
        PFN_vkCreateImage create_image{};
        PFN_vkGetImageMemoryRequirements memory_requirements{};
        PFN_vkGetMemoryWin32HandlePropertiesKHR handle_properties{};
        PFN_vkAllocateMemory allocate_memory{};
        PFN_vkBindImageMemory bind_memory{};
        PFN_vkCreateSemaphore create_semaphore{};
        PFN_vkImportSemaphoreWin32HandleKHR import_semaphore{};
        PFN_vkDestroyImage destroy_image{};
        PFN_vkFreeMemory free_memory{};
        PFN_vkDestroySemaphore destroy_semaphore{};
    } vk;

    Impl(pl_vulkan device, PFN_vkGetInstanceProcAddr get_proc, Mode mode, bool combine)
        : vulkan(device), instance_proc(get_proc), upscale(mode == Mode::SuperResolution),
          spatial_quality(upscale ? videoSrQuality() : 0), prefer_combined(combine && spatial_quality)
    {}

    ~Impl() { clear(); }

    void clear()
    {
        if (input.image || output.image || released || done)
            pl_gpu_finish(vulkan->gpu);
        if (processor)
            api->destroy(processor);
        processor = nullptr;
        pl_renderer_destroy(&converter);
        for (auto *image : {&input, &output, &flow_input, &midpoint}) {
            pl_tex_destroy(vulkan->gpu, &image->texture);
            if (image->image)
                vk.destroy_image(vulkan->device, image->image, nullptr);
            if (image->memory)
                vk.free_memory(vulkan->device, image->memory, nullptr);
            *image = {};
        }
        if (released)
            vk.destroy_semaphore(vulkan->device, released, nullptr);
        if (done)
            vk.destroy_semaphore(vulkan->device, done, nullptr);
        released = done = VK_NULL_HANDLE;
        sequence = processed = 0;
        input_written = output_readable = midpoint_readable = false;
    }

    bool loadDeviceFunctions()
    {
        if (!instance_proc || vulkan->queue_graphics.index != vulkan->queue_compute.index ||
            vulkan->queue_graphics.index != vulkan->queue_transfer.index)
            return false;
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            instance_proc(vulkan->instance, "vkGetDeviceProcAddr"));
        if (!get_device_proc)
            return false;
#define LOAD(field, name) \
        vk.field = reinterpret_cast<PFN_##name>(get_device_proc(vulkan->device, #name)); \
        if (!vk.field) return false
        LOAD(create_image, vkCreateImage);
        LOAD(memory_requirements, vkGetImageMemoryRequirements);
        LOAD(handle_properties, vkGetMemoryWin32HandlePropertiesKHR);
        LOAD(allocate_memory, vkAllocateMemory);
        LOAD(bind_memory, vkBindImageMemory);
        LOAD(create_semaphore, vkCreateSemaphore);
        LOAD(import_semaphore, vkImportSemaphoreWin32HandleKHR);
        LOAD(destroy_image, vkDestroyImage);
        LOAD(free_memory, vkFreeMemory);
        LOAD(destroy_semaphore, vkDestroySemaphore);
#undef LOAD
        return true;
    }

    bool importImage(SharedImage &shared, HANDLE handle, VkFormat format, VkImageUsageFlags usage,
                     int image_width, int image_height)
    {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = {uint32_t(image_width), uint32_t(image_height), 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.usage = usage;
        if (vk.create_image(vulkan->device, &image, nullptr, &shared.image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements{};
        vk.memory_requirements(vulkan->device, shared.image, &requirements);
        VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (vk.handle_properties(vulkan->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                 handle, &properties) != VK_SUCCESS)
            return false;
        const uint32_t types = requirements.memoryTypeBits & properties.memoryTypeBits;
        if (!types)
            return false;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &import;
        dedicated.image = shared.image;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &dedicated;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = qCountTrailingZeroBits(types);
        if (vk.allocate_memory(vulkan->device, &allocate, nullptr, &shared.memory) != VK_SUCCESS ||
            vk.bind_memory(vulkan->device, shared.image, shared.memory, 0) != VK_SUCCESS)
            return false;
        pl_vulkan_wrap_params wrap{};
        wrap.image = shared.image;
        wrap.width = image_width;
        wrap.height = image_height;
        wrap.format = format;
        wrap.usage = usage;
        shared.texture = pl_vulkan_wrap(vulkan->gpu, &wrap);
        return shared.texture;
    }

    bool importFence(VkSemaphore &semaphore, HANDLE handle)
    {
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &timeline;
        if (vk.create_semaphore(vulkan->device, &info, nullptr, &semaphore) != VK_SUCCESS)
            return false;
        VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        import.semaphore = semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        return vk.import_semaphore(vulkan->device, &import) == VK_SUCCESS;
    }

    bool initialize()
    {
        const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            instance_proc(vulkan->instance, "vkGetPhysicalDeviceProperties2"));
        if (!get_properties || !loadDeviceFunctions())
            return false;
        VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &identity;
        get_properties(vulkan->phys_device, &properties);
        if (!identity.deviceLUIDValid || properties.properties.vendorID != 0x10de)
            return false;
        const auto runtime = qEnvironmentVariable(upscale ? "CHIAKI_DLSS_RUNTIME" : "CHIAKI_VFG_RUNTIME");
        if (!QFileInfo(runtime).isAbsolute())
            return false;
        library.setFileName(QCoreApplication::applicationDirPath() + "/chiaki-veyra-vfg.dll");
        const auto entry = reinterpret_cast<VeyraVfgGetApi>(library.resolve("chiaki_veyra_vfg_get_api"));
        if (!entry || !(api = entry(VeyraVfgAbi)))
            return false;

        const auto directory = runtime.toStdWString();
        const auto mode = qEnvironmentVariable(upscale ? "CHIAKI_DLSS_SR" : "CHIAKI_VFG").toUpper();
        VeyraVfgConfig config{};
        config.width = uint32_t(width);
        config.height = uint32_t(height);
        config.quality = upscale ? (mode == "L" ? 2u : mode == "M" ? 1u : 0u)
                                 : (mode == "LOW" ? 0u : mode == "HIGH" ? 2u : 1u);
        config.hdr10 = uint32_t(hdr10);
        config.video_sr_quality = uint32_t(spatial_quality);
        config.runtime_directory = directory.c_str();
        QByteArray project_id, version;
        if (upscale) {
            QFile settings(runtime + "/../config/ngx-local.json");
            if (!settings.open(QIODevice::ReadOnly))
                return false;
            const auto ngx = QJsonDocument::fromJson(settings.readAll()).object();
            project_id = ngx.value("ngxProjectId").toString().toUtf8();
            version = ngx.value("engineVersion").toString().toUtf8();
            if (project_id.isEmpty() || version.isEmpty())
                return false;
            config.project_id = project_id.constData();
            config.engine_version = version.constData();
            config.output_width = uint32_t(WindowsVideoEnhancer::scaledExtent(width));
            config.output_height = uint32_t(WindowsVideoEnhancer::scaledExtent(height));
        }
        static_assert(sizeof(config.adapter_luid) == VK_LUID_SIZE);
        std::memcpy(&config.adapter_luid, identity.deviceLUID, VK_LUID_SIZE);
        VeyraVfgShared handles{};
        if (prefer_combined) {
            const auto vfg_directory = qEnvironmentVariable("CHIAKI_VFG_RUNTIME");
            if (QFileInfo(vfg_directory).isAbsolute()) {
                const auto path = vfg_directory.toStdWString();
                const auto quality = qEnvironmentVariable("CHIAKI_VFG");
                config.video_sr_vfg_runtime = path.c_str();
                config.video_sr_vfg_quality = quality == "low" ? 0u : quality == "high" ? 2u : 1u;
                processor = api->create(&config, &handles);
                config.video_sr_vfg_runtime = nullptr;
            }
        }
        if (!processor)
            processor = api->create(&config, &handles);
        if (!processor)
            return false;

        const VkFormat format = upscale && !spatial_quality ? VK_FORMAT_R16G16B16A16_SFLOAT
            : hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
        constexpr auto renderable = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const int out_width = upscale ? int(config.output_width) : width;
        const int out_height = upscale ? int(config.output_height) : height;
        if (!importImage(input, handles.input, format, renderable, width, height) ||
            (upscale && !importImage(output, handles.output, format, VK_IMAGE_USAGE_SAMPLED_BIT,
                                    out_width, out_height)) ||
            (upscale && !spatial_quality && !importImage(flow_input, handles.flow_input, VK_FORMAT_B8G8R8A8_UNORM,
                                     renderable, width, height)) ||
            (handles.midpoint && !importImage(midpoint, handles.midpoint, format, VK_IMAGE_USAGE_SAMPLED_BIT,
                                               out_width, out_height)) ||
            !importFence(released, handles.input_released) || !importFence(done, handles.processing_done))
            return false;
        converter = pl_renderer_create(nullptr, vulkan->gpu);
        if (!converter)
            return false;
        qInfo().nospace() << (upscale && handles.midpoint ? "RTX Video spatial SR + Veyra VFG 2x (shared D3D12 queue)" :
                             spatial_quality ? "RTX Video spatial SR" :
                             upscale ? "Veyra DLSS temporal SR" : "Veyra VFG 2x")
                          << " initialized on decoded GPU frames: " << width << 'x' << height
                          << " -> " << out_width << 'x' << out_height
                          << " HDR10=" << hdr10 << " quality="
                          << (spatial_quality ? config.video_sr_quality : config.quality);
        return true;
    }

    bool hold(pl_tex texture)
    {
        pl_vulkan_hold_params hold{};
        hold.tex = texture;
        hold.layout = VK_IMAGE_LAYOUT_GENERAL;
        hold.qf = VK_QUEUE_FAMILY_EXTERNAL;
        hold.semaphore = {released, ++sequence};
        return pl_vulkan_hold_ex(vulkan->gpu, &hold);
    }

    void release(pl_tex texture, bool initialized, uint64_t fence)
    {
        pl_vulkan_release_params release{};
        release.tex = texture;
        release.layout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        release.qf = initialized ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
        if (fence)
            release.semaphore = {done, fence};
        pl_vulkan_release_ex(vulkan->gpu, &release);
    }

    bool prepare(int source_width, int source_height, const pl_color_space &encoded_color)
    {
        const bool source_hdr = pl_color_space_is_hdr(&encoded_color);
        if (failed || source_width <= 0 || source_height <= 0 ||
            (source_hdr && (spatial_quality || encoded_color.transfer != PL_COLOR_TRC_PQ)))
            return false;
        if (!processor || width != source_width || height != source_height || hdr10 != source_hdr) {
            clear();
            width = source_width;
            height = source_height;
            hdr10 = source_hdr;
            if (!initialize()) {
                fail("initialization");
                return false;
            }
        }
        return true;
    }

    pl_frame process(const pl_frame &source, const pl_color_space &encoded_color, bool reset,
                     pl_frame *generated)
    {
        if (generated)
            *generated = {};
        if (!source.num_planes || !source.planes[0].texture ||
            !prepare(int(pl_rect_w(source.crop)), int(pl_rect_h(source.crop)), encoded_color))
            return {};

        if (output_readable) {
            if (!hold(output.texture))
                return fail("releasing the previous interpolated image");
            output_readable = false;
        }
        if (midpoint_readable) {
            if (!hold(midpoint.texture))
                return fail("releasing the previous midpoint");
            midpoint_readable = false;
        }
        release(input.texture, input_written, processed);
        auto color = encoded_color;
        if (spatial_quality)
            color = pl_color_space_srgb;
        else if (upscale)
            color.transfer = PL_COLOR_TRC_LINEAR;
        auto target = asRgbFrame(input.texture, color);
        if (!pl_render_image(converter, &source, &target, &pl_render_fast_params))
            return fail("converting the decoded image");
        if (upscale && !spatial_quality) {
            release(flow_input.texture, input_written, processed);
            auto motion_target = asRgbFrame(flow_input.texture, pl_color_space_srgb);
            if (!pl_render_image(converter, &source, &motion_target, &pl_render_fast_params) ||
                !hold(flow_input.texture))
                return fail("preparing optical flow");
        }
        if (!hold(input.texture))
            return fail("releasing the decoded image to D3D12");
        input_written = true;
        pl_gpu_flush(vulkan->gpu);

        const auto result = api->submit(processor, sequence, reset);
        if (result == VeyraVfgResult::Failed)
            return fail("dispatching the model");
        processed = sequence;
        if (midpoint.texture && generated && result == VeyraVfgResult::OutputReady) {
            release(midpoint.texture, true, processed);
            midpoint_readable = true;
            *generated = asRgbFrame(midpoint.texture, color);
        }
        if (!upscale)
            return {};
        release(output.texture, true, processed);
        output_readable = true;
        return asRgbFrame(output.texture, color);
    }

    pl_frame fail(const char *action)
    {
        failed = true;
        qWarning() << (upscale ? "Veyra DLSS SR" : "Veyra VFG")
                   << "fell back to native video while" << action;
        return {};
    }
};

bool WindowsVideoEnhancer::requested(Mode mode)
{
    static const bool enabled = [] {
        const auto mode = qEnvironmentVariable("CHIAKI_VFG");
        return mode == "low" || mode == "medium" || mode == "high";
    }();
    static const bool sr = [] {
        const auto model = qEnvironmentVariable("CHIAKI_DLSS_SR").toUpper();
        return model == "1" || model == "K" || model == "M" || model == "L" || videoSrQuality() != 0;
    }();
    return mode == Mode::SuperResolution ? sr : enabled;
}

int WindowsVideoEnhancer::scaledExtent(int source_extent)
{
    const bool double_size = videoSrQuality() && qEnvironmentVariableIntValue("CHIAKI_RTX_VIDEO_SCALE") == 2;
    return double_size ? source_extent * 2 : source_extent * 4 / 3;
}

WindowsVideoEnhancer::WindowsVideoEnhancer(pl_vulkan vulkan, PFN_vkGetInstanceProcAddr get_instance_proc, Mode mode,
                                           bool prefer_combined)
    : impl(std::make_unique<Impl>(vulkan, get_instance_proc, mode, prefer_combined))
{}

WindowsVideoEnhancer::~WindowsVideoEnhancer() = default;

bool WindowsVideoEnhancer::prepare(int width, int height, const pl_color_space &encoded_color)
{
    return impl->prepare(width, height, encoded_color);
}

bool WindowsVideoEnhancer::generatesFrames() const
{
    return impl->midpoint.texture != nullptr;
}

pl_frame WindowsVideoEnhancer::process(const pl_frame &source, const pl_color_space &encoded_color, bool reset,
                                      pl_frame *generated)
{
    return impl->process(source, encoded_color, reset, generated);
}

bool WindowsVideoEnhancer::saveSample(pl_gpu gpu, const pl_frame &source, const QString &path,
                                     int width, int height,
                                     const pl_render_params *render_params)
{
    pl_tex_params params{};
    params.w = width;
    params.h = height;
    params.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
        pl_fmt_caps(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
    params.renderable = params.host_readable = true;
    if (!params.format)
        return false;
    auto texture = pl_tex_create(gpu, &params);
    if (!texture)
        return false;
    auto renderer = pl_renderer_create(nullptr, gpu);
    auto target = asRgbFrame(texture, pl_color_space_srgb);
    QImage image(width, height, QImage::Format_RGBA8888);
    pl_tex_transfer_params transfer{};
    transfer.tex = texture;
    transfer.ptr = image.bits();
    transfer.row_pitch = size_t(image.bytesPerLine());
    const bool saved = renderer && !image.isNull() &&
        pl_render_image(renderer, &source, &target, render_params ? render_params : &pl_render_fast_params) &&
        pl_tex_download(gpu, &transfer) && image.save(path, "PNG");
    pl_renderer_destroy(&renderer);
    pl_tex_destroy(gpu, &texture);
    return saved;
}
