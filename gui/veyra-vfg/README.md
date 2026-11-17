# Windows video enhancement

Chiaki maps the Vulkan decoder's frames directly. The optional Windows provider uses shared D3D12 textures and timeline fences on the same NVIDIA adapter. It uses Veyra-NRVideo's DLSS Super Resolution and VFG backends. Optical flow is estimated from consecutive decoded frames, since Remote Play supplies no game-engine motion or depth buffers.

Spatial RTX Video SR and VFG can share a D3D12 queue. VFG consumes the upscaled real frame on that queue. Vulkan receives the upscaled real frame and the generated frame for presentation.

Build `gui/veyra-vfg` separately with MSVC x64. Set `VEYRA_SOURCE_DIR` to a Veyra-NRVideo v2.1.9 checkout and `CUDA_DRIVER_INCLUDE_DIR` to the CUDA driver headers. For DLSS SR, also set `DLSS_SDK_ROOT` and `NVOF_INCLUDE_DIR` to the DLSS SDK and NVIDIA Optical Flow SDK 5 interface headers. Put `chiaki-veyra-vfg.dll` beside `chiaki.exe` and place Veyra's `VfgShotChange.dxil` in the adjacent `shaders` directory. The model and SDK binaries are supplied separately by their respective projects.

| Option | Value |
| --- | --- |
| `CHIAKI_VFG` | `low`, `medium`, or `high` for 2× interpolation. Request a 30 fps source with `--source-fps 30` or the stream settings. |
| `CHIAKI_VFG_RUNTIME` | Absolute path to Veyra's `runtime/nvidia-vfg` directory. |
| `CHIAKI_DLSS_SR` | `K`, `M`, or `L` for 4/3 scaling on each axis. This is experimental. |
| `CHIAKI_RTX_VIDEO_SR` | `1` to `4` for spatial RTX Video Super Resolution on SDR input. Uses the same runtime directory as DLSS. |
| `CHIAKI_RTX_VIDEO_SCALE` | `2` to double both axes for RTX Video SR, giving 3840×2160 from 1920×1080. |
| `CHIAKI_DLSS_RUNTIME` | Absolute path to Veyra's `runtime/experimental` directory. The existing NGX configuration is read from `runtime/config/ngx-local.json`. |
| `CHIAKI_VRR` | `1` to use synchronized Vulkan presentation when the NVIDIA driver reports VRR is possible on the active output. Activation is controlled by the driver and display. |
| `CHIAKI_STREAM_CLEANUP` | `1` for conservative spatial deblocking and debanding of stream video. |
| `CHIAKI_DIRECT_PRESENT_IMMEDIATE` | `1` to skip the extra software wait on newly decoded frames on the supported Vulkan path. |
| `--borderless-fullscreen` | Fill the current monitor with a borderless window. `--borderless-window` opens a smaller centered window. |

For timing runs, use `CHIAKI_VERBOSE_LOG_NOISE=1` with verbose logging. The latency entries end at swapchain submission. On a 30 fps source, the real frame is scheduled about 16.7 ms after decode with 2× interpolation. An optional `CHIAKI_FRAME_SAMPLE_DIR` pointing to an existing directory writes one comparison from the GPU textures. Leave it unset when recording timings.

Press **Ctrl+H** during Windows Vulkan streaming to open HDR calibration. With HEVC HDR and a PQ/BT.2020 swapchain, the panel applies changes live. Peak `0` and black `-1` use the active Windows display's reported luminance. The black setting is in millinits. Spline supports a midtone contrast adjustment. BT.2390 uses its reference knee offset. Display gamut mapping uses the reported primaries and white point when available. The swapchain continues to carry PQ in BT.2020.
