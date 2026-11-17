# chiaki-dlss Windows PS5 preset

The preset requests 1920×1080 HEVC at 50 Mbps for local PS5 Remote Play. It uses Vulkan with hardware decoder selection, direct frame mapping, zero-copy, VRR, and IDR recovery after FEC failure. It applies video values to the active Chiaki profile. Existing console registrations and account settings stay in that profile.

| Preset | PS5 request | Client processing |
| --- | --- | --- |
| `RtxVideo4K60` (default) | HEVC SDR, 30 fps | RTX Video quality 4 to 3840×2160, low VFG at 2× |
| `RtxVideo4K` | HEVC SDR, 30 fps | RTX Video quality 4 to 3840×2160 |
| `Native` | HEVC SDR, 60 fps | Direct presentation |
| `Hdr60` | HEVC HDR10, 30 fps | PQ and display gamut mapping, low VFG at 2× |

## Apply the preset

Extract the ZIP and open PowerShell in the extracted folder. Use Windows PowerShell 5.1 or newer. If Windows marked the script as downloaded, run `Unblock-File -LiteralPath .\Apply-ChiakiPreset.ps1` for this file.

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File .\Apply-ChiakiPreset.ps1
```

Use `-WhatIf` to inspect the values. The command writes the Windows user registry values. To apply the enhancement and VRR environment flags, launch the executable through the same script with the options below. The HDR profile uses the tested 1400-nit peak, automatic black level, Spline, and display gamut mapping. Add `-AutoHdrPeak` to use the active display's reported peak. Press **Ctrl+H** during HDR10 streaming to tune these values live.

## Launch chiaki-dlss with the preset

Use a Windows build of chiaki-dlss with the video enhancement provider. The RTX Video and VFG modes also need the Veyra-NRVideo runtime. Get the executable, provider, and shader layout from the [build instructions](https://github.com/gusc2013C/chiaki-dlss/tree/codex/chiaki-low-latency/gui/veyra-vfg). Pair your own PS5 in Chiaki before using direct connection.

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File .\Apply-ChiakiPreset.ps1 `
  -Preset RtxVideo4K60 -ChiakiExe 'D:\Apps\chiaki-dlss\chiaki.exe' `
  -VeyraRuntimeDir 'D:\Apps\Veyra\runtime'
```

Substitute the paths on your PC. Choose your console in the Chiaki window. Add `-ConsoleNickname` and `-ConsoleAddress` with your own registered console's details to open the stream directly in borderless fullscreen. The launcher passes rendering flags to this process and leaves global environment variables alone.
