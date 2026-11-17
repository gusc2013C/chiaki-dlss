cmake_minimum_required(VERSION 3.21)
project(chiaki_veyra_vfg LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build the Windows x64 provider with MSVC")
endif()

set(VEYRA_SOURCE_DIR "" CACHE PATH "Veyra-NRVideo v2.1.9 checkout")
set(CUDA_DRIVER_INCLUDE_DIR "" CACHE PATH "NVIDIA CUDA driver headers")
if(NOT EXISTS "${VEYRA_SOURCE_DIR}/src/pipeline/VfgBackend.cpp" OR
   NOT EXISTS "${CUDA_DRIVER_INCLUDE_DIR}/cuda.h")
    message(FATAL_ERROR "Set VEYRA_SOURCE_DIR and CUDA_DRIVER_INCLUDE_DIR")
endif()

add_library(chiaki-veyra-vfg SHARED veyraVfg.cpp
    "${VEYRA_SOURCE_DIR}/src/pipeline/VfgBackend.cpp"
    "${VEYRA_SOURCE_DIR}/src/pipeline/VfgAmpereCompat.cpp"
    "${VEYRA_SOURCE_DIR}/src/core/SceneCadenceAnalyzer.cpp"
    "${VEYRA_SOURCE_DIR}/src/gfx/D3D12DeviceContext.cpp"
    "${VEYRA_SOURCE_DIR}/src/gfx/CommandSlotRing.cpp"
    "${VEYRA_SOURCE_DIR}/src/base/FileIdentity.cpp"
    "${VEYRA_SOURCE_DIR}/src/base/RuntimePaths.cpp"
    "${VEYRA_SOURCE_DIR}/src/base/Log.cpp"
    "${VEYRA_SOURCE_DIR}/src/base/NgxResult.cpp")
target_compile_features(chiaki-veyra-vfg PRIVATE cxx_std_20)
target_compile_options(chiaki-veyra-vfg PRIVATE /utf-8)
target_compile_definitions(chiaki-veyra-vfg PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX VEYRA_HAVE_CUDA_DRIVER_HEADERS=1)
target_include_directories(chiaki-veyra-vfg PRIVATE ../include
    "${VEYRA_SOURCE_DIR}/include" "${CUDA_DRIVER_INCLUDE_DIR}")
target_link_libraries(chiaki-veyra-vfg PRIVATE d3d12 dxgi d3dcompiler dxguid advapi32)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK checkout")
set(NVOF_INCLUDE_DIR "" CACHE PATH "NVIDIA Optical Flow SDK 5 interface headers")
if(DLSS_SDK_ROOT OR NVOF_INCLUDE_DIR)
    if(NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx.h" OR
       NOT EXISTS "${NVOF_INCLUDE_DIR}/nvOpticalFlowD3D12.h")
        message(FATAL_ERROR "DLSS super resolution requires both DLSS_SDK_ROOT and NVOF_INCLUDE_DIR")
    endif()
    set_property(TARGET chiaki-veyra-vfg PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_sources(chiaki-veyra-vfg PRIVATE veyraSr.cpp
        "${VEYRA_SOURCE_DIR}/src/ngx/DlssSrBackend.cpp"
        "${VEYRA_SOURCE_DIR}/src/ngx/NgxCoreHost.cpp"
        "${VEYRA_SOURCE_DIR}/src/ngx/NvOfSession.cpp")
    target_sources(chiaki-veyra-vfg PRIVATE "${VEYRA_SOURCE_DIR}/src/ngx/VideoSrBackend.cpp")
    target_compile_definitions(chiaki-veyra-vfg PRIVATE CHIAKI_VEYRA_SR=1 VEYRA_HAS_NVOF=1)
    target_include_directories(chiaki-veyra-vfg PRIVATE "${DLSS_SDK_ROOT}/include" "${NVOF_INCLUDE_DIR}")
    target_link_libraries(chiaki-veyra-vfg PRIVATE
        "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s$<$<CONFIG:Debug>:_dbg>.lib")
endif()
