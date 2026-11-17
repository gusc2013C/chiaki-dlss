#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <memory>

class QString;

class WindowsVideoEnhancer {
public:
    enum class Mode { SuperResolution, FrameGeneration };
    static bool requested(Mode mode);
    static int scaledExtent(int source_extent);
    WindowsVideoEnhancer(pl_vulkan vulkan, PFN_vkGetInstanceProcAddr get_instance_proc, Mode mode,
                         bool prefer_combined = false);
    ~WindowsVideoEnhancer();
    WindowsVideoEnhancer(const WindowsVideoEnhancer &) = delete;
    WindowsVideoEnhancer &operator=(const WindowsVideoEnhancer &) = delete;

    bool prepare(int width, int height, const pl_color_space &encoded_color);
    bool generatesFrames() const;
    // Frames are borrowed until the next process call. Without SR the caller
    // presents the original real frame and the optional generated midpoint.
    pl_frame process(const pl_frame &source, const pl_color_space &encoded_color, bool reset,
                     pl_frame *generated = nullptr);
    static bool saveSample(pl_gpu gpu, const pl_frame &source, const QString &path,
                           int width, int height,
                           const pl_render_params *params = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
